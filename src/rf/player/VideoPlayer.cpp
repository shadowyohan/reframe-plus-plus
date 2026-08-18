#include "rf/player/VideoPlayer.h"

#include <mfapi.h>
#include <shlwapi.h>
#include <wininet.h>
#include <mfmediaengine.h>

#include <algorithm>

#include "rf/core/Log.h"
#include "rf/core/Strings.h"

using Microsoft::WRL::ComPtr;

namespace rf {

class MediaEngineNotify final : public IMFMediaEngineNotify {
public:
    explicit MediaEngineNotify(VideoPlayer* player) : player_(player) {}

    void Detach() {
        std::scoped_lock lock(mutex_);
        player_ = nullptr;
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IMFMediaEngineNotify) || riid == IID_IUnknown) {
            *out = static_cast<IMFMediaEngineNotify*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG n = --refs_;
        if (n == 0) delete this;
        return n;
    }

    STDMETHODIMP EventNotify(DWORD event, DWORD_PTR, DWORD) override {
        std::scoped_lock lock(mutex_);
        if (player_) player_->OnEngineEvent(static_cast<std::uint32_t>(event));
        return S_OK;
    }

private:
    ~MediaEngineNotify() = default;

    std::atomic<ULONG> refs_{1};
    std::mutex mutex_;
    VideoPlayer* player_ = nullptr;
};

VideoPlayer::VideoPlayer() = default;
VideoPlayer::~VideoPlayer() { Shutdown(); }

Status VideoPlayer::Init(const D3DDevicePtr& device) {
    if (!device) return Status::Fail("null D3D device");
    device_ = device;

    RF_HR(MFStartup(MF_VERSION, MFSTARTUP_FULL));
    mf_started_ = true;

    UINT token = 0;
    RF_HR(MFCreateDXGIDeviceManager(&token, &dxgi_manager_));
    RF_HR(dxgi_manager_->ResetDevice(device_->device(), token));

    ComPtr<IMFMediaEngineClassFactory> factory;
    RF_HR(CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER,
                           IID_PPV_ARGS(&factory)));

    auto* notify = new MediaEngineNotify(this);
    notify_.Attach(static_cast<IUnknown*>(static_cast<IMFMediaEngineNotify*>(notify)));

    ComPtr<IMFAttributes> attributes;
    RF_HR(MFCreateAttributes(&attributes, 4));
    RF_HR(attributes->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify_.Get()));
    RF_HR(attributes->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, dxgi_manager_.Get()));

    RF_HR(attributes->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT,
                                DXGI_FORMAT_B8G8R8A8_UNORM));

    RF_HR(factory->CreateInstance(0, attributes.Get(), &engine_));

    RF_INFO("video player ready");
    return Status::Ok();
}

void VideoPlayer::Shutdown() {
    Close();

    if (notify_) {

        static_cast<MediaEngineNotify*>(
            static_cast<IMFMediaEngineNotify*>(notify_.Get()))->Detach();
    }
    if (engine_) {
        engine_->Shutdown();
        engine_.Reset();
    }
    notify_.Reset();
    dxgi_manager_.Reset();
    srv_.Reset();
    texture_.Reset();
    device_.reset();

    if (mf_started_) {
        MFShutdown();
        mf_started_ = false;
    }
}

Status VideoPlayer::Open(const std::filesystem::path& file) {
    if (!engine_) return Status::Fail("player is not initialised");

    Close();

    ready_.store(false, std::memory_order_release);
    failed_.store(false, std::memory_order_release);
    ended_.store(false, std::memory_order_release);
    size_known_.store(false, std::memory_order_release);
    error_.clear();

    wchar_t url[INTERNET_MAX_URL_LENGTH]{};
    DWORD url_len = INTERNET_MAX_URL_LENGTH;
    if (FAILED(::UrlCreateFromPathW(file.c_str(), url, &url_len, 0)))
        return Status::Fail("could not build a URL for the file");

    BSTR source = ::SysAllocString(url);
    if (!source) return Status::Fail("out of memory");
    const HRESULT hr = engine_->SetSource(source);
    ::SysFreeString(source);
    if (FAILED(hr)) return Status::Fail(hr, "IMFMediaEngine::SetSource");

    file_ = file;
    open_ = true;
    engine_->SetVolume(volume_);
    RF_INFO("player opened {}", file.filename().string());
    return Status::Ok();
}

void VideoPlayer::Close() {
    if (!open_) return;
    if (engine_) {
        engine_->Pause();

        BSTR empty = ::SysAllocString(L"");
        if (empty) {
            engine_->SetSource(empty);
            ::SysFreeString(empty);
        }
    }
    open_ = false;
    ready_.store(false, std::memory_order_release);
    file_.clear();
    width_ = height_ = 0;
    srv_.Reset();
    texture_.Reset();
}

