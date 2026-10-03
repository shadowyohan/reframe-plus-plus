#include "rf/audio/WasapiCapture.h"

#include <audioclientactivationparams.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>

#include <wrl/implements.h>

#include "rf/core/Log.h"
#include "rf/core/Strings.h"
#include "rf/core/Time.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mmdevapi.lib")

using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::FtmBase;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;

namespace rf {
namespace {
constexpr REFERENCE_TIME kBufferDuration = 20 * 10'000;
constexpr DWORD kActivationTimeoutMs = 5'000;
constexpr DWORD kWakeMs = 40;
constexpr Ticks100ns kRetryEvery = 1'000 * 10'000;
constexpr Ticks100ns kLongestSilenceChunk = 100 * 10'000;
constexpr Ticks100ns kSilenceSlack = 10 * 10'000;

class ActivationWaiter
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, FtmBase,
                          IActivateAudioInterfaceCompletionHandler> {
public:
    ActivationWaiter() : done_(::CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~ActivationWaiter() override { ::CloseHandle(done_); }

    STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation*) override {
        ::SetEvent(done_);
        return S_OK;
    }

    bool Wait(DWORD timeout_ms) const {
        return ::WaitForSingleObject(done_, timeout_ms) == WAIT_OBJECT_0;
    }

private:
    HANDLE done_;
};

class DeviceWatcher
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, FtmBase, IMMNotificationClient> {
public:
    DeviceWatcher(WasapiCapture* owner, EDataFlow flow, ERole role, std::wstring preferred)
        : owner_(owner), flow_(flow), role_(role), preferred_(std::move(preferred)) {}

    STDMETHOD(OnDefaultDeviceChanged)(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == flow_ && role == role_) owner_->FollowDefaultDevice();
        return S_OK;
    }

    STDMETHOD(OnDeviceStateChanged)(LPCWSTR id, DWORD state) override {
        if (state == DEVICE_STATE_ACTIVE && IsPreferred(id)) owner_->ReopenDevice();
        return S_OK;
    }

    STDMETHOD(OnDeviceAdded)(LPCWSTR id) override {
        if (IsPreferred(id)) owner_->ReopenDevice();
        return S_OK;
    }

    STDMETHOD(OnDeviceRemoved)(LPCWSTR) override { return S_OK; }
    STDMETHOD(OnPropertyValueChanged)(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    bool IsPreferred(LPCWSTR id) const { return id && !preferred_.empty() && preferred_ == id; }

    WasapiCapture* owner_;
    EDataFlow flow_;
    ERole role_;
    std::wstring preferred_;
};

const char* SourceName(AudioSource source) {
    switch (source) {
        case AudioSource::SystemLoopback: return "loopback";
        case AudioSource::Microphone:     return "microphone";
        case AudioSource::Application:    return "application";
    }
    return "audio";
}

EDataFlow FlowOf(AudioSource source) {
    return source == AudioSource::SystemLoopback ? eRender : eCapture;
}

ERole RoleOf(AudioSource source) {
    return source == AudioSource::SystemLoopback ? eConsole : eCommunications;
}

}

std::uint32_t SilenceFramesOwed(Ticks100ns expected, Ticks100ns now, std::uint32_t sample_rate) {
    const Ticks100ns gap = now - expected;
    if (gap < kSilenceSlack) return 0;
    const Ticks100ns chunk = std::min(gap, kLongestSilenceChunk);
    return static_cast<std::uint32_t>(chunk * sample_rate / kOneSecond100ns);
}

WasapiCapture::~WasapiCapture() { Stop(); }

Status WasapiCapture::Start(AudioSource source, const AudioCallback& on_audio,
                            const std::string& device_id) {
    if (running_) return Status::Fail("audio capture already running");
    on_audio_ = on_audio;
    source_ = source;
    preferred_device_ = source == AudioSource::Microphone ? device_id : std::string{};

    RF_HR(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                             IID_PPV_ARGS(&enumerator_)));
    notifier_ = Make<DeviceWatcher>(this, FlowOf(source), RoleOf(source), ToWide(preferred_device_));
    if (FAILED(enumerator_->RegisterEndpointNotificationCallback(notifier_.Get()))) {
        RF_WARN("{}: device changes will not be followed", SourceName(source));
        notifier_.Reset();
    }

