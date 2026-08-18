#include "rf/gpu/ColorConverter.h"

#include <cmath>

#include "rf/core/Log.h"

using Microsoft::WRL::ComPtr;

namespace rf {

Status ColorConverter::Init(const D3DDevicePtr& device, std::uint32_t src_width,
                            std::uint32_t src_height, DXGI_FORMAT src_format,
                            std::uint32_t dst_width, std::uint32_t dst_height,
                            DXGI_FORMAT dst_format, ColorSpace color) {
    device_ = device;
    src_width_ = src_width;
    src_height_ = src_height;
    src_format_ = src_format;
    dst_width_ = dst_width;
    dst_height_ = dst_height;
    dst_format_ = dst_format;

    passthrough_ = (src_format == dst_format && src_width == dst_width && src_height == dst_height);
    if (passthrough_) {
        RF_INFO("colour conversion not needed (format and size already match)");
        return Status::Ok();
    }

    RF_HR(device_->device()->QueryInterface(IID_PPV_ARGS(&video_device_)));
    RF_HR(device_->context()->QueryInterface(IID_PPV_ARGS(&video_context_)));

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
    desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    desc.InputWidth = src_width_;
    desc.InputHeight = src_height_;
    desc.OutputWidth = dst_width_;
    desc.OutputHeight = dst_height_;

    desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    RF_HR(video_device_->CreateVideoProcessorEnumerator(&desc, &enumerator_));
    RF_HR(video_device_->CreateVideoProcessor(enumerator_.Get(), 0, &processor_));

    D3D11_TEXTURE2D_DESC td{};
    td.Width = dst_width_;
    td.Height = dst_height_;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = dst_format_;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;

    for (int i = 0; i < kPoolSize; ++i) {
        RF_HR(device_->device()->CreateTexture2D(&td, nullptr, &outputs_[i]));
        RF_HR(video_device_->CreateVideoProcessorOutputView(outputs_[i].Get(), enumerator_.Get(),
                                                            &ovd, &output_views_[i]));
    }

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE in_cs{};
    in_cs.Usage = 0;
    in_cs.RGB_Range = 0;
    in_cs.YCbCr_Matrix = 1;
    in_cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
    video_context_->VideoProcessorSetStreamColorSpace(processor_.Get(), 0, &in_cs);

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE out_cs = in_cs;
    const bool rgb_out = dst_format_ == DXGI_FORMAT_B8G8R8A8_UNORM ||
                         dst_format_ == DXGI_FORMAT_R8G8B8A8_UNORM;
    if (!rgb_out) {
        out_cs.RGB_Range = 1;
        out_cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    }
    video_context_->VideoProcessorSetOutputColorSpace(processor_.Get(), &out_cs);

    if (color == ColorSpace::Rec2020Pq) {
        video_context_->VideoProcessorSetStreamColorSpace1(
            processor_.Get(), 0, DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
        video_context_->VideoProcessorSetOutputColorSpace1(
            processor_.Get(), DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020);
    }

    video_context_->VideoProcessorSetStreamFrameFormat(processor_.Get(), 0,
                                                       D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    video_context_->VideoProcessorSetStreamAutoProcessingMode(processor_.Get(), 0, FALSE);

    const double src_aspect = static_cast<double>(src_width_) / src_height_;
    const double dst_aspect = static_cast<double>(dst_width_) / dst_height_;
    if (std::abs(src_aspect - dst_aspect) > 0.001) {
        RECT fitted{0, 0, static_cast<LONG>(dst_width_), static_cast<LONG>(dst_height_)};
        if (src_aspect > dst_aspect) {
            const LONG height = static_cast<LONG>(dst_width_ / src_aspect + 0.5);
            fitted.top = (static_cast<LONG>(dst_height_) - height) / 2;
            fitted.bottom = fitted.top + height;
        } else {
            const LONG width = static_cast<LONG>(dst_height_ * src_aspect + 0.5);
            fitted.left = (static_cast<LONG>(dst_width_) - width) / 2;
            fitted.right = fitted.left + width;
        }

        video_context_->VideoProcessorSetStreamDestRect(processor_.Get(), 0, TRUE, &fitted);

        const D3D11_VIDEO_COLOR black{{0.0f, 0.0f, 0.0f, 1.0f}};
        video_context_->VideoProcessorSetOutputBackgroundColor(processor_.Get(), FALSE, &black);

        RF_INFO("letterboxing {}x{} into {}x{}: {},{} to {},{}", src_width_, src_height_,
                dst_width_, dst_height_, fitted.left, fitted.top, fitted.right, fitted.bottom);
    }

    RF_INFO("colour converter {}x{} fmt {} -> {}x{} fmt {} ({})", src_width_, src_height_,
            static_cast<int>(src_format_), dst_width_, dst_height_, static_cast<int>(dst_format_),
            ToString(color));
    return Status::Ok();
}

Status ColorConverter::InputViewFor(ID3D11Texture2D* src, ID3D11VideoProcessorInputView** out) {
    for (auto& [texture, view] : input_views_) {
        if (texture == src && view) {
            *out = view.Get();
            return Status::Ok();
        }
    }

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
    ivd.FourCC = 0;
    ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ivd.Texture2D.MipSlice = 0;
    ivd.Texture2D.ArraySlice = 0;

    ComPtr<ID3D11VideoProcessorInputView> view;
    RF_HR(video_device_->CreateVideoProcessorInputView(src, enumerator_.Get(), &ivd, &view));

    input_views_[next_input_] = {src, view};
    next_input_ = (next_input_ + 1) % kInputCacheSize;
    *out = view.Get();
    return Status::Ok();
}

Status ColorConverter::Convert(ID3D11Texture2D* src, ID3D11Texture2D** out) {
    if (!src || !out) return Status::Fail(E_POINTER, "ColorConverter::Convert");

    if (passthrough_) {
        *out = src;
        return Status::Ok();
    }

    ID3D11VideoProcessorInputView* input_view = nullptr;
    RF_TRY(InputViewFor(src, &input_view));

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = input_view;

    const int slot = next_output_;
    next_output_ = (next_output_ + 1) % kPoolSize;

    {
        D3DDevice::ContextLock lock(*device_);
        RF_HR(video_context_->VideoProcessorBlt(processor_.Get(), output_views_[slot].Get(), 0, 1,
                                                &stream));
    }

    *out = outputs_[slot].Get();
    return Status::Ok();
}

}
