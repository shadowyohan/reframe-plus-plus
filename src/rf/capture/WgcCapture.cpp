#include "rf/capture/WgcCapture.h"

#ifdef RF_HAS_WGC

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.h>

#include "rf/core/Log.h"

#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "dwmapi.lib")

using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
using Microsoft::WRL::ComPtr;

namespace rf {
namespace {

constexpr int kFramePoolBuffers = 3;

ComPtr<ID3D11Texture2D> TextureFromSurface(const IDirect3DSurface& surface) {
    auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    ComPtr<ID3D11Texture2D> texture;
    winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&texture)));
    return texture;
}

}

WgcCapture::WgcCapture(D3DDevicePtr device) : device_(std::move(device)) {}

WgcCapture::~WgcCapture() { Stop(); }

Status WgcCapture::CreateItem(const CaptureTarget& target, GraphicsCaptureItem& out) {
    auto interop = winrt::get_activation_factory<GraphicsCaptureItem>()
                       .as<::IGraphicsCaptureItemInterop>();

    HRESULT hr = S_OK;
    switch (target.kind) {
        case CaptureTarget::Kind::Window:
            hr = interop->CreateForWindow(static_cast<HWND>(target.hwnd),
                                          winrt::guid_of<GraphicsCaptureItem>(),
                                          winrt::put_abi(out));
            break;
        case CaptureTarget::Kind::Display:
        case CaptureTarget::Kind::PrimaryDisplay: {
            HMONITOR monitor = target.hmonitor
                                   ? static_cast<HMONITOR>(target.hmonitor)
                                   : ::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
            hr = interop->CreateForMonitor(monitor, winrt::guid_of<GraphicsCaptureItem>(),
                                           winrt::put_abi(out));
            break;
        }
    }
    if (FAILED(hr)) return Status::Fail(hr, "IGraphicsCaptureItemInterop::CreateFor*");
    return Status::Ok();
}

Status WgcCapture::Start(const CaptureTarget& target, const FrameCallback& on_frame) {
    if (running_) return Status::Fail("capture already running");
    on_frame_ = on_frame;

    try {
        if (!GraphicsCaptureSession::IsSupported())
            return Status::Fail("Windows.Graphics.Capture is not supported on this system");

        RF_TRY(CreateItem(target, item_));

        ComPtr<IDXGIDevice> dxgi;
        RF_HR(device_->device()->QueryInterface(IID_PPV_ARGS(&dxgi)));
        winrt::com_ptr<::IInspectable> inspectable;
        RF_HR(::CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put()));
        winrt_device_ = inspectable.as<IDirect3DDevice>();

        const auto size = item_.Size();
        width_ = static_cast<std::uint32_t>(size.Width);
        height_ = static_cast<std::uint32_t>(size.Height);

        pool_ = Direct3D11CaptureFramePool::CreateFreeThreaded(
            winrt_device_, DirectXPixelFormat::B8G8R8A8UIntNormalized, kFramePoolBuffers, size);

        session_ = pool_.CreateCaptureSession(item_);
        session_.IsCursorCaptureEnabled(target.capture_cursor);

        try {
            if (winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
                    L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired")) {
                session_.IsBorderRequired(target.show_capture_border);
            }
        } catch (const winrt::hresult_error& e) {
            RF_WARN("IsBorderRequired unavailable: 0x{:08X}", static_cast<unsigned>(e.code()));
        }

        frame_token_ = pool_.FrameArrived({this, &WgcCapture::OnFrameArrived});
        closed_token_ = item_.Closed([this](auto&&, auto&&) {
            RF_WARN("capture item closed (window destroyed / display removed)");
            running_ = false;
        });

        running_ = true;
        session_.StartCapture();
        RF_INFO("WGC capture started {}x{}", width_, height_);
        return Status::Ok();
    } catch (const winrt::hresult_error& e) {
        return Status::Fail(e.code(), winrt::to_string(e.message()));
    }
}

void WgcCapture::Stop() {
    if (!running_.exchange(false) && !pool_) return;
    try {
        if (pool_ && frame_token_) pool_.FrameArrived(frame_token_);
        if (item_ && closed_token_) item_.Closed(closed_token_);
        if (session_) session_.Close();
        if (pool_) pool_.Close();
    } catch (const winrt::hresult_error& e) {
        RF_WARN("WGC teardown: 0x{:08X}", static_cast<unsigned>(e.code()));
    }
    session_ = nullptr;
    pool_ = nullptr;
    item_ = nullptr;
}

void WgcCapture::OnFrameArrived(const Direct3D11CaptureFramePool& pool,
                                const winrt::Windows::Foundation::IInspectable&) {
    if (!running_.load(std::memory_order_relaxed)) return;

    try {

        auto frame = pool.TryGetNextFrame();
        if (!frame) return;

        const auto size = frame.ContentSize();
        if (static_cast<std::uint32_t>(size.Width) != width_ ||
            static_cast<std::uint32_t>(size.Height) != height_) {

            RF_INFO("content resized {}x{} -> {}x{}", width_, height_, size.Width, size.Height);
            width_ = static_cast<std::uint32_t>(size.Width);
            height_ = static_cast<std::uint32_t>(size.Height);
            pool.Recreate(winrt_device_, DirectXPixelFormat::B8G8R8A8UIntNormalized,
                          kFramePoolBuffers, size);
            ++stats_.recoveries;
            return;
        }

        const Ticks100ns ts = frame.SystemRelativeTime().count();
        if (ts == last_timestamp_) {
            ++stats_.frames_repeated;
            return;
        }
        last_timestamp_ = ts;

        auto texture = TextureFromSurface(frame.Surface());

        CapturedFrame out;
        out.texture = texture.Get();
        out.width = width_;
        out.height = height_;
        out.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        out.timestamp = ts;
        out.content_changed = true;
        out.frame_index = frame_index_++;

        ++stats_.frames_captured;
        if (on_frame_) on_frame_(out);
    } catch (const winrt::hresult_error& e) {
        RF_ERROR("WGC frame handling failed: 0x{:08X}", static_cast<unsigned>(e.code()));
        ++stats_.frames_dropped;
    }
}

}

#endif
