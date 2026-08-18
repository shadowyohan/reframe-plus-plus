#include "rf/capture/DxgiDuplCapture.h"

#include "rf/core/Log.h"

using Microsoft::WRL::ComPtr;

namespace rf {
namespace {
constexpr UINT kAcquireTimeoutMs = 16;
}

DxgiDuplCapture::DxgiDuplCapture(D3DDevicePtr device) : device_(std::move(device)) {}

DxgiDuplCapture::~DxgiDuplCapture() { Stop(); }

Status DxgiDuplCapture::CreateDuplication() {
    ReleaseDuplication();

    ComPtr<IDXGIOutput> output;
    if (target_.kind == CaptureTarget::Kind::Display && target_.hmonitor) {
        for (UINT i = 0; device_->adapter()->EnumOutputs(i, output.ReleaseAndGetAddressOf()) !=
                         DXGI_ERROR_NOT_FOUND;
             ++i) {
            DXGI_OUTPUT_DESC desc{};
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == target_.hmonitor) break;
        }
    }
    if (!output) RF_HR(device_->adapter()->EnumOutputs(0, &output));
    RF_HR(output.As(&output_));

    if (DXGI_OUTPUT_DESC desc{}; SUCCEEDED(output_->GetDesc(&desc))) {
        desktop_x_ = desc.DesktopCoordinates.left;
        desktop_y_ = desc.DesktopCoordinates.top;
    }

    static const DXGI_FORMAT kFormats[] = {
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_FORMAT_R10G10B10A2_UNORM,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
    };

    ComPtr<IDXGIOutput5> output5;
    HRESULT hr = output.As(&output5);
    if (SUCCEEDED(hr)) {
        hr = output5->DuplicateOutput1(device_->device(), 0, ARRAYSIZE(kFormats), kFormats, &dupl_);
    }
    if (FAILED(hr)) {

        RF_WARN("DuplicateOutput1 failed (0x{:08X}), falling back", static_cast<unsigned>(hr));
        RF_HR(output_->DuplicateOutput(device_->device(), &dupl_));
    }

    DXGI_OUTDUPL_DESC desc{};
    dupl_->GetDesc(&desc);
    width_ = desc.ModeDesc.Width;
    height_ = desc.ModeDesc.Height;
    format_ = desc.ModeDesc.Format;

    RF_INFO("desktop duplication {}x{} fmt={} desktop_image_in_system_memory={}", width_, height_,
            static_cast<int>(format_), desc.DesktopImageInSystemMemory != 0);
    return Status::Ok();
}

void DxgiDuplCapture::ReleaseDuplication() {
    if (dupl_) dupl_->ReleaseFrame();
    dupl_.Reset();
}

Status DxgiDuplCapture::Start(const CaptureTarget& target, const FrameCallback& on_frame) {
    if (running_) return Status::Fail("capture already running");
    target_ = target;
    on_frame_ = on_frame;

    RF_TRY(CreateDuplication());

    running_ = true;
    thread_ = std::thread([this] { CaptureLoop(); });
    return Status::Ok();
}

void DxgiDuplCapture::Stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    ReleaseDuplication();
}

void DxgiDuplCapture::CaptureLoop() {
    ::SetThreadDescription(::GetCurrentThread(), L"rf-capture-dxgi");
    MmcssScope mmcss(L"Capture");

    Ticks100ns last_present = 0;

    while (running_.load(std::memory_order_relaxed)) {
        if (!dupl_) {
            if (auto s = CreateDuplication(); !s.ok()) {
                RF_WARN("duplication rebuild failed: {}", s.str());
                ::Sleep(100);
                continue;
            }
            ++stats_.recoveries;
        }

        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> resource;
        const HRESULT hr = dupl_->AcquireNextFrame(kAcquireTimeoutMs, &info, &resource);

        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {

            continue;
        }
        if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) {

            RF_WARN("duplication lost (0x{:08X}) - rebuilding", static_cast<unsigned>(hr));
            ReleaseDuplication();
            continue;
        }
        if (FAILED(hr)) {
            RF_ERROR("AcquireNextFrame failed 0x{:08X}", static_cast<unsigned>(hr));
            ReleaseDuplication();
            continue;
        }

        const Ticks100ns present = info.LastPresentTime.QuadPart;

        const bool has_new_image = info.AccumulatedFrames > 0 && present != 0 && present != last_present;

        if (has_new_image) {
            last_present = present;

            ComPtr<ID3D11Texture2D> acquired;
            if (SUCCEEDED(resource.As(&acquired))) {

                ID3D11Texture2D* picture = acquired.Get();
                if (target_.capture_cursor) {
                    if (!cursor_ready_) {
                        if (auto s = cursor_.Init(device_, width_, height_, format_); s.ok())
                            cursor_ready_ = true;
                        else
                            RF_WARN("no cursor in the recording: {}", s.str());
                    }
                    if (cursor_ready_) {
                        ID3D11Texture2D* composed = nullptr;
                        if (auto s = cursor_.Compose(acquired.Get(), desktop_x_, desktop_y_,
                                                     &composed);
                            s.ok() && composed)
                            picture = composed;
                    }
                }

                CapturedFrame frame;
                frame.texture = picture;
                frame.width = width_;
                frame.height = height_;
                frame.format = format_;
                frame.timestamp = QpcTo100ns(QpcNow());
                frame.content_changed = true;
                frame.frame_index = frame_index_++;

                ++stats_.frames_captured;
                if (on_frame_) on_frame_(frame);
            }
        } else {
            ++stats_.frames_repeated;
        }

        dupl_->ReleaseFrame();
    }

    RF_INFO("capture loop ended: {} frames, {} repeats, {} recoveries", stats_.frames_captured,
            stats_.frames_repeated, stats_.recoveries);
}

}
