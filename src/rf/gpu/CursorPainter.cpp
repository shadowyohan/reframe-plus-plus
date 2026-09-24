#include "rf/gpu/CursorPainter.h"

#include <windows.h>

#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "rf/core/Log.h"

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace rf {
namespace {

constexpr char kShader[] = R"(
cbuffer Params : register(b0) { float4 rect; };

struct VsOut {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VsOut VsMain(uint id : SV_VertexID) {
    VsOut o;
    o.uv = float2(id & 1, (id >> 1) & 1);
    o.pos = float4(lerp(rect.xy, rect.zw, o.uv), 0.0, 1.0);
    return o;
}

Texture2D<float4> Shape : register(t0);
SamplerState Samp : register(s0);

float4 PsMain(VsOut i) : SV_TARGET {
    return Shape.Sample(Samp, i.uv);
}
)";

Status Compile(const char* entry, const char* profile, ComPtr<ID3DBlob>& out) {
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = ::D3DCompile(kShader, sizeof(kShader) - 1, "cursor", nullptr, nullptr, entry,
                                    profile, 0, 0, &out, &errors);
    if (FAILED(hr)) {
        if (errors)
            RF_WARN("cursor shader: {}", static_cast<const char*>(errors->GetBufferPointer()));
        return Status::Fail(hr, "D3DCompile");
    }
    return Status::Ok();
}

bool RasterizeCursor(HICON icon, int width, int height, std::vector<std::uint32_t>& out) {
    HDC screen = ::GetDC(nullptr);
    HDC dc = ::CreateCompatibleDC(screen);
    ::ReleaseDC(nullptr, screen);
    if (!dc) return false;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    auto render = [&](COLORREF background, std::vector<std::uint32_t>& pixels) {
        void* bits = nullptr;
        HBITMAP bitmap = ::CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bitmap) return false;
        HGDIOBJ old = ::SelectObject(dc, bitmap);

        RECT rect{0, 0, width, height};
        HBRUSH brush = ::CreateSolidBrush(background);
        ::FillRect(dc, &rect, brush);
        ::DeleteObject(brush);
        ::DrawIconEx(dc, 0, 0, icon, width, height, 0, nullptr, DI_NORMAL);
        ::GdiFlush();

        auto* first = static_cast<std::uint32_t*>(bits);
        pixels.assign(first, first + static_cast<std::size_t>(width) * height);
        ::SelectObject(dc, old);
        ::DeleteObject(bitmap);
        return true;
    };

    std::vector<std::uint32_t> on_black, on_white;
    const bool ok = render(RGB(0, 0, 0), on_black) && render(RGB(255, 255, 255), on_white);
    ::DeleteDC(dc);
    if (!ok) return false;

    out.resize(on_black.size());
    for (std::size_t i = 0; i < out.size(); ++i) {
        const std::uint32_t b = on_black[i];
        const std::uint32_t w = on_white[i];

        int alpha = 0;
        for (int shift = 0; shift < 24; shift += 8) {
            const int cb = static_cast<int>((b >> shift) & 0xFF);
            const int cw = static_cast<int>((w >> shift) & 0xFF);
            alpha = std::max(alpha, 255 - (cw - cb));
        }
        alpha = std::clamp(alpha, 0, 255);

        out[i] = (static_cast<std::uint32_t>(alpha) << 24) | (b & 0x00FFFFFFu);
    }
    return true;
}

}

Status CursorPainter::Init(const D3DDevicePtr& device, std::uint32_t width, std::uint32_t height,
                           DXGI_FORMAT format) {
    device_ = device;
    RF_TRY(CreatePool(width, height, format));
    RF_TRY(EnsurePipeline());
    RF_INFO("cursor painter up: {}x{}", width, height);
    return Status::Ok();
}

Status CursorPainter::CreatePool(std::uint32_t width, std::uint32_t height, DXGI_FORMAT format) {
    width_ = width;
    height_ = height;
    format_ = format;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    for (int i = 0; i < kPoolSize; ++i) {
        RF_HR(device_->device()->CreateTexture2D(&td, nullptr, &pool_[i]));
        RF_HR(device_->device()->CreateRenderTargetView(pool_[i].Get(), nullptr, &pool_rtv_[i]));
    }
    return Status::Ok();
}

Status CursorPainter::EnsurePipeline() {
    ComPtr<ID3DBlob> vs_blob, ps_blob;
    RF_TRY(Compile("VsMain", "vs_4_0", vs_blob));
    RF_TRY(Compile("PsMain", "ps_4_0", ps_blob));

    RF_HR(device_->device()->CreateVertexShader(vs_blob->GetBufferPointer(),
                                                vs_blob->GetBufferSize(), nullptr, &vs_));
    RF_HR(device_->device()->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(),
                                               nullptr, &ps_));

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(float) * 4;
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    RF_HR(device_->device()->CreateBuffer(&cb, nullptr, &constants_));

    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    RF_HR(device_->device()->CreateBlendState(&blend, &blend_));

    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    RF_HR(device_->device()->CreateSamplerState(&sampler, &sampler_));

    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    RF_HR(device_->device()->CreateRasterizerState(&raster, &raster_));
    return Status::Ok();
}

