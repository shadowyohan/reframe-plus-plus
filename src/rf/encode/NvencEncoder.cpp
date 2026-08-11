#include "rf/encode/NvencEncoder.h"

#include <windows.h>

#include <algorithm>
#include <format>

#include "rf/core/Log.h"

#ifdef RF_HAS_NVENC
#include <nvEncodeAPI.h>
#endif

namespace rf {

#ifndef RF_HAS_NVENC

struct NvencEncoder::Api {};
NvencEncoder::NvencEncoder(D3DDevicePtr device) : device_(std::move(device)) {}
NvencEncoder::~NvencEncoder() { Close(); }
bool NvencEncoder::Available() { return false; }
Status NvencEncoder::Open(const EncoderConfig&, const PacketCallback&) {
    return Status::Fail("NVENC backend not built");
}
void NvencEncoder::Close() {}
Status NvencEncoder::Submit(const CapturedFrame&) { return Status::Fail("not built"); }
Status NvencEncoder::RequestKeyframe() { return Status::Ok(); }
Status NvencEncoder::Flush() { return Status::Ok(); }
Status NvencEncoder::InitSession() { return Status::Fail("not built"); }
Status NvencEncoder::DrainSlot(int) { return Status::Fail("not built"); }
void NvencEncoder::DestroySession() {}

#else

namespace {

using PfnCreateInstance = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);

const char* NvStatusName(NVENCSTATUS s) {
    switch (s) {
        case NV_ENC_SUCCESS:                      return "NV_ENC_SUCCESS";
        case NV_ENC_ERR_UNSUPPORTED_DEVICE:       return "UNSUPPORTED_DEVICE";
        case NV_ENC_ERR_INVALID_ENCODERDEVICE:    return "INVALID_ENCODERDEVICE";
        case NV_ENC_ERR_INVALID_VERSION:          return "INVALID_VERSION (driver older than SDK)";
        case NV_ENC_ERR_OUT_OF_MEMORY:            return "OUT_OF_MEMORY";
        case NV_ENC_ERR_UNSUPPORTED_PARAM:        return "UNSUPPORTED_PARAM";
        case NV_ENC_ERR_INVALID_PARAM:            return "INVALID_PARAM";
        default:                                  return "NVENCSTATUS";
    }
}

}

struct NvencEncoder::Api {
    HMODULE dll = nullptr;
    NV_ENCODE_API_FUNCTION_LIST fn{};
    void* encoder = nullptr;
    NV_ENC_REGISTERED_PTR registered[2] = {};
    NV_ENC_INPUT_PTR mapped[2] = {};
    NV_ENC_OUTPUT_PTR bitstream[2] = {};

    ~Api() {
        if (encoder) {
            for (int i = 0; i < 2; ++i) {
                if (mapped[i]) fn.nvEncUnmapInputResource(encoder, mapped[i]);
                if (registered[i]) fn.nvEncUnregisterResource(encoder, registered[i]);
                if (bitstream[i]) fn.nvEncDestroyBitstreamBuffer(encoder, bitstream[i]);
            }
            fn.nvEncDestroyEncoder(encoder);
        }
        if (dll) ::FreeLibrary(dll);
    }
};

