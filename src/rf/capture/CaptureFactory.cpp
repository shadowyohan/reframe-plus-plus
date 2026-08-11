#include "rf/capture/DxgiDuplCapture.h"
#include "rf/capture/GameCapture.h"
#include "rf/capture/IVideoCapture.h"
#include "rf/capture/WgcCapture.h"
#include "rf/core/Log.h"

#ifdef RF_HAS_WGC
#include <winrt/Windows.Graphics.Capture.h>
#endif

namespace rf {

const char* ToString(CaptureBackend b) {
    switch (b) {
        case CaptureBackend::Auto:                   return "auto";
        case CaptureBackend::WindowsGraphicsCapture: return "Windows.Graphics.Capture";
        case CaptureBackend::DesktopDuplication:     return "Desktop Duplication";
        case CaptureBackend::GameHook:               return "Game Capture (hook)";
    }
    return "?";
}

bool IsWgcSupported() {
#ifdef RF_HAS_WGC
    try {
        return winrt::Windows::Graphics::Capture::GraphicsCaptureSession::IsSupported();
    } catch (...) {
        return false;
    }
#else
    return false;
#endif
}

Status CreateVideoCapture(const D3DDevicePtr& device, CaptureBackend backend,
                          const CaptureTarget& target, VideoCapturePtr& out) {
    if (!device) return Status::Fail("null D3D device");

    if (backend == CaptureBackend::GameHook) {
        out = std::make_unique<GameCapture>(device);
        RF_INFO("capture backend: {}", ToString(backend));
        return Status::Ok();
    }

    if (backend == CaptureBackend::Auto) {

        backend = (target.kind == CaptureTarget::Kind::Window)
                      ? CaptureBackend::WindowsGraphicsCapture
                      : CaptureBackend::DesktopDuplication;
        if (backend == CaptureBackend::WindowsGraphicsCapture && !IsWgcSupported())
            backend = CaptureBackend::DesktopDuplication;
    }

#ifdef RF_HAS_WGC
    if (backend == CaptureBackend::WindowsGraphicsCapture) {
        out = std::make_unique<WgcCapture>(device);
        RF_INFO("capture backend: {}", ToString(backend));
        return Status::Ok();
    }
#else
    if (backend == CaptureBackend::WindowsGraphicsCapture) {
        RF_WARN("built without WGC support - using Desktop Duplication");
        backend = CaptureBackend::DesktopDuplication;
    }
#endif

    out = std::make_unique<DxgiDuplCapture>(device);
    RF_INFO("capture backend: {}", ToString(backend));
    return Status::Ok();
}

}
