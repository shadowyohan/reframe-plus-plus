#include "rf/gpu/D3DDevice.h"

#include "rf/core/Log.h"
#include "rf/core/Strings.h"

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

    out = std::move(self);
    return Status::Ok();
}

Status D3DDevice::CreateForLuid(std::int32_t luid_low, std::int32_t luid_high,
                                std::shared_ptr<D3DDevice>& out) {
    for (const AdapterInfo& info : EnumerateAdapters()) {
        if (info.luid_low == luid_low && info.luid_high == luid_high && !info.is_software) {
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

void D3DDevice::SetLowGpuPriority() {

    if (ComPtr<IDXGIDevice1> dxgi; SUCCEEDED(device_.As(&dxgi))) {
        dxgi->SetMaximumFrameLatency(1);
    }
}

}
