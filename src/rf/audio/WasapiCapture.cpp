#include "rf/audio/WasapiCapture.h"

#include <functiondiscoverykeys_devpkey.h>

#include "rf/core/Log.h"
#include "rf/core/Strings.h"
#include "rf/core/Time.h"

#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

namespace rf {
namespace {
constexpr REFERENCE_TIME kBufferDuration = 20 * 10'000;
}

WasapiCapture::~WasapiCapture() { Stop(); }

Status WasapiCapture::Start(AudioSource source, const AudioCallback& on_audio,
                            const std::string& device_id) {
    if (running_) return Status::Fail("audio capture already running");
    source_ = source;
    on_audio_ = on_audio;

    ComPtr<IMMDeviceEnumerator> enumerator;
    RF_HR(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                             IID_PPV_ARGS(&enumerator)));

    const EDataFlow flow = (source == AudioSource::SystemLoopback) ? eRender : eCapture;
    if (source == AudioSource::Microphone && !device_id.empty()) {

        if (FAILED(enumerator->GetDevice(ToWide(device_id).c_str(), &device_))) {
            RF_WARN("selected microphone is gone - using the default one");
            device_.Reset();
        }
    }
    if (!device_) {
        const ERole role = (source == AudioSource::SystemLoopback) ? eConsole : eCommunications;
        RF_HR(enumerator->GetDefaultAudioEndpoint(flow, role, &device_));
    }

    RF_HR(device_->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, &client_));

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
    if (source == AudioSource::SystemLoopback) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;

    const HRESULT hr =
        client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kBufferDuration, 0, &fmt, nullptr);
    if (FAILED(hr)) return Status::Fail(hr, "IAudioClient::Initialize");

    event_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    RF_HR(client_->SetEventHandle(event_));
    RF_HR(client_->GetService(IID_PPV_ARGS(&capture_)));

    silence_.assign(static_cast<std::size_t>(format_.sample_rate) * format_.channels / 50, 0.0f);

    RF_HR(client_->Start());
    running_ = true;
    thread_ = std::thread([this] { CaptureLoop(); });

    RF_INFO("audio capture started: {} {} Hz {} ch",
            source == AudioSource::SystemLoopback ? "loopback" : "microphone", format_.sample_rate,
            format_.channels);
    return Status::Ok();
}

void WasapiCapture::Stop() {
    running_ = false;
    if (event_) ::SetEvent(event_);
    if (thread_.joinable()) thread_.join();
    if (client_) client_->Stop();
    if (event_) {
        ::CloseHandle(event_);
        event_ = nullptr;
    }
    capture_.Reset();
    client_.Reset();
    device_.Reset();
}

void WasapiCapture::CaptureLoop() {
    ::SetThreadDescription(::GetCurrentThread(), L"rf-audio-wasapi");
    MmcssScope mmcss(L"Pro Audio");

    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    while (running_.load(std::memory_order_relaxed)) {

        const DWORD wait = ::WaitForSingleObject(event_, 40);
        if (!running_) break;

        if (wait == WAIT_TIMEOUT && source_ == AudioSource::SystemLoopback) {
            AudioChunk chunk;
            chunk.samples = silence_.data();
            chunk.frames = static_cast<std::uint32_t>(silence_.size() / format_.channels);
            chunk.channels = format_.channels;
            chunk.sample_rate = format_.sample_rate;
            chunk.timestamp = Now100ns();
            ++silence_filled_;
            if (on_audio_) on_audio_(chunk);
            continue;
        }

        UINT32 packet = 0;
        while (SUCCEEDED(capture_->GetNextPacketSize(&packet)) && packet > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 position = 0;
            UINT64 qpc = 0;

            if (FAILED(capture_->GetBuffer(&data, &frames, &flags, &position, &qpc))) break;

            AudioChunk chunk;
            chunk.frames = frames;
            chunk.channels = format_.channels;
            chunk.sample_rate = format_.sample_rate;

            chunk.timestamp = qpc ? static_cast<Ticks100ns>(qpc) : Now100ns();

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                if (silence_.size() < static_cast<std::size_t>(frames) * format_.channels)
                    silence_.assign(static_cast<std::size_t>(frames) * format_.channels, 0.0f);
                chunk.samples = silence_.data();
            } else {
                chunk.samples = reinterpret_cast<const float*>(data);
            }

            frames_captured_ += frames;
            if (on_audio_) on_audio_(chunk);

            capture_->ReleaseBuffer(frames);
        }
    }

    ::CoUninitialize();
    RF_INFO("audio capture ended: {} frames, {} silence fills", frames_captured_, silence_filled_);
}

}
