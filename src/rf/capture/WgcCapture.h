#pragma once
#include "rf/capture/IVideoCapture.h"

#ifdef RF_HAS_WGC

#include <atomic>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

namespace rf {

class WgcCapture final : public IVideoCapture {
public:
    explicit WgcCapture(D3DDevicePtr device);
    ~WgcCapture() override;

    Status Start(const CaptureTarget& target, const FrameCallback& on_frame) override;
    void Stop() override;

    CaptureStats stats() const override { return stats_; }
    CaptureBackend backend() const override { return CaptureBackend::WindowsGraphicsCapture; }
    std::uint32_t width() const override { return width_; }
    std::uint32_t height() const override { return height_; }

private:
    void OnFrameArrived(const winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool& pool,
                        const winrt::Windows::Foundation::IInspectable&);

    Status CreateItem(const CaptureTarget& target,
                      winrt::Windows::Graphics::Capture::GraphicsCaptureItem& out);

    D3DDevicePtr device_;
    FrameCallback on_frame_;

    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice winrt_device_{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item_{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool pool_{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session_{nullptr};
    winrt::event_token frame_token_{};
    winrt::event_token closed_token_{};

    std::atomic<bool> running_{false};
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint64_t frame_index_ = 0;
    Ticks100ns last_timestamp_ = 0;
    CaptureStats stats_{};
};

}

#endif
