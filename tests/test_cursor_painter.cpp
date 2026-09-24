#include "rf/gpu/CursorPainter.h"

#include "test_framework.h"

using Microsoft::WRL::ComPtr;

namespace {

ComPtr<ID3D11Texture2D> BlankFrame(const rf::D3DDevicePtr& device, std::uint32_t width,
                                   std::uint32_t height) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    device->device()->CreateTexture2D(&td, nullptr, &texture);
    return texture;
}

}

TEST(CursorPainter_FollowsTheFrameWhenTheDisplaySizeChanges) {
    rf::D3DDevicePtr device;
    if (!rf::D3DDevice::CreateForOutput(nullptr, device).ok()) return;

    rf::CursorPainter painter;
    CHECK(painter.Init(device, 1920, 1080, DXGI_FORMAT_B8G8R8A8_UNORM).ok());

    const auto smaller = BlankFrame(device, 1280, 720);
    CHECK(smaller != nullptr);
    ID3D11Texture2D* composed = nullptr;
    CHECK(painter.Compose(smaller.Get(), 0, 0, &composed).ok());
    CHECK(composed != nullptr);

    D3D11_TEXTURE2D_DESC desc{};
    composed->GetDesc(&desc);
    CHECK_EQ(desc.Width, 1280u);
    CHECK_EQ(desc.Height, 720u);
}
