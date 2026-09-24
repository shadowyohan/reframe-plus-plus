#pragma once
#include <d3d11_4.h>
#include <dxgi1_6.h>

#include <memory>

#include <wrl/client.h>

#include "rf/core/Status.h"
#include "rf/gpu/GpuInfo.h"

namespace rf {

class D3DDevice {
public:
    static Status Create(const AdapterInfo& adapter, std::shared_ptr<D3DDevice>& out);

    static Status CreateForOutput(void* hwnd, std::shared_ptr<D3DDevice>& out);

    static Status CreateForLuid(std::int32_t luid_low, std::int32_t luid_high,
                                std::shared_ptr<D3DDevice>& out);

    ID3D11Device5* device() const { return device_.Get(); }
    ID3D11DeviceContext4* context() const { return context_.Get(); }
    IDXGIAdapter1* adapter() const { return dxgi_adapter_.Get(); }
    const AdapterInfo& info() const { return info_; }

    class ContextLock {
    public:
        explicit ContextLock(D3DDevice& dev) : mt_(dev.multithread_.Get()) {
            if (mt_) mt_->Enter();
        }
        ~ContextLock() {
            if (mt_) mt_->Leave();
        }
        ContextLock(const ContextLock&) = delete;
        ContextLock& operator=(const ContextLock&) = delete;

    private:
        ID3D11Multithread* mt_;
    };

    [[nodiscard]] HRESULT DeviceRemovedReason() const;
    [[nodiscard]] bool alive() const { return DeviceRemovedReason() == S_OK; }

    static const char* DescribeRemovedReason(HRESULT reason);

private:
    void RaiseGpuPriority();

    Microsoft::WRL::ComPtr<ID3D11Device5> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context_;
    Microsoft::WRL::ComPtr<ID3D11Multithread> multithread_;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> dxgi_adapter_;
    AdapterInfo info_;
};

using D3DDevicePtr = std::shared_ptr<D3DDevice>;

}
