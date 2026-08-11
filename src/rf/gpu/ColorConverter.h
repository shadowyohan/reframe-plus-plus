#pragma once
#include <d3d11_4.h>

#include <wrl/client.h>

#include "rf/core/Media.h"
#include "rf/core/Status.h"
#include "rf/gpu/D3DDevice.h"

namespace rf {

class ColorConverter {
public:
    Status Init(const D3DDevicePtr& device, std::uint32_t src_width, std::uint32_t src_height,
                DXGI_FORMAT src_format, std::uint32_t dst_width, std::uint32_t dst_height,
                DXGI_FORMAT dst_format, ColorSpace color);

    Status Convert(ID3D11Texture2D* src, ID3D11Texture2D** out);

    [[nodiscard]] DXGI_FORMAT dst_format() const { return dst_format_; }
    [[nodiscard]] bool passthrough() const { return passthrough_; }

private:
    D3DDevicePtr device_;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> video_device_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext2> video_context_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> processor_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;

    static constexpr int kPoolSize = 8;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> outputs_[kPoolSize];
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> output_views_[kPoolSize];
    int next_output_ = 0;

    std::uint32_t src_width_ = 0, src_height_ = 0;
    std::uint32_t dst_width_ = 0, dst_height_ = 0;
    DXGI_FORMAT src_format_ = DXGI_FORMAT_B8G8R8A8_UNORM;
    DXGI_FORMAT dst_format_ = DXGI_FORMAT_NV12;
    bool passthrough_ = false;
};

}
