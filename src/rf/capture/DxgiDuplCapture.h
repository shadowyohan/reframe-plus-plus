#pragma once
#include <dxgi1_6.h>

#include <atomic>
#include <thread>

#include "rf/capture/IVideoCapture.h"
#include "rf/gpu/CursorPainter.h"

namespace rf {

class DxgiDuplCapture final : public IVideoCapture {
public:
    explicit DxgiDuplCapture(D3DDevicePtr device);
    ~DxgiDuplCapture() override;

    Status Start(const CaptureTarget& target, const FrameCallback& on_frame) override;
    void Stop() override;

    CaptureStats stats() const override { return stats_; }
    CaptureBackend backend() const override { return CaptureBackend::DesktopDuplication; }
    std::uint32_t width() const override { return width_; }
    std::uint32_t height() const override { return height_; }

private:
    Status CreateDuplication();
    void ReleaseDuplication();
    void CaptureLoop();

    D3DDevicePtr device_;
    CaptureTarget target_;
    FrameCallback on_frame_;

    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> dupl_;
    Microsoft::WRL::ComPtr<IDXGIOutput1> output_;

    CursorPainter cursor_;
    bool cursor_ready_ = false;
    std::int32_t desktop_x_ = 0;
    std::int32_t desktop_y_ = 0;

    std::thread thread_;
    std::atomic<bool> running_{false};

    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_B8G8R8A8_UNORM;
    std::uint64_t frame_index_ = 0;
    CaptureStats stats_{};
};

}
