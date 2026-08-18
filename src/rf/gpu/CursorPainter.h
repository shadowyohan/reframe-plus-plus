#pragma once
#include <d3d11_4.h>

#include <cstdint>

#include <wrl/client.h>

#include "rf/core/Status.h"
#include "rf/gpu/D3DDevice.h"

namespace rf {

class CursorPainter {
public:
    Status Init(const D3DDevicePtr& device, std::uint32_t width, std::uint32_t height,
                DXGI_FORMAT format);

    Status Compose(ID3D11Texture2D* frame, std::int32_t origin_x, std::int32_t origin_y,
                   ID3D11Texture2D** out);

private:
    Status EnsurePipeline();
    Status EnsureShape(void* hcursor);

    D3DDevicePtr device_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;

    static constexpr int kPoolSize = 4;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> pool_[kPoolSize];
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> pool_rtv_[kPoolSize];
    int next_ = 0;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;

    void* shape_source_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> shape_;
    std::int32_t hotspot_x_ = 0;
    std::int32_t hotspot_y_ = 0;
    std::uint32_t shape_w_ = 0;
    std::uint32_t shape_h_ = 0;
};

}