#define RF_NV(expr)                                                                     \
    do {                                                                                \
        const NVENCSTATUS rf_nv__ = (expr);                                             \
        if (rf_nv__ != NV_ENC_SUCCESS)                                                  \
            return Status::Fail(E_FAIL,                                                 \
                                std::string(#expr) + " -> " + NvStatusName(rf_nv__));   \
    } while (0)

NvencEncoder::NvencEncoder(D3DDevicePtr device) : device_(std::move(device)) {}
NvencEncoder::~NvencEncoder() { Close(); }

bool NvencEncoder::Available() {
    HMODULE dll = ::LoadLibraryW(L"nvEncodeAPI64.dll");
    if (!dll) return false;
    const bool has_entry = ::GetProcAddress(dll, "NvEncodeAPICreateInstance") != nullptr;
    ::FreeLibrary(dll);
    return has_entry;
}

Status NvencEncoder::InitSession() {
    api_ = std::make_unique<Api>();

    api_->dll = ::LoadLibraryW(L"nvEncodeAPI64.dll");
    if (!api_->dll) return Status::Fail("nvEncodeAPI64.dll not present (NVIDIA driver missing)");

    if (auto max_ver = reinterpret_cast<NVENCSTATUS(NVENCAPI*)(uint32_t*)>(
            ::GetProcAddress(api_->dll, "NvEncodeAPIGetMaxSupportedVersion"))) {
        uint32_t supported = 0;
        max_ver(&supported);
        constexpr uint32_t kBuiltAgainst = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
        if (supported < kBuiltAgainst)
            return Status::Fail(std::format(
                "driver supports NVENC API {}.{}, built against {}.{} - update the driver",
                supported >> 4, supported & 0xF, NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION));
    }

    auto create = reinterpret_cast<PfnCreateInstance>(
        ::GetProcAddress(api_->dll, "NvEncodeAPICreateInstance"));
    if (!create) return Status::Fail("NvEncodeAPICreateInstance not exported");

    api_->fn.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    RF_NV(create(&api_->fn));

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS session{};
    session.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    session.device = device_->device();
    session.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    session.apiVersion = NVENCAPI_VERSION;
    RF_NV(api_->fn.nvEncOpenEncodeSessionEx(&session, &api_->encoder));
    return Status::Ok();
}

Status NvencEncoder::Open(const EncoderConfig& config, const PacketCallback& on_packet) {
    config_ = config;
    on_packet_ = on_packet;
    epoch_ = config.epoch;

    if (config.format.codec != Codec::H264)
        return Status::Fail("NVENC backend currently implements H.264 only");

    RF_TRY(InitSession());

    const auto& f = config_.format;
    const std::uint32_t fps = std::max(1u, f.fps_num / std::max(1u, f.fps_den));
    const std::uint32_t gop = std::max(1u, fps * config_.keyframe_interval_ms / 1000);

    const bool high_rate = fps > 70;
    const std::uint32_t q = high_rate ? std::min(config_.quality_vs_speed, 50u)
                                      : config_.quality_vs_speed;
    const GUID preset = q >= 80   ? NV_ENC_PRESET_P6_GUID
                        : q >= 60 ? NV_ENC_PRESET_P5_GUID
                        : q >= 40 ? NV_ENC_PRESET_P4_GUID
                                  : NV_ENC_PRESET_P3_GUID;

    NV_ENC_PRESET_CONFIG preset_cfg{};
    preset_cfg.version = NV_ENC_PRESET_CONFIG_VER;
    preset_cfg.presetCfg.version = NV_ENC_CONFIG_VER;
    RF_NV(api_->fn.nvEncGetEncodePresetConfigEx(api_->encoder, NV_ENC_CODEC_H264_GUID, preset,
                                                NV_ENC_TUNING_INFO_LOW_LATENCY, &preset_cfg));

    NV_ENC_CONFIG enc_cfg = preset_cfg.presetCfg;
    enc_cfg.gopLength = gop;
    enc_cfg.frameIntervalP = 1;
    enc_cfg.rcParams.rateControlMode =
        config_.rate_control == RateControl::VBR ? NV_ENC_PARAMS_RC_VBR : NV_ENC_PARAMS_RC_CBR;
    enc_cfg.rcParams.averageBitRate = config_.bitrate_kbps * 1000;
    enc_cfg.rcParams.maxBitRate = (config_.max_bitrate_kbps ? config_.max_bitrate_kbps
                                                            : config_.bitrate_kbps * 3 / 2) * 1000;

    enc_cfg.rcParams.vbvBufferSize = enc_cfg.rcParams.averageBitRate / fps * 4;
    enc_cfg.rcParams.enableAQ = 1;

    enc_cfg.rcParams.multiPass =
        high_rate ? NV_ENC_MULTI_PASS_DISABLED : NV_ENC_TWO_PASS_QUARTER_RESOLUTION;
    enc_cfg.encodeCodecConfig.h264Config.idrPeriod = gop;
    enc_cfg.encodeCodecConfig.h264Config.repeatSPSPPS = 1;

    auto& vui = enc_cfg.encodeCodecConfig.h264Config.h264VUIParameters;
    vui.videoSignalTypePresentFlag = 1;
    vui.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
    vui.videoFullRangeFlag = 0;
    vui.colourDescriptionPresentFlag = 1;
    const bool hdr = config_.format.color == ColorSpace::Rec2020Pq;
    vui.colourPrimaries =
        hdr ? NV_ENC_VUI_COLOR_PRIMARIES_BT2020 : NV_ENC_VUI_COLOR_PRIMARIES_BT709;
    vui.transferCharacteristics = hdr ? NV_ENC_VUI_TRANSFER_CHARACTERISTIC_SMPTE2084
                                      : NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
    vui.colourMatrix =
        hdr ? NV_ENC_VUI_MATRIX_COEFFS_BT2020_NCL : NV_ENC_VUI_MATRIX_COEFFS_BT709;

    NV_ENC_INITIALIZE_PARAMS init{};
    init.version = NV_ENC_INITIALIZE_PARAMS_VER;
    init.encodeGUID = NV_ENC_CODEC_H264_GUID;
    init.presetGUID = preset;
    init.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;
    init.encodeWidth = f.width;
    init.encodeHeight = f.height;
    init.darWidth = f.width;
    init.darHeight = f.height;
    init.frameRateNum = f.fps_num;
    init.frameRateDen = f.fps_den;
    init.enablePTD = 1;
    init.encodeConfig = &enc_cfg;
    if (api_->fn.nvEncInitializeEncoder(api_->encoder, &init) != NV_ENC_SUCCESS) {
        enc_cfg.rcParams.multiPass = NV_ENC_MULTI_PASS_DISABLED;
        RF_NV(api_->fn.nvEncInitializeEncoder(api_->encoder, &init));
        RF_INFO("NVENC: multipass not supported here - continuing single-pass");
    }

    RF_TRY(converter_.Init(device_, f.width, f.height, DXGI_FORMAT_B8G8R8A8_UNORM, f.width,
                           f.height, DXGI_FORMAT_NV12, f.color));

    D3D11_TEXTURE2D_DESC td{};
    td.Width = f.width;
    td.Height = f.height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;

    for (int i = 0; i < 2; ++i) {
        RF_HR(device_->device()->CreateTexture2D(&td, nullptr, &input_[i]));

        NV_ENC_REGISTER_RESOURCE reg{};
        reg.version = NV_ENC_REGISTER_RESOURCE_VER;
        reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        reg.resourceToRegister = input_[i].Get();
        reg.width = f.width;
        reg.height = f.height;
        reg.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
        reg.bufferUsage = NV_ENC_INPUT_IMAGE;
        RF_NV(api_->fn.nvEncRegisterResource(api_->encoder, &reg));
        api_->registered[i] = reg.registeredResource;

        NV_ENC_CREATE_BITSTREAM_BUFFER bs{};
        bs.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        RF_NV(api_->fn.nvEncCreateBitstreamBuffer(api_->encoder, &bs));
        api_->bitstream[i] = bs.bitstreamBuffer;
    }
    parity_ = 0;
    pending_ = false;

    std::uint8_t header[512];
    std::uint32_t header_size = 0;
    NV_ENC_SEQUENCE_PARAM_PAYLOAD spp{};
    spp.version = NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER;
    spp.spsppsBuffer = header;
    spp.inBufferSize = sizeof(header);
    spp.outSPSPPSPayloadSize = &header_size;
    if (api_->fn.nvEncGetSequenceParams(api_->encoder, &spp) == NV_ENC_SUCCESS && header_size)
        codec_private_.data.assign(header, header + header_size);

    open_ = true;
    RF_INFO("NVENC session up: {}x{}@{} {} kbps GOP={} preset P{} AQ=on", f.width, f.height, fps,
            config_.bitrate_kbps, gop,
            config_.quality_vs_speed >= 80 ? 6 : config_.quality_vs_speed >= 60 ? 5
                                            : config_.quality_vs_speed >= 40 ? 4 : 3);
    return Status::Ok();
}

Status NvencEncoder::DrainSlot(int slot) {
    NV_ENC_LOCK_BITSTREAM lock{};
    lock.version = NV_ENC_LOCK_BITSTREAM_VER;
    lock.outputBitstream = api_->bitstream[slot];
    RF_NV(api_->fn.nvEncLockBitstream(api_->encoder, &lock));

    auto packet = std::make_shared<Packet>();
    packet->kind = MediaKind::Video;
    packet->data.assign(
        static_cast<std::uint8_t*>(lock.bitstreamBufferPtr),
        static_cast<std::uint8_t*>(lock.bitstreamBufferPtr) + lock.bitstreamSizeInBytes);
    packet->pts = static_cast<Ticks100ns>(lock.outputTimeStamp) - epoch_;
    packet->dts = packet->pts;
    packet->duration =
        kOneSecond100ns * config_.format.fps_den / std::max(1u, config_.format.fps_num);
    packet->keyframe =
        lock.pictureType == NV_ENC_PIC_TYPE_IDR || lock.pictureType == NV_ENC_PIC_TYPE_I;

    api_->fn.nvEncUnlockBitstream(api_->encoder, api_->bitstream[slot]);
    if (api_->mapped[slot]) {
        api_->fn.nvEncUnmapInputResource(api_->encoder, api_->mapped[slot]);
        api_->mapped[slot] = nullptr;
    }

    ++stats_.frames_encoded;
    stats_.bytes_out += packet->size();
    if (on_packet_) on_packet_(std::move(packet));
    return Status::Ok();
}

Status NvencEncoder::Submit(const CapturedFrame& frame) {
    if (!open_) return Status::Fail("encoder not open");
    std::scoped_lock serialize(encode_mutex_);
    const Ticks100ns start = Now100ns();
    const int slot = parity_;

    if (frame.content_changed || !have_input_) {

        ID3D11Texture2D* nv12 = nullptr;
        RF_TRY(converter_.Convert(frame.texture, &nv12));
        D3DDevice::ContextLock lock(*device_);
        device_->context()->CopyResource(input_[slot].Get(), nv12);
        have_input_ = true;
    } else {

        D3DDevice::ContextLock lock(*device_);
        device_->context()->CopyResource(input_[slot].Get(), input_[slot ^ 1].Get());
    }

    NV_ENC_MAP_INPUT_RESOURCE map{};
    map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    map.registeredResource = api_->registered[slot];
    RF_NV(api_->fn.nvEncMapInputResource(api_->encoder, &map));
    api_->mapped[slot] = map.mappedResource;

    NV_ENC_PIC_PARAMS pic{};
    pic.version = NV_ENC_PIC_PARAMS_VER;
    pic.inputBuffer = map.mappedResource;
    pic.bufferFmt = map.mappedBufferFmt;
    pic.inputWidth = config_.format.width;
    pic.inputHeight = config_.format.height;
    pic.outputBitstream = api_->bitstream[slot];
    pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pic.inputTimeStamp = static_cast<uint64_t>(frame.timestamp);
    if (force_idr_.exchange(false))
        pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;

    ++stats_.frames_submitted;
    const NVENCSTATUS enc = api_->fn.nvEncEncodePicture(api_->encoder, &pic);
    if (enc != NV_ENC_SUCCESS) {
        api_->fn.nvEncUnmapInputResource(api_->encoder, map.mappedResource);
        api_->mapped[slot] = nullptr;
        ++stats_.frames_dropped;
        return Status::Fail(std::string("nvEncEncodePicture -> ") + NvStatusName(enc));
    }

    Status drained = Status::Ok();
    if (pending_) drained = DrainSlot(slot ^ 1);
    pending_ = true;
    parity_ ^= 1;

    stats_.avg_encode_ms = 0.95 * stats_.avg_encode_ms + 0.05 * Ticks100nsToMs(Now100ns() - start);
    return drained;
}

Status NvencEncoder::RequestKeyframe() {
    force_idr_ = true;
    return Status::Ok();
}

Status NvencEncoder::Flush() {

    std::scoped_lock serialize(encode_mutex_);
    if (open_ && pending_) {
        pending_ = false;
        return DrainSlot(parity_ ^ 1);
    }
    return Status::Ok();
}

void NvencEncoder::DestroySession() {
    if (open_) Flush();
    api_.reset();
    input_[0].Reset();
    input_[1].Reset();
    open_ = false;
    have_input_ = false;
    pending_ = false;
}

void NvencEncoder::Close() { DestroySession(); }

#endif

}
