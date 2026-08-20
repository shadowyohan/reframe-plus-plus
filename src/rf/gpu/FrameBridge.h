#pragma once
#include <d3d11_4.h>

#include <cstdint>

#include <wrl/client.h>

#include "rf/core/Status.h"
#include "rf/gpu/D3DDevice.h"

namespace rf {

class FrameBridge {
public:
    Status Init(const D3DDevicePtr& capture, const D3DDevicePtr& encode, std::uint32_t width,
                std::uint32_t height, DXGI_FORMAT format);

    Status Import(ID3D11Texture2D* frame, ID3D11Texture2D** out);

    [[nodiscard]] bool crosses_devices() const { return crosses_devices_; }

private:
    Status Share(DXGI_FORMAT format, bool keyed);
    Status Verify(DXGI_FORMAT format);
    void WaitForCaptureGpu();
    void Release();

    D3DDevicePtr capture_;
    D3DDevicePtr encode_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    bool crosses_devices_ = false;
    bool keyed_ = false;

    static constexpr int kSlots = 6;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> written_[kSlots];
    Microsoft::WRL::ComPtr<ID3D11Texture2D> read_[kSlots];
    Microsoft::WRL::ComPtr<ID3D11Texture2D> landed_[kSlots];
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> write_mutex_[kSlots];
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> read_mutex_[kSlots];
    Microsoft::WRL::ComPtr<ID3D11Query> drawn_;
    int next_ = 0;
};

}