void VideoPlayer::OnEngineEvent(std::uint32_t event) {
    switch (event) {
        case MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA:
            size_known_.store(true, std::memory_order_release);
            SelectEveryAudioStream();
            break;
        case MF_MEDIA_ENGINE_EVENT_CANPLAY:
            ready_.store(true, std::memory_order_release);
            break;
        case MF_MEDIA_ENGINE_EVENT_ENDED:
            ended_.store(true, std::memory_order_release);
            break;
        case MF_MEDIA_ENGINE_EVENT_ERROR: {

            ComPtr<IMFMediaError> err;
            if (SUCCEEDED(engine_->GetError(&err)) && err)
                RF_WARN("media engine error {} (hr 0x{:08x})", err->GetErrorCode(),
                        static_cast<unsigned>(err->GetExtendedErrorCode()));
            failed_.store(true, std::memory_order_release);
            break;
        }
        default:
            break;
    }
}

void VideoPlayer::SelectEveryAudioStream() {
    ComPtr<IMFMediaEngineEx> ex;
    if (!engine_ || FAILED(engine_.As(&ex))) return;

    DWORD count = 0;
    if (FAILED(ex->GetNumberOfStreams(&count)) || count == 0) return;

    bool changed = false;
    for (DWORD i = 0; i < count; ++i) {
        PROPVARIANT type;
        ::PropVariantInit(&type);
        const bool is_audio =
            SUCCEEDED(ex->GetStreamAttribute(i, MF_MT_MAJOR_TYPE, &type)) &&
            type.vt == VT_CLSID && *type.puuid == MFMediaType_Audio;
        ::PropVariantClear(&type);
        if (!is_audio) continue;

        BOOL selected = FALSE;
        if (FAILED(ex->GetStreamSelection(i, &selected)) || selected) continue;
        if (SUCCEEDED(ex->SetStreamSelection(i, TRUE))) changed = true;
    }

    if (changed && SUCCEEDED(ex->ApplyStreamSelections()))
        RF_INFO("player: all {} streams selected - every audio track will be heard", count);
}

Status VideoPlayer::EnsureTexture() {
    DWORD w = 0, h = 0;
    if (FAILED(engine_->GetNativeVideoSize(&w, &h)) || w == 0 || h == 0)
        return Status::Fail("native video size unavailable");

    if (texture_ && w == width_ && h == height_) return Status::Ok();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;

    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    ComPtr<ID3D11Texture2D> texture;
    RF_HR(device_->device()->CreateTexture2D(&desc, nullptr, &texture));
    ComPtr<ID3D11ShaderResourceView> srv;
    RF_HR(device_->device()->CreateShaderResourceView(texture.Get(), nullptr, &srv));

    texture_ = std::move(texture);
    srv_ = std::move(srv);
    width_ = w;
    height_ = h;
    return Status::Ok();
}

bool VideoPlayer::Update() {
    if (!engine_ || !open_) return false;

    if (failed_.exchange(false, std::memory_order_acq_rel)) {
        error_ = "не удалось воспроизвести файл";
        RF_WARN("player: {}", error_);
        return false;
    }

    if (!size_known_.load(std::memory_order_acquire)) return false;
    if (auto s = EnsureTexture(); !s) return false;

    LONGLONG pts = 0;
    if (engine_->OnVideoStreamTick(&pts) != S_OK) return false;

    const RECT dst{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
    MFARGB border{0, 0, 0, 255};
    return SUCCEEDED(engine_->TransferVideoFrame(texture_.Get(), nullptr, &dst, &border));
}

void VideoPlayer::Play() {
    if (!engine_ || !open_) return;

    if (ended_.exchange(false, std::memory_order_acq_rel)) engine_->SetCurrentTime(0.0);
    engine_->Play();
}

void VideoPlayer::Pause() {
    if (engine_ && open_) engine_->Pause();
}

void VideoPlayer::TogglePlay() {
    if (playing()) Pause(); else Play();
}

void VideoPlayer::Seek(double seconds) {
    if (!engine_ || !open_) return;
    const double total = duration();
    engine_->SetCurrentTime(std::clamp(seconds, 0.0, total > 0.0 ? total : seconds));
    ended_.store(false, std::memory_order_release);
}

void VideoPlayer::SetVolume(float volume) {
    volume_ = std::clamp(volume, 0.0f, 1.0f);
    if (engine_) engine_->SetVolume(volume_);
}

bool VideoPlayer::playing() const {
    return engine_ && open_ && !engine_->IsPaused() && !engine_->IsEnded();
}

double VideoPlayer::position() const {
    return engine_ && open_ ? engine_->GetCurrentTime() : 0.0;
}

double VideoPlayer::duration() const {
    if (!engine_ || !open_) return 0.0;
    const double d = engine_->GetDuration();

    return (d > 0.0 && d < 1e9) ? d : 0.0;
}

}
