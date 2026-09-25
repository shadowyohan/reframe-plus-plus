#include "rf/gpu/D3DDevice.h"

#include "rf/core/Log.h"
#include "rf/core/Strings.h"

#include <mutex>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace rf {

Status D3DDevice::Create(const AdapterInfo& adapter_info, std::shared_ptr<D3DDevice>& out) {
    auto self = std::shared_ptr<D3DDevice>(new D3DDevice());
    self->info_ = adapter_info;

    ComPtr<IDXGIFactory6> factory;
    RF_HR(::CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
    RF_HR(factory->EnumAdapters1(adapter_info.index, &self->dxgi_adapter_));

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#ifdef _DEBUG

    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    static const D3D_FEATURE_LEVEL kLevels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL got{};

    HRESULT hr = ::D3D11CreateDevice(self->dxgi_adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                     flags, kLevels, ARRAYSIZE(kLevels), D3D11_SDK_VERSION,
                                     &device, &got, &context);
    if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG)) {
        flags &= ~static_cast<UINT>(D3D11_CREATE_DEVICE_DEBUG);
        hr = ::D3D11CreateDevice(self->dxgi_adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
                                 kLevels, ARRAYSIZE(kLevels), D3D11_SDK_VERSION, &device, &got,
                                 &context);
    }
    if (FAILED(hr)) return Status::Fail(hr, "D3D11CreateDevice");

    RF_HR(device.As(&self->device_));
    RF_HR(context.As(&self->context_));

    if (SUCCEEDED(context.As(&self->multithread_))) self->multithread_->SetMultithreadProtected(TRUE);

    RF_INFO("D3D11 device on adapter {} (feature level 0x{:X})", adapter_info.index,
            static_cast<unsigned>(got));

    self->RaiseGpuPriority();
    out = std::move(self);
    return Status::Ok();
}

Status D3DDevice::CreateForLuid(std::int32_t luid_low, std::int32_t luid_high,
                                std::shared_ptr<D3DDevice>& out) {
    for (const AdapterInfo& info : EnumerateSelectableAdapters()) {
        if (info.luid_low == luid_low && info.luid_high == luid_high) {
            RF_INFO("using the configured adapter: {}", ToUtf8(info.description));
            return Create(info, out);
        }
    }
    RF_WARN("the configured GPU is not present - falling back to automatic selection");
    return CreateForOutput(nullptr, out);
}

Status D3DDevice::CreateForOutput(void* hwnd, std::shared_ptr<D3DDevice>& out) {
    AdapterInfo info;
    if (!FindAdapterForOutput(hwnd, info)) return Status::Fail("no DXGI adapter found");
    return Create(info, out);
}

HRESULT D3DDevice::DeviceRemovedReason() const {
    return device_ ? device_->GetDeviceRemovedReason() : E_POINTER;
}

const char* D3DDevice::DescribeRemovedReason(HRESULT reason) {
    switch (reason) {
        case DXGI_ERROR_DEVICE_HUNG:
            return "device hung (a workload took too long - usually another app's)";
        case DXGI_ERROR_DEVICE_REMOVED: return "device removed (driver update or GPU reset)";
        case DXGI_ERROR_DEVICE_RESET:   return "device reset (TDR recovery)";
        case DXGI_ERROR_DRIVER_INTERNAL_ERROR: return "driver internal error";
        case DXGI_ERROR_INVALID_CALL:   return "invalid call";
        default:                        return "unknown";
    }
}

namespace {

enum class GpuSchedulingClass : INT { kBelowNormal = 1, kHigh = 4, kRealtime = 5 };

constexpr INT kMaxGpuThreadPriority = 7;

bool HardwareSchedulingEnabled() {
    DWORD mode = 0;
    DWORD size = sizeof(mode);
    return ::RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers",
                          L"HwSchMode", RRF_RT_REG_DWORD, nullptr, &mode, &size) == ERROR_SUCCESS &&
           mode == 2;
}

void EnableIncreasePriorityPrivilege() {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &token)) return;
    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (::LookupPrivilegeValueW(nullptr, SE_INC_BASE_PRIORITY_NAME, &privileges.Privileges[0].Luid)) {
        ::AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr);
    }
    ::CloseHandle(token);
}

bool SetProcessGpuSchedulingClass(GpuSchedulingClass scheduling_class) {
    using SetClassFn = LONG(APIENTRY*)(HANDLE, INT);
    static const auto set_class = [] {
        HMODULE gdi = ::LoadLibraryExW(L"gdi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return gdi ? reinterpret_cast<SetClassFn>(
                         ::GetProcAddress(gdi, "D3DKMTSetProcessSchedulingPriorityClass"))
                   : nullptr;
    }();
    return set_class &&
           set_class(::GetCurrentProcess(), static_cast<INT>(scheduling_class)) >= 0;
}

void RaiseProcessGpuSchedulingClass() {
    EnableIncreasePriorityPrivilege();
    const bool hags = HardwareSchedulingEnabled();
    if (!hags && SetProcessGpuSchedulingClass(GpuSchedulingClass::kRealtime)) {
        RF_INFO("GPU scheduling class: realtime");
    } else if (SetProcessGpuSchedulingClass(GpuSchedulingClass::kHigh)) {
        RF_INFO("GPU scheduling class: high (HAGS {})", hags ? "on" : "off");
    } else {
        RF_WARN("GPU scheduling class unchanged - under full GPU load frames may drop; run elevated");
    }
}

}

void LowerProcessGpuPriority() {
    if (!SetProcessGpuSchedulingClass(GpuSchedulingClass::kBelowNormal))
        RF_WARN("could not lower the GPU scheduling class");
}

void D3DDevice::RaiseGpuPriority() {
    static std::once_flag process_class_once;
    std::call_once(process_class_once, RaiseProcessGpuSchedulingClass);

    if (ComPtr<IDXGIDevice1> dxgi; SUCCEEDED(device_.As(&dxgi))) {
        dxgi->SetMaximumFrameLatency(1);
        if (HRESULT hr = dxgi->SetGPUThreadPriority(kMaxGpuThreadPriority); FAILED(hr)) {
            RF_WARN("SetGPUThreadPriority failed: 0x{:08X}", static_cast<unsigned>(hr));
        }
    }
}

}
