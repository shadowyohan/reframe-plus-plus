#include "rf/encode/AmfEncoder.h"

#include <windows.h>

#include "rf/core/Log.h"

#ifdef RF_HAS_AMF
#include <components/VideoEncoderAV1.h>
#include <components/VideoEncoderHEVC.h>
#include <components/VideoEncoderVCE.h>
#include <core/Factory.h>
#include <core/Version.h>
#endif

namespace rf {

#ifndef RF_HAS_AMF

struct AmfEncoder::Api {};

AmfEncoder::AmfEncoder(D3DDevicePtr device) : device_(std::move(device)) {}
AmfEncoder::~AmfEncoder() { Close(); }

bool AmfEncoder::Available() { return false; }

Status AmfEncoder::Open(const EncoderConfig&, const PacketCallback&) {
    return Status::Fail("AMF backend not built - configure with -DRF_AMF_SDK_DIR=<AMF SDK>");
}

void AmfEncoder::Close() {}
void AmfEncoder::DestroySession() {}
void AmfEncoder::OutputLoop() {}
Status AmfEncoder::Submit(const CapturedFrame&) { return Status::Fail("AMF backend not built"); }
Status AmfEncoder::RequestKeyframe() { return Status::Ok(); }
Status AmfEncoder::Flush() { return Status::Ok(); }

#else

namespace {

using AmfInitFn = AMF_RESULT(AMF_CDECL_CALL*)(amf_uint64, amf::AMFFactory**);

const char* AmfStatusName(AMF_RESULT r) {
    switch (r) {
        case AMF_OK:                    return "AMF_OK";
        case AMF_FAIL:                  return "AMF_FAIL";
        case AMF_NOT_SUPPORTED:         return "NOT_SUPPORTED";
        case AMF_NO_DEVICE:             return "NO_DEVICE";
        case AMF_INVALID_FORMAT:        return "INVALID_FORMAT";
        case AMF_OUT_OF_MEMORY:         return "OUT_OF_MEMORY";
        case AMF_INPUT_FULL:            return "INPUT_FULL";
        case AMF_DECODER_NO_FREE_SURFACES: return "NO_FREE_SURFACES";
        case AMF_REPEAT:                return "REPEAT";
        case AMF_EOF:                   return "EOF";
        default:                        return "AMF_RESULT";
    }
}

}

struct AmfEncoder::Api {
    HMODULE dll = nullptr;
    amf::AMFFactory* factory = nullptr;
    amf::AMFContextPtr context;
    amf::AMFComponentPtr encoder;

