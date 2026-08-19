#include "rf/gpu/FrameBridge.h"

#include "rf/core/Log.h"

using Microsoft::WRL::ComPtr;

namespace rf {
namespace {

constexpr std::uint64_t kKeyWriter = 0;
constexpr std::uint64_t kKeyReader = 1;

}

Status FrameBridge::Init(const D3DDevicePtr& capture, const D3DDevicePtr& encode,
                         std::uint32_t width, std::uint32_t height, DXGI_FORMAT format) {
    capture_ = capture;
    encode_ = encode;
    width_ = width;
    height_ = height;
    crosses_devices_ = encode && capture && encode->device() != capture->device();

    if (!crosses_devices_) return Status::Ok();

    if (auto s = Share(format, true); s.ok()) {
        keyed_ = true;
        RF_INFO("frame bridge up: {}x{} shared with a keyed mutex", width, height);
        return Status::Ok();
    } else {
        RF_DEBUG("shared frames with a keyed mutex unavailable: {}", s.str());
    }

    for (int i = 0; i < kSlots; ++i) {
        written_[i].Reset();
        read_[i].Reset();
        write_mutex_[i].Reset();
        read_mutex_[i].Reset();
    }

    RF_TRY(Share(format, false));
    keyed_ = false;
    RF_INFO("frame bridge up: {}x{} shared without a keyed mutex", width, height);
    return Status::Ok();
}

Status FrameBridge::Share(DXGI_FORMAT format, bool keyed) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width_;
    td.Height = height_;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = keyed ? D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX
                         : D3D11_RESOURCE_MISC_SHARED;

    for (int i = 0; i < kSlots; ++i) {
        RF_HR(capture_->device()->CreateTexture2D(&td, nullptr, &written_[i]));

        if (keyed) {
            ComPtr<IDXGIResource1> resource;
            RF_HR(written_[i].As(&resource));

            HANDLE shared = nullptr;
            RF_HR(resource->CreateSharedHandle(
                nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &shared));

            ComPtr<ID3D11Device1> encode_device;
            RF_HR(encode_->device()->QueryInterface(IID_PPV_ARGS(&encode_device)));
            const HRESULT opened =
                encode_device->OpenSharedResource1(shared, IID_PPV_ARGS(&read_[i]));
            ::CloseHandle(shared);
            if (FAILED(opened))
                return Status::Fail(opened, "the encoding GPU could not open the shared frame");

            RF_HR(written_[i].As(&write_mutex_[i]));
            RF_HR(read_[i].As(&read_mutex_[i]));
            continue;
        }

        ComPtr<IDXGIResource> resource;
        RF_HR(written_[i].As(&resource));

        HANDLE shared = nullptr;
        RF_HR(resource->GetSharedHandle(&shared));
        const HRESULT opened =
            encode_->device()->OpenSharedResource(shared, IID_PPV_ARGS(&read_[i]));
        if (FAILED(opened))
            return Status::Fail(opened, "the encoding GPU could not open the shared frame");
    }
    return Status::Ok();
}

Status FrameBridge::Import(ID3D11Texture2D* frame, ID3D11Texture2D** out) {
    if (!frame || !out) return Status::Fail(E_POINTER, "FrameBridge::Import");

    const int slot = next_;
    next_ = (next_ + 1) % kSlots;

    const D3D11_BOX box{0, 0, 0, width_, height_, 1};

    if (!crosses_devices_) {
        *out = frame;
        return Status::Ok();
    }

    if (keyed_ && write_mutex_[slot]->AcquireSync(kKeyWriter, 8) != WAIT_OBJECT_0)
        return Status::Fail("the other adapter is still reading this frame");

    {
        D3DDevice::ContextLock lock(*capture_);
        capture_->context()->CopySubresourceRegion(written_[slot].Get(), 0, 0, 0, 0, frame, 0, &box);
        capture_->context()->Flush();
    }

    if (keyed_) {
        write_mutex_[slot]->ReleaseSync(kKeyReader);
        if (read_mutex_[slot]->AcquireSync(kKeyReader, 8) != WAIT_OBJECT_0)
            return Status::Fail("could not take the frame on the encoding adapter");
        read_mutex_[slot]->ReleaseSync(kKeyWriter);
    }

    *out = read_[slot].Get();
    return Status::Ok();
}

}