Status CursorPainter::EnsureShape(void* hcursor) {
    if (shape_ && shape_source_ == hcursor) return Status::Ok();

    shape_.Reset();
    shape_source_ = hcursor;

    ICONINFO info{};
    if (!::GetIconInfo(static_cast<HICON>(hcursor), &info))
        return Status::Fail("GetIconInfo for the pointer failed");

    BITMAP bitmap{};
    int width = 32, height = 32;
    if (info.hbmColor && ::GetObjectW(info.hbmColor, sizeof(bitmap), &bitmap)) {
        width = bitmap.bmWidth;
        height = bitmap.bmHeight;
    } else if (info.hbmMask && ::GetObjectW(info.hbmMask, sizeof(bitmap), &bitmap)) {
        width = bitmap.bmWidth;

        height = bitmap.bmHeight / 2;
    }

    hotspot_x_ = static_cast<std::int32_t>(info.xHotspot);
    hotspot_y_ = static_cast<std::int32_t>(info.yHotspot);
    if (info.hbmColor) ::DeleteObject(info.hbmColor);
    if (info.hbmMask) ::DeleteObject(info.hbmMask);

    std::vector<std::uint32_t> pixels;
    if (width <= 0 || height <= 0 ||
        !RasterizeCursor(static_cast<HICON>(hcursor), width, height, pixels))
        return Status::Fail("could not rasterise the pointer");

    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(width);
    td.Height = static_cast<UINT>(height);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = pixels.data();
    data.SysMemPitch = static_cast<UINT>(width) * 4;

    ComPtr<ID3D11Texture2D> texture;
    RF_HR(device_->device()->CreateTexture2D(&td, &data, &texture));
    RF_HR(device_->device()->CreateShaderResourceView(texture.Get(), nullptr, &shape_));

    shape_w_ = static_cast<std::uint32_t>(width);
    shape_h_ = static_cast<std::uint32_t>(height);
    return Status::Ok();
}

Status CursorPainter::Compose(ID3D11Texture2D* frame, std::int32_t origin_x, std::int32_t origin_y,
                              ID3D11Texture2D** out) {
    if (!frame || !out) return Status::Fail(E_POINTER, "CursorPainter::Compose");

    D3D11_TEXTURE2D_DESC incoming{};
    frame->GetDesc(&incoming);
    if (incoming.Width != width_ || incoming.Height != height_ || incoming.Format != format_) {
        RF_INFO("cursor painter follows the display to {}x{}", incoming.Width, incoming.Height);
        RF_TRY(CreatePool(incoming.Width, incoming.Height, incoming.Format));
    }

    const int slot = next_;
    next_ = (next_ + 1) % kPoolSize;
    *out = pool_[slot].Get();

    CURSORINFO cursor{sizeof(cursor)};
    const bool visible =
        ::GetCursorInfo(&cursor) && (cursor.flags & CURSOR_SHOWING) != 0 && cursor.hCursor;

    D3DDevice::ContextLock lock(*device_);
    ID3D11DeviceContext* ctx = device_->context();
    ctx->CopyResource(pool_[slot].Get(), frame);

    if (!visible) return Status::Ok();
    if (auto s = EnsureShape(cursor.hCursor); !s.ok()) return Status::Ok();

    const float x0 = static_cast<float>(cursor.ptScreenPos.x - origin_x - hotspot_x_);
    const float y0 = static_cast<float>(cursor.ptScreenPos.y - origin_y - hotspot_y_);
    const float x1 = x0 + static_cast<float>(shape_w_);
    const float y1 = y0 + static_cast<float>(shape_h_);

    if (x1 < 0.0f || y1 < 0.0f || x0 > static_cast<float>(width_) ||
        y0 > static_cast<float>(height_))
        return Status::Ok();

    const float rect[4] = {
        x0 / static_cast<float>(width_) * 2.0f - 1.0f,
        1.0f - y0 / static_cast<float>(height_) * 2.0f,
        x1 / static_cast<float>(width_) * 2.0f - 1.0f,
        1.0f - y1 / static_cast<float>(height_) * 2.0f,
    };

    D3D11_MAPPED_SUBRESOURCE mapped{};
    RF_HR(ctx->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
    std::memcpy(mapped.pData, rect, sizeof(rect));
    ctx->Unmap(constants_.Get(), 0);

    const D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width_),
                                  static_cast<float>(height_), 0.0f, 1.0f};
    const FLOAT factor[4] = {0, 0, 0, 0};
    ID3D11RenderTargetView* rtv = pool_rtv_[slot].Get();
    ID3D11ShaderResourceView* srv = shape_.Get();
    ID3D11SamplerState* sampler = sampler_.Get();
    ID3D11Buffer* cb = constants_.Get();

    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->OMSetBlendState(blend_.Get(), factor, 0xFFFFFFFF);
    ctx->RSSetState(raster_.Get());
    ctx->RSSetViewports(1, &viewport);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->VSSetShader(vs_.Get(), nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &cb);
    ctx->PSSetShader(ps_.Get(), nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->PSSetSamplers(0, 1, &sampler);
    ctx->Draw(4, 0);

    ID3D11RenderTargetView* none = nullptr;
    ctx->OMSetRenderTargets(1, &none, nullptr);
    return Status::Ok();
}

}
