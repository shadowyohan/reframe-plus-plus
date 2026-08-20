#include "rf/gpu/FrameBridge.h"

#include <cstring>
#include <format>

#include "rf/core/Log.h"

using Microsoft::WRL::ComPtr;

namespace rf {
namespace {

constexpr std::uint64_t kKeyWriter = 0;
constexpr std::uint64_t kKeyReader = 1;
constexpr DWORD kMutexWaitMs = 16;

}

Status FrameBridge::Init(const D3DDevicePtr& capture, const D3DDevicePtr& encode,
                         std::uint32_t width, std::uint32_t height, DXGI_FORMAT format) {
    capture_ = capture;
    encode_ = encode;
    width_ = width;
    height_ = height;
    crosses_devices_ = encode && capture && encode->device() != capture->device();

    if (!crosses_devices_) return Status::Ok();

    keyed_ = true;
    if (Status shared = Share(format, true); shared.ok()) {
        if (Status seen = Verify(format); seen.ok()) {
            RF_INFO("frame bridge up: {}x{} shared with a keyed mutex", width, height);
            return Status::Ok();
        } else {
            RF_DEBUG("frames shared with a keyed mutex do not arrive: {}", seen.str());
        }
    } else {
        RF_DEBUG("shared frames with a keyed mutex unavailable: {}", shared.str());
    }

    Release();

    keyed_ = false;
    RF_TRY(Share(format, false));
    RF_TRY(Verify(format));
    RF_INFO("frame bridge up: {}x{} shared without a keyed mutex", width, height);
    return Status::Ok();
}

void FrameBridge::Release() {
    for (int i = 0; i < kSlots; ++i) {
        written_[i].Reset();
        read_[i].Reset();
        landed_[i].Reset();
        write_mutex_[i].Reset();
        read_mutex_[i].Reset();
    }
    drawn_.Reset();
    next_ = 0;
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

    D3D11_TEXTURE2D_DESC landing = td;
    landing.MiscFlags = 0;

    if (!keyed) {
        const D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
        RF_HR(capture_->device()->CreateQuery(&qd, &drawn_));
    }

    for (int i = 0; i < kSlots; ++i) {
        RF_HR(capture_->device()->CreateTexture2D(&td, nullptr, &written_[i]));
        RF_HR(encode_->device()->CreateTexture2D(&landing, nullptr, &landed_[i]));

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

Status FrameBridge::Verify(DXGI_FORMAT format) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width_;
    td.Height = height_;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;

    ComPtr<ID3D11Texture2D> probe;
    RF_HR(capture_->device()->CreateTexture2D(&td, nullptr, &probe));

    ComPtr<ID3D11RenderTargetView> rtv;
    RF_HR(capture_->device()->CreateRenderTargetView(probe.Get(), nullptr, &rtv));

    constexpr float kMagenta[4] = {1.0f, 0.0f, 1.0f, 1.0f};
    {
        D3DDevice::ContextLock lock(*capture_);
        capture_->context()->ClearRenderTargetView(rtv.Get(), kMagenta);
        capture_->context()->Flush();
    }

    ID3D11Texture2D* arrived = nullptr;
    RF_TRY(Import(probe.Get(), &arrived));
    if (!arrived) return Status::Fail("the encoding GPU returned no frame");

    D3D11_TEXTURE2D_DESC staging = td;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ComPtr<ID3D11Texture2D> readback;
    RF_HR(encode_->device()->CreateTexture2D(&staging, nullptr, &readback));

    D3D11_MAPPED_SUBRESOURCE mapped{};
    std::uint8_t pixel[4]{};
    {
        D3DDevice::ContextLock lock(*encode_);
        encode_->context()->CopyResource(readback.Get(), arrived);
        RF_HR(encode_->context()->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        std::memcpy(pixel, mapped.pData, sizeof(pixel));
        encode_->context()->Unmap(readback.Get(), 0);
    }

    next_ = 0;

    const bool magenta = pixel[0] > 200 && pixel[1] < 60 && pixel[2] > 200;
    if (!magenta)
        return Status::Fail(std::format(
            "the encoding GPU sees b={} g={} r={} where the captured frame is magenta", pixel[0],
            pixel[1], pixel[2]));
    return Status::Ok();
}

void FrameBridge::WaitForCaptureGpu() {
    if (!drawn_) return;

    D3DDevice::ContextLock lock(*capture_);
    capture_->context()->End(drawn_.Get());

    BOOL done = FALSE;
    for (int spins = 0; spins < 2000; ++spins) {
        if (capture_->context()->GetData(drawn_.Get(), &done, sizeof(done), 0) == S_OK && done)
            return;
        ::Sleep(0);
    }
}

Status FrameBridge::Import(ID3D11Texture2D* frame, ID3D11Texture2D** out) {
    if (!frame || !out) return Status::Fail(E_POINTER, "FrameBridge::Import");

    if (!crosses_devices_) {
        *out = frame;
        return Status::Ok();
    }

    const int slot = next_;
    next_ = (next_ + 1) % kSlots;

    const D3D11_BOX box{0, 0, 0, width_, height_, 1};

    if (keyed_ && write_mutex_[slot]->AcquireSync(kKeyWriter, kMutexWaitMs) != WAIT_OBJECT_0)
        return Status::Fail("the encoding GPU is still holding this frame");

    {
        D3DDevice::ContextLock lock(*capture_);
        capture_->context()->CopySubresourceRegion(written_[slot].Get(), 0, 0, 0, 0, frame, 0, &box);
        capture_->context()->Flush();
    }

    if (!keyed_) WaitForCaptureGpu();

    if (keyed_) {
        write_mutex_[slot]->ReleaseSync(kKeyReader);
        if (read_mutex_[slot]->AcquireSync(kKeyReader, kMutexWaitMs) != WAIT_OBJECT_0)
            return Status::Fail("could not take the frame on the encoding adapter");
    }

    {
        D3DDevice::ContextLock lock(*encode_);
        encode_->context()->CopySubresourceRegion(landed_[slot].Get(), 0, 0, 0, 0,
                                                  read_[slot].Get(), 0, &box);
        encode_->context()->Flush();
    }

    if (keyed_) read_mutex_[slot]->ReleaseSync(kKeyWriter);

    *out = landed_[slot].Get();
    return Status::Ok();
}

}