    event_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (auto s = OpenDevice(); !s.ok()) {
        RF_WARN("{} unavailable for now ({}) - writing silence and retrying", SourceName(source),
                s.str());
        lost_ = true;
        next_retry_ = Now100ns() + kRetryEvery;
    }
    StartThread();
    return Status::Ok();
}

Status WasapiCapture::StartApplication(std::uint32_t process_id, const AudioCallback& on_audio) {
    if (running_) return Status::Fail("audio capture already running");
    on_audio_ = on_audio;
    source_ = AudioSource::Application;

    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = process_id;
    params.ProcessLoopbackParams.ProcessLoopbackMode =
        PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT activation{};
    activation.vt = VT_BLOB;
    activation.blob.cbSize = sizeof(params);
    activation.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    auto handler = Make<ActivationWaiter>();
    ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
    RF_HR(::ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
                                        __uuidof(IAudioClient), &activation, handler.Get(),
                                        &operation));
    if (!handler->Wait(kActivationTimeoutMs))
        return Status::Fail("process loopback activation timed out");

    HRESULT activated = E_FAIL;
    ComPtr<IUnknown> client;
    RF_HR(operation->GetActivateResult(&activated, &client));
    if (FAILED(activated)) return Status::Fail(activated, "process loopback activation");
    RF_HR(client.As(&client_));

    event_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (auto s = InitializeClient(); !s.ok()) {
        CloseDevice();
        return s;
    }
    StartThread();
    return Status::Ok();
}

Status WasapiCapture::OpenDevice() {
    if (!preferred_device_.empty() &&
        FAILED(enumerator_->GetDevice(ToWide(preferred_device_).c_str(), &device_))) {
        RF_WARN("selected microphone is gone - using the default one");
        device_.Reset();
    }
    using_preferred_ = device_ != nullptr;
    if (!device_) RF_HR(enumerator_->GetDefaultAudioEndpoint(FlowOf(source_), RoleOf(source_), &device_));

    if (auto s = [&]() -> Status {
            RF_HR(device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_));
            return InitializeClient();
        }();
        !s.ok()) {
        CloseDevice();
        return s;
    }
    return Status::Ok();
}

Status WasapiCapture::InitializeClient() {
    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = 48'000;
    fmt.wBitsPerSample = 32;
    fmt.nBlockAlign = fmt.nChannels * fmt.wBitsPerSample / 8;
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
    format_.sample_rate = fmt.nSamplesPerSec;
    format_.channels = fmt.nChannels;
    format_.bits = 32;

    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                  AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    if (source_ != AudioSource::Microphone) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;

    const HRESULT hr =
        client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kBufferDuration, 0, &fmt, nullptr);
    if (FAILED(hr)) return Status::Fail(hr, "IAudioClient::Initialize");

    RF_HR(client_->SetEventHandle(event_));
    RF_HR(client_->GetService(IID_PPV_ARGS(&capture_)));
    RF_HR(client_->Start());

    RF_INFO("audio capture started: {} {} Hz {} ch", SourceName(source_), format_.sample_rate,
            format_.channels);
    return Status::Ok();
}

void WasapiCapture::CloseDevice() {
    if (client_) client_->Stop();
    capture_.Reset();
    client_.Reset();
    device_.Reset();
}

void WasapiCapture::StartThread() {
    if (format_.sample_rate == 0) {
        format_.sample_rate = 48'000;
        format_.channels = 2;
        format_.bits = 32;
    }
    silence_.assign(static_cast<std::size_t>(format_.sample_rate * kLongestSilenceChunk /
                                             kOneSecond100ns) *
                        format_.channels,
                    0.0f);
    expected_ts_ = Now100ns();
    running_ = true;
    thread_ = std::thread([this] { CaptureLoop(); });
}

void WasapiCapture::FollowDefaultDevice() {
    if (using_preferred_.load(std::memory_order_relaxed)) return;
    ReopenDevice();
}

