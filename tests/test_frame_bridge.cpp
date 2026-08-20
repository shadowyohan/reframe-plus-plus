#include "rf/gpu/FrameBridge.h"

#include "test_framework.h"

using Microsoft::WRL::ComPtr;
using rf::D3DDevice;
using rf::D3DDevicePtr;
using rf::FrameBridge;

namespace {

constexpr std::uint32_t kWidth = 320;
constexpr std::uint32_t kHeight = 180;

D3DDevicePtr SecondDeviceOnSameAdapter(const D3DDevicePtr& first) {
    D3DDevicePtr second;
    if (auto s = D3DDevice::CreateForLuid(first->info().luid_low, first->info().luid_high, second);
        !s.ok())
        return nullptr;
    return second;
}

ComPtr<ID3D11Texture2D> PaintedFrame(const D3DDevicePtr& device, const float colour[4]) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = kWidth;
    td.Height = kHeight;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;

    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->device()->CreateTexture2D(&td, nullptr, &texture))) return nullptr;

    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(device->device()->CreateRenderTargetView(texture.Get(), nullptr, &rtv)))
        return nullptr;

    D3DDevice::ContextLock lock(*device);
    device->context()->ClearRenderTargetView(rtv.Get(), colour);
    device->context()->Flush();
    return texture;
}

bool ReadFirstPixel(const D3DDevicePtr& device, ID3D11Texture2D* frame, std::uint8_t out[4]) {
    D3D11_TEXTURE2D_DESC td{};
    frame->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.MiscFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ComPtr<ID3D11Texture2D> readback;
    if (FAILED(device->device()->CreateTexture2D(&td, nullptr, &readback))) return false;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    D3DDevice::ContextLock lock(*device);
    device->context()->CopyResource(readback.Get(), frame);
    if (FAILED(device->context()->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    std::memcpy(out, mapped.pData, 4);
    device->context()->Unmap(readback.Get(), 0);
    return true;
}

}

TEST(FrameBridge_MovesTheActualPixelsToTheOtherDevice) {
    D3DDevicePtr capture;
    if (auto s = D3DDevice::CreateForOutput(nullptr, capture); !s.ok() || !capture) {
        SKIP("no D3D11 device on this machine");
        return;
    }

    D3DDevicePtr encode = SecondDeviceOnSameAdapter(capture);
    if (!encode || encode->device() == capture->device()) {
        SKIP("no second device to bridge to");
        return;
    }

    FrameBridge bridge;
    if (auto s = bridge.Init(capture, encode, kWidth, kHeight, DXGI_FORMAT_B8G8R8A8_UNORM);
        !s.ok()) {
        SKIP("this driver does not share textures between devices");
        return;
    }

    constexpr float kOrange[4] = {1.0f, 0.5f, 0.0f, 1.0f};
    ComPtr<ID3D11Texture2D> frame = PaintedFrame(capture, kOrange);
    CHECK(frame != nullptr);

    for (int i = 0; i < 8; ++i) {
        ID3D11Texture2D* arrived = nullptr;
        CHECK(bridge.Import(frame.Get(), &arrived).ok());
        CHECK(arrived != nullptr);

        std::uint8_t pixel[4]{};
        CHECK(ReadFirstPixel(encode, arrived, pixel));
        CHECK(pixel[0] < 40);
        CHECK(pixel[1] > 100 && pixel[1] < 160);
        CHECK(pixel[2] > 220);
    }
}