    ~Api() {
        if (encoder) {
            encoder->Terminate();
            encoder = nullptr;
        }
        if (context) {
            context->Terminate();
            context = nullptr;
        }
        if (dll) ::FreeLibrary(dll);
    }
};

#define RF_AMF(expr)                                                                   \
    do {                                                                               \
        const AMF_RESULT rf_amf__ = (expr);                                            \
        if (rf_amf__ != AMF_OK)                                                        \
            return Status::Fail(E_FAIL,                                                \
                                std::string(#expr) + " -> " + AmfStatusName(rf_amf__)); \
    } while (0)

AmfEncoder::AmfEncoder(D3DDevicePtr device) : device_(std::move(device)) {}
AmfEncoder::~AmfEncoder() { Close(); }

bool AmfEncoder::Available() {
    HMODULE dll = ::LoadLibraryW(AMF_DLL_NAME);
    if (!dll) return false;
    const bool has_entry = ::GetProcAddress(dll, AMF_INIT_FUNCTION_NAME) != nullptr;
    ::FreeLibrary(dll);
    return has_entry;
}

namespace {

const wchar_t* ComponentFor(Codec codec) {
    switch (codec) {
        case Codec::HEVC: return AMFVideoEncoder_HEVC;
        case Codec::AV1:  return AMFVideoEncoder_AV1;
        case Codec::H264: break;
    }
    return AMFVideoEncoderVCE_AVC;
}

void ConfigureH264(amf::AMFComponent* enc, const EncoderConfig& config, std::uint32_t gop,
                   std::uint32_t peak) {
    enc->SetProperty(AMF_VIDEO_ENCODER_USAGE, AMF_VIDEO_ENCODER_USAGE_TRANSCODING);
    enc->SetProperty(AMF_VIDEO_ENCODER_PROFILE, AMF_VIDEO_ENCODER_PROFILE_HIGH);
    enc->SetProperty(AMF_VIDEO_ENCODER_QUALITY_PRESET,
                     config.quality_vs_speed >= 80 ? AMF_VIDEO_ENCODER_QUALITY_PRESET_QUALITY
                     : config.quality_vs_speed >= 40 ? AMF_VIDEO_ENCODER_QUALITY_PRESET_BALANCED
                                                     : AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED);
    enc->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE, ::AMFConstructRate(config.format.fps_num, config.format.fps_den));
    enc->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, static_cast<amf_int64>(config.bitrate_kbps) * 1000);
    enc->SetProperty(AMF_VIDEO_ENCODER_PEAK_BITRATE, static_cast<amf_int64>(peak) * 1000);
    enc->SetProperty(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD,
                     config.rate_control == RateControl::VBR
                         ? AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR
                         : AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR);
    enc->SetProperty(AMF_VIDEO_ENCODER_IDR_PERIOD, static_cast<amf_int64>(gop));
    enc->SetProperty(AMF_VIDEO_ENCODER_B_PIC_PATTERN, static_cast<amf_int64>(0));
    enc->SetProperty(AMF_VIDEO_ENCODER_DE_BLOCKING_FILTER, true);
    enc->SetProperty(AMF_VIDEO_ENCODER_ENABLE_VBAQ, true);
}

void ConfigureHevc(amf::AMFComponent* enc, const EncoderConfig& config, std::uint32_t gop,
                   std::uint32_t peak) {
    enc->SetProperty(AMF_VIDEO_ENCODER_HEVC_USAGE, AMF_VIDEO_ENCODER_HEVC_USAGE_TRANSCODING);
    enc->SetProperty(AMF_VIDEO_ENCODER_HEVC_PROFILE, AMF_VIDEO_ENCODER_HEVC_PROFILE_MAIN);
    enc->SetProperty(AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET,
                     config.quality_vs_speed >= 80 ? AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_QUALITY
                     : config.quality_vs_speed >= 40 ? AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_BALANCED
                                                     : AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_SPEED);
    enc->SetProperty(AMF_VIDEO_ENCODER_HEVC_FRAMERATE,
                     ::AMFConstructRate(config.format.fps_num, config.format.fps_den));
    enc->SetProperty(AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE, static_cast<amf_int64>(config.bitrate_kbps) * 1000);
    enc->SetProperty(AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE, static_cast<amf_int64>(peak) * 1000);
    enc->SetProperty(AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD,
                     config.rate_control == RateControl::VBR
                         ? AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR
                         : AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR);
    enc->SetProperty(AMF_VIDEO_ENCODER_HEVC_GOP_SIZE, static_cast<amf_int64>(gop));
    enc->SetProperty(AMF_VIDEO_ENCODER_HEVC_NUM_GOPS_PER_IDR, static_cast<amf_int64>(1));
    enc->SetProperty(AMF_VIDEO_ENCODER_HEVC_HEADER_INSERTION_MODE,
                     AMF_VIDEO_ENCODER_HEVC_HEADER_INSERTION_MODE_IDR_ALIGNED);
    enc->SetProperty(AMF_VIDEO_ENCODER_HEVC_ENABLE_VBAQ, true);
}

void ConfigureAv1(amf::AMFComponent* enc, const EncoderConfig& config, std::uint32_t gop,
                  std::uint32_t peak) {
    enc->SetProperty(AMF_VIDEO_ENCODER_AV1_USAGE, AMF_VIDEO_ENCODER_AV1_USAGE_TRANSCODING);
    enc->SetProperty(AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET,
                     config.quality_vs_speed >= 80 ? AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_QUALITY
                     : config.quality_vs_speed >= 40 ? AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_BALANCED
                                                     : AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_SPEED);
    enc->SetProperty(AMF_VIDEO_ENCODER_AV1_ALIGNMENT_MODE, AMF_VIDEO_ENCODER_AV1_ALIGNMENT_MODE_NO_RESTRICTIONS);
    enc->SetProperty(AMF_VIDEO_ENCODER_AV1_FRAMERATE,
                     ::AMFConstructRate(config.format.fps_num, config.format.fps_den));
    enc->SetProperty(AMF_VIDEO_ENCODER_AV1_TARGET_BITRATE, static_cast<amf_int64>(config.bitrate_kbps) * 1000);
    enc->SetProperty(AMF_VIDEO_ENCODER_AV1_PEAK_BITRATE, static_cast<amf_int64>(peak) * 1000);
    enc->SetProperty(AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD,
                     config.rate_control == RateControl::VBR
                         ? AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR
                         : AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_CBR);
    enc->SetProperty(AMF_VIDEO_ENCODER_AV1_GOP_SIZE, static_cast<amf_int64>(gop));
    enc->SetProperty(AMF_VIDEO_ENCODER_AV1_HEADER_INSERTION_MODE,
                     AMF_VIDEO_ENCODER_AV1_HEADER_INSERTION_MODE_KEY_FRAME_ALIGNED);
    enc->SetProperty(AMF_VIDEO_ENCODER_AV1_AQ_MODE, AMF_VIDEO_ENCODER_AV1_AQ_MODE_CAQ);
}

const wchar_t* ExtraDataProperty(Codec codec) {
    switch (codec) {
        case Codec::HEVC: return AMF_VIDEO_ENCODER_HEVC_EXTRADATA;
        case Codec::AV1:  return AMF_VIDEO_ENCODER_AV1_EXTRA_DATA;
        case Codec::H264: break;
    }
    return AMF_VIDEO_ENCODER_EXTRADATA;
}

bool IsKeyframe(amf::AMFBuffer* buffer, Codec codec) {
    amf_int64 type = -1;
    switch (codec) {
        case Codec::HEVC:
            buffer->GetProperty(AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE, &type);
            return type == AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_IDR ||
                   type == AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_I;
        case Codec::AV1:
            buffer->GetProperty(AMF_VIDEO_ENCODER_AV1_OUTPUT_FRAME_TYPE, &type);
            return type == AMF_VIDEO_ENCODER_AV1_OUTPUT_FRAME_TYPE_KEY;
        case Codec::H264: break;
    }
    buffer->GetProperty(AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE, &type);
    return type == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_IDR || type == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_I;
}

}

Status AmfEncoder::Open(const EncoderConfig& config, const PacketCallback& on_packet) {
    config_ = config;
    on_packet_ = on_packet;
    epoch_ = config.epoch;

    api_ = std::make_unique<Api>();

    api_->dll = ::LoadLibraryW(AMF_DLL_NAME);
    if (!api_->dll) return Status::Fail("amfrt64.dll not present (AMD driver missing)");

    auto init = reinterpret_cast<AmfInitFn>(::GetProcAddress(api_->dll, AMF_INIT_FUNCTION_NAME));
    if (!init) return Status::Fail("AMFInit not exported");

    RF_AMF(init(AMF_FULL_VERSION, &api_->factory));
    RF_AMF(api_->factory->CreateContext(&api_->context));
    RF_AMF(api_->context->InitDX11(device_->device(), amf::AMF_DX11_0));
    RF_AMF(api_->factory->CreateComponent(api_->context, ComponentFor(config.format.codec), &api_->encoder));

    const auto& f = config_.format;
    const std::uint32_t fps = std::max(1u, f.fps_num / std::max(1u, f.fps_den));
    const std::uint32_t gop = std::max(1u, fps * config_.keyframe_interval_ms / 1000);

    amf::AMFComponent* enc = api_->encoder;
    const std::uint32_t peak =
        config_.max_bitrate_kbps ? config_.max_bitrate_kbps : config_.bitrate_kbps * 3 / 2;
    switch (f.codec) {
        case Codec::HEVC: ConfigureHevc(enc, config_, gop, peak); break;
        case Codec::AV1:  ConfigureAv1(enc, config_, gop, peak); break;
        case Codec::H264: ConfigureH264(enc, config_, gop, peak); break;
    }

    RF_AMF(enc->Init(amf::AMF_SURFACE_BGRA, static_cast<amf_int32>(f.width),
                     static_cast<amf_int32>(f.height)));

    amf::AMFVariant extradata;
    if (enc->GetProperty(ExtraDataProperty(f.codec), &extradata) == AMF_OK &&
        extradata.type == amf::AMF_VARIANT_INTERFACE) {
        amf::AMFBufferPtr header(extradata.pInterface);
        if (header) {
            const auto* bytes = static_cast<const std::uint8_t*>(header->GetNative());
            codec_private_.data.assign(bytes, bytes + header->GetSize());
        }
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = f.width;
    td.Height = f.height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    for (auto& input : inputs_) RF_HR(device_->device()->CreateTexture2D(&td, nullptr, &input));

    open_ = true;
    running_ = true;
    output_thread_ = std::thread([this] { OutputLoop(); });

    RF_INFO("AMF session up: {}x{}@{} {} kbps GOP={} preset {} input=BGRA direct", f.width, f.height,
            fps, config_.bitrate_kbps, gop,
            config_.quality_vs_speed >= 80   ? "quality"
            : config_.quality_vs_speed >= 40 ? "balanced"
                                             : "speed");
    return Status::Ok();
}

void AmfEncoder::OutputLoop() {
    ::SetThreadDescription(::GetCurrentThread(), L"rf-encode-amf");

    while (running_.load(std::memory_order_relaxed)) {
        amf::AMFDataPtr data;
        const AMF_RESULT result = api_->encoder->QueryOutput(&data);

        if (result == AMF_REPEAT || (result == AMF_OK && !data)) {
            ::Sleep(1);
            continue;
        }
        if (result == AMF_EOF) break;
        if (result != AMF_OK) {
            RF_WARN("AMF QueryOutput: {}", AmfStatusName(result));
            ::Sleep(1);
            continue;
        }

        amf::AMFBufferPtr buffer(data);
        if (!buffer) continue;

        auto packet = std::make_shared<Packet>();
        packet->kind = MediaKind::Video;
        packet->track = 0;

        const auto* bytes = static_cast<const std::uint8_t*>(buffer->GetNative());
        packet->data.assign(bytes, bytes + buffer->GetSize());
        packet->pts = static_cast<Ticks100ns>(buffer->GetPts());
        packet->dts = packet->pts;
        packet->duration =
            kOneSecond100ns * config_.format.fps_den / std::max(1u, config_.format.fps_num);

        packet->keyframe = IsKeyframe(buffer, config_.format.codec);

        ++stats_.frames_encoded;
        stats_.bytes_out += packet->size();
        if (on_packet_) on_packet_(std::move(packet));
    }
}

Status AmfEncoder::Submit(const CapturedFrame& frame) {
    if (!open_) return Status::Fail("encoder not open");

    std::scoped_lock lock(submit_mutex_);

    ID3D11Texture2D* source = frame.texture && frame.content_changed ? frame.texture
                                                                    : last_input_.Get();
    if (!source) return Status::Ok();

    ID3D11Texture2D* input = inputs_[next_input_].Get();
    next_input_ = (next_input_ + 1) % kInputTextures;
    {
        const D3D11_BOX box{0, 0, 0, config_.format.width, config_.format.height, 1};
        D3DDevice::ContextLock ctx_lock(*device_);
        device_->context()->CopySubresourceRegion(input, 0, 0, 0, 0, source, 0, &box);
        device_->context()->Flush();
    }
    last_input_ = input;

    amf::AMFSurfacePtr surface;
    if (api_->context->CreateSurfaceFromDX11Native(input, &surface, nullptr) != AMF_OK)
        return Status::Fail("AMF could not wrap the captured texture");

    surface->SetPts(static_cast<amf_pts>(frame.timestamp - epoch_));
    surface->SetDuration(static_cast<amf_pts>(kOneSecond100ns * config_.format.fps_den /
                                              std::max(1u, config_.format.fps_num)));
    if (force_idr_.exchange(false)) {
        switch (config_.format.codec) {
            case Codec::HEVC:
                surface->SetProperty(AMF_VIDEO_ENCODER_HEVC_FORCE_PICTURE_TYPE,
                                     AMF_VIDEO_ENCODER_HEVC_PICTURE_TYPE_IDR);
                break;
            case Codec::AV1:
                surface->SetProperty(AMF_VIDEO_ENCODER_AV1_FORCE_FRAME_TYPE,
                                     AMF_VIDEO_ENCODER_AV1_FORCE_FRAME_TYPE_KEY);
                break;
            case Codec::H264:
                surface->SetProperty(AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE,
                                     AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR);
                break;
        }
    }

    ++stats_.frames_submitted;

    const AMF_RESULT result = api_->encoder->SubmitInput(surface);
    if (result == AMF_INPUT_FULL || result == AMF_DECODER_NO_FREE_SURFACES) {
        ++stats_.frames_dropped;
        return Status::Ok();
    }
    if (result != AMF_OK) {
        ++stats_.frames_dropped;
        return Status::Fail(std::string("AMF SubmitInput -> ") + AmfStatusName(result));
    }
    return Status::Ok();
}

Status AmfEncoder::RequestKeyframe() {
    force_idr_ = true;
    return Status::Ok();
}

Status AmfEncoder::Flush() { return Status::Ok(); }

void AmfEncoder::DestroySession() {
    running_ = false;
    if (output_thread_.joinable()) output_thread_.join();

    api_.reset();
    for (auto& input : inputs_) input.Reset();
    last_input_.Reset();
    next_input_ = 0;
    open_ = false;
}

void AmfEncoder::Close() { DestroySession(); }

#endif

}