void WasapiCapture::ReopenDevice() {
    if (source_ == AudioSource::Application) return;
    reopen_.store(true, std::memory_order_relaxed);
    if (event_) ::SetEvent(event_);
}

void WasapiCapture::Stop() {
    running_ = false;
    if (event_) ::SetEvent(event_);
    if (thread_.joinable()) thread_.join();
    if (enumerator_ && notifier_) enumerator_->UnregisterEndpointNotificationCallback(notifier_.Get());
    notifier_.Reset();
    CloseDevice();
    enumerator_.Reset();
    if (event_) {
        ::CloseHandle(event_);
        event_ = nullptr;
    }
}

void WasapiCapture::Deliver(const float* samples, std::uint32_t frames, Ticks100ns timestamp) {
    AudioChunk chunk;
    chunk.samples = samples;
    chunk.frames = frames;
    chunk.channels = format_.channels;
    chunk.sample_rate = format_.sample_rate;
    chunk.timestamp = timestamp;
    expected_ts_ = timestamp + static_cast<Ticks100ns>(frames) * kOneSecond100ns / format_.sample_rate;
    if (on_audio_) on_audio_(chunk);
}

void WasapiCapture::FillSilenceUntil(Ticks100ns now) {
    while (const std::uint32_t frames = SilenceFramesOwed(expected_ts_, now, format_.sample_rate)) {
        ++silence_filled_;
        Deliver(silence_.data(), frames, expected_ts_);
    }
}

void WasapiCapture::TryToRecover(Ticks100ns now) {
    if (source_ == AudioSource::Application || now < next_retry_) return;
    if (auto s = OpenDevice(); !s.ok()) {
        next_retry_ = now + kRetryEvery;
        return;
    }
    lost_ = false;
    RF_INFO("{} capture recovered after {} silence fills", SourceName(source_), silence_filled_);
}

void WasapiCapture::DrainPackets() {
    UINT32 packet = 0;
    HRESULT hr = S_OK;
    while (SUCCEEDED(hr = capture_->GetNextPacketSize(&packet)) && packet > 0) {
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        UINT64 position = 0;
        UINT64 qpc = 0;

        if (FAILED(hr = capture_->GetBuffer(&data, &frames, &flags, &position, &qpc))) break;

        const float* samples = reinterpret_cast<const float*>(data);
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            if (silence_.size() < static_cast<std::size_t>(frames) * format_.channels)
                silence_.assign(static_cast<std::size_t>(frames) * format_.channels, 0.0f);
            samples = silence_.data();
        }

        frames_captured_ += frames;
        Deliver(samples, frames, qpc ? static_cast<Ticks100ns>(qpc) : Now100ns());
        capture_->ReleaseBuffer(frames);
    }

    if (FAILED(hr)) {
        RF_WARN("{} capture lost its device (0x{:08X}) - writing silence until it comes back",
                SourceName(source_), static_cast<unsigned>(hr));
        CloseDevice();
        lost_ = true;
        next_retry_ = Now100ns();
    }
}

void WasapiCapture::CaptureLoop() {
    ::SetThreadDescription(::GetCurrentThread(), L"rf-audio-wasapi");
    MmcssScope mmcss(L"Pro Audio");

    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    while (running_.load(std::memory_order_relaxed)) {
        const DWORD wait = ::WaitForSingleObject(event_, kWakeMs);
        if (!running_) break;

        if (reopen_.exchange(false, std::memory_order_relaxed)) {
            RF_INFO("{}: the audio device changed - reopening", SourceName(source_));
            CloseDevice();
            lost_ = true;
            next_retry_ = 0;
        }

        const Ticks100ns now = Now100ns();
        if (lost_) TryToRecover(now);

        if (!lost_ && capture_) DrainPackets();

        const bool quiet = wait == WAIT_TIMEOUT && source_ != AudioSource::Microphone;
        if (lost_ || quiet) FillSilenceUntil(Now100ns());
    }

    ::CoUninitialize();
    RF_INFO("audio capture ended: {} frames, {} silence fills", frames_captured_, silence_filled_);
}

}
