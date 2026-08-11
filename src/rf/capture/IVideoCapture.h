#pragma once
#include <d3d11_4.h>

#include <functional>
#include <memory>
#include <string>

#include <wrl/client.h>

#include "rf/core/Status.h"
#include "rf/core/Time.h"
#include "rf/gpu/D3DDevice.h"

namespace rf {

struct CapturedFrame {
    ID3D11Texture2D* texture = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM;
    Ticks100ns timestamp = 0;
    bool content_changed = true;
    std::uint64_t frame_index = 0;
};

using FrameCallback = std::function<void(const CapturedFrame&)>;

enum class CaptureBackend {
    Auto,
    WindowsGraphicsCapture,
    DesktopDuplication,
    GameHook,
};

const char* ToString(CaptureBackend b);

struct CaptureTarget {
    enum class Kind { PrimaryDisplay, Display, Window } kind = Kind::PrimaryDisplay;
    void* hwnd = nullptr;
    void* hmonitor = nullptr;
    bool capture_cursor = true;
    bool show_capture_border = false;
};

struct CaptureStats {
    std::uint64_t frames_captured = 0;
    std::uint64_t frames_repeated = 0;
    std::uint64_t frames_dropped = 0;
    std::uint64_t recoveries = 0;
    double avg_acquire_ms = 0.0;
};

class IVideoCapture {
public:
    virtual ~IVideoCapture() = default;

    virtual Status Start(const CaptureTarget& target, const FrameCallback& on_frame) = 0;
    virtual void Stop() = 0;

    [[nodiscard]] virtual CaptureStats stats() const = 0;
    [[nodiscard]] virtual CaptureBackend backend() const = 0;
    [[nodiscard]] virtual std::uint32_t width() const = 0;
    [[nodiscard]] virtual std::uint32_t height() const = 0;
};

using VideoCapturePtr = std::unique_ptr<IVideoCapture>;

Status CreateVideoCapture(const D3DDevicePtr& device, CaptureBackend backend,
                          const CaptureTarget& target, VideoCapturePtr& out);

bool IsWgcSupported();

}
