#include "rf/encode/NvencEncoder.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <vector>

#include "rf/core/Log.h"

#ifdef RF_HAS_NVENC
#include <nvEncodeAPI.h>

namespace rf {
std::uint32_t g_nvenc_minor = NVENCAPI_MINOR_VERSION;
}

#undef NVENCAPI_VERSION
#define NVENCAPI_VERSION (NVENCAPI_MAJOR_VERSION | (rf::g_nvenc_minor << 24))
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
void NvencEncoder::OutputLoop() {}
void NvencEncoder::DestroySession() {}

#else

namespace {

using PfnCreateInstance = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);

constexpr int kDepth = 8;

const char* NvStatusName(NVENCSTATUS s) {
    switch (s) {
        case NV_ENC_SUCCESS:                      return "NV_ENC_SUCCESS";
        case NV_ENC_ERR_UNSUPPORTED_DEVICE:       return "UNSUPPORTED_DEVICE";
        case NV_ENC_ERR_INVALID_ENCODERDEVICE:    return "INVALID_ENCODERDEVICE";
        case NV_ENC_ERR_INVALID_VERSION:          return "INVALID_VERSION (driver older than SDK)";
        case NV_ENC_ERR_OUT_OF_MEMORY:            return "OUT_OF_MEMORY";
        case NV_ENC_ERR_UNSUPPORTED_PARAM:        return "UNSUPPORTED_PARAM";
        case NV_ENC_ERR_INVALID_PARAM:            return "INVALID_PARAM";
        case NV_ENC_ERR_MAP_FAILED:               return "MAP_FAILED";
        case NV_ENC_ERR_ENCODER_BUSY:             return "ENCODER_BUSY";
        default:                                  return "NVENCSTATUS";
    }
}

}

struct NvencEncoder::Api {
    HMODULE dll = nullptr;
    NV_ENCODE_API_FUNCTION_LIST fn{};
    void* encoder = nullptr;

    struct Slot {
        NV_ENC_OUTPUT_PTR bitstream = nullptr;
        HANDLE event = nullptr;
        NV_ENC_INPUT_PTR mapped = nullptr;
        Ticks100ns submitted = 0;
    };
    Slot slots[kDepth];

    std::vector<std::pair<ID3D11Texture2D*, NV_ENC_REGISTERED_PTR>> registrations;

    ~Api() {
        if (encoder) {
            for (Slot& slot : slots) {
                if (slot.mapped) fn.nvEncUnmapInputResource(encoder, slot.mapped);
                if (slot.event) {
                    NV_ENC_EVENT_PARAMS ev{};
                    ev.version = NV_ENC_EVENT_PARAMS_VER;
                    ev.completionEvent = slot.event;
                    fn.nvEncUnregisterAsyncEvent(encoder, &ev);
                    ::CloseHandle(slot.event);
                }
                if (slot.bitstream) fn.nvEncDestroyBitstreamBuffer(encoder, slot.bitstream);
            }
            for (auto& [texture, registered] : registrations)
                fn.nvEncUnregisterResource(encoder, registered);
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
        const uint32_t driver_major = supported >> 4;
        const uint32_t driver_minor = supported & 0xF;

        if (driver_major < NVENCAPI_MAJOR_VERSION)
            return Status::Fail(std::format(
                "driver supports NVENC API {}.{}, built against {}.{} - update the driver",
                driver_major, driver_minor, NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION));

        if (driver_major == NVENCAPI_MAJOR_VERSION && driver_minor < NVENCAPI_MINOR_VERSION) {
            g_nvenc_minor = driver_minor;
            RF_INFO("driver supports NVENC API {}.{} - talking {}.{} to it", driver_major,
                    driver_minor, NVENCAPI_MAJOR_VERSION, driver_minor);
        }
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
    const GUID preset = q >= 80   ? NV_ENC_PRESET_P5_GUID
                        : q >= 60 ? NV_ENC_PRESET_P4_GUID
                        : q >= 40 ? NV_ENC_PRESET_P3_GUID
                                  : NV_ENC_PRESET_P2_GUID;

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

    enc_cfg.rcParams.multiPass = NV_ENC_MULTI_PASS_DISABLED;
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

    init.enableEncodeAsync = 1;
    init.encodeConfig = &enc_cfg;
    if (api_->fn.nvEncInitializeEncoder(api_->encoder, &init) != NV_ENC_SUCCESS) {
        enc_cfg.rcParams.enableAQ = 0;
        RF_NV(api_->fn.nvEncInitializeEncoder(api_->encoder, &init));
        RF_INFO("NVENC: adaptive quantisation not supported here - continuing without it");
    }

    constexpr std::uint32_t kCropSlack = 16;
    direct_rgb_ = config_.input_width == 0 ||
                  (config_.input_width >= f.width && config_.input_height >= f.height &&
                   config_.input_width - f.width <= kCropSlack &&
                   config_.input_height - f.height <= kCropSlack);

    if (!direct_rgb_)
        RF_TRY(converter_.Init(device_, config_.input_width, config_.input_height,
                               DXGI_FORMAT_B8G8R8A8_UNORM, f.width, f.height, DXGI_FORMAT_NV12,
                               f.color));

    D3D11_TEXTURE2D_DESC td{};
    td.Width = f.width;
    td.Height = f.height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = direct_rgb_ ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    for (auto& input : inputs_) RF_HR(device_->device()->CreateTexture2D(&td, nullptr, &input));

    for (int i = 0; i < kDepth; ++i) {
        NV_ENC_CREATE_BITSTREAM_BUFFER bs{};
        bs.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        RF_NV(api_->fn.nvEncCreateBitstreamBuffer(api_->encoder, &bs));
        api_->slots[i].bitstream = bs.bitstreamBuffer;

        api_->slots[i].event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!api_->slots[i].event) return Status::Fail("CreateEvent for the NVENC slot failed");

        NV_ENC_EVENT_PARAMS ev{};
        ev.version = NV_ENC_EVENT_PARAMS_VER;
        ev.completionEvent = api_->slots[i].event;
        RF_NV(api_->fn.nvEncRegisterAsyncEvent(api_->encoder, &ev));

        free_slots_.push_back(i);
    }

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
    running_ = true;
    output_thread_ = std::thread([this] { OutputLoop(); });

    RF_INFO("NVENC session up: {}x{}@{} {} kbps GOP={} preset P{} single-pass async depth={} "
            "input={}",
            f.width, f.height, fps, config_.bitrate_kbps, gop,
            q >= 80 ? 5 : q >= 60 ? 4 : q >= 40 ? 3 : 2, kDepth,
            direct_rgb_ ? "BGRA direct" : "NV12 via the video processor (scaling)");
    return Status::Ok();
}

void NvencEncoder::OutputLoop() {
    ::SetThreadDescription(::GetCurrentThread(), L"rf-encode-nvenc");

    while (true) {
        int slot = -1;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return !inflight_.empty() || !running_; });
            if (inflight_.empty()) break;
            slot = inflight_.front();
            inflight_.pop_front();
        }

        Api::Slot& s = api_->slots[slot];

        if (::WaitForSingleObject(s.event, 5000) != WAIT_OBJECT_0)
            RF_WARN("NVENC did not finish a frame within 5 s - dropping it");
        else {
            NV_ENC_LOCK_BITSTREAM lock_bs{};
            lock_bs.version = NV_ENC_LOCK_BITSTREAM_VER;
            lock_bs.outputBitstream = s.bitstream;
            if (api_->fn.nvEncLockBitstream(api_->encoder, &lock_bs) == NV_ENC_SUCCESS) {
                auto packet = std::make_shared<Packet>();
                packet->kind = MediaKind::Video;
                packet->data.assign(static_cast<std::uint8_t*>(lock_bs.bitstreamBufferPtr),
                                    static_cast<std::uint8_t*>(lock_bs.bitstreamBufferPtr) +
                                        lock_bs.bitstreamSizeInBytes);
                packet->pts = static_cast<Ticks100ns>(lock_bs.outputTimeStamp) - epoch_;
                packet->dts = packet->pts;
                packet->duration = kOneSecond100ns * config_.format.fps_den /
                                   std::max(1u, config_.format.fps_num);
                packet->keyframe = lock_bs.pictureType == NV_ENC_PIC_TYPE_IDR ||
                                   lock_bs.pictureType == NV_ENC_PIC_TYPE_I;

                api_->fn.nvEncUnlockBitstream(api_->encoder, s.bitstream);

                ++stats_.frames_encoded;
                stats_.bytes_out += packet->size();
                stats_.avg_encode_ms =
                    0.95 * stats_.avg_encode_ms + 0.05 * Ticks100nsToMs(Now100ns() - s.submitted);
                if (on_packet_) on_packet_(std::move(packet));
            } else {
                RF_WARN("nvEncLockBitstream failed - frame lost");
            }
        }

        if (s.mapped) {
            api_->fn.nvEncUnmapInputResource(api_->encoder, s.mapped);
            s.mapped = nullptr;
        }

        {
            std::scoped_lock lock(mutex_);
            free_slots_.push_back(slot);
        }
        cv_.notify_all();
    }
}

Status NvencEncoder::Submit(const CapturedFrame& frame) {
    if (!open_) return Status::Fail("encoder not open");

    ID3D11Texture2D* source = nullptr;
    if (frame.texture && frame.content_changed) {
        source = frame.texture;

        if (!direct_rgb_) RF_TRY(converter_.Convert(frame.texture, &source));
    } else {

        source = last_input_;
    }
    if (!source) return Status::Ok();

    ID3D11Texture2D* input = inputs_[next_input_].Get();
    next_input_ = (next_input_ + 1) % kInputTextures;
    {

        const D3D11_BOX box{0, 0, 0, config_.format.width, config_.format.height, 1};
        D3DDevice::ContextLock lock(*device_);
        device_->context()->CopySubresourceRegion(input, 0, 0, 0, 0, source, 0, &box);
    }
    last_input_ = input;

    return EncodeTexture(input, frame.timestamp);
}

Status NvencEncoder::EncodeTexture(ID3D11Texture2D* input, Ticks100ns timestamp) {

    int slot = -1;
    {
        std::scoped_lock lock(mutex_);
        if (free_slots_.empty()) {

            ++stats_.frames_dropped;
            return Status::Ok();
        }
        slot = free_slots_.front();
        free_slots_.pop_front();
        stats_.avg_queue_depth = 0.9 * stats_.avg_queue_depth + 0.1 * inflight_.size();
    }

    Api::Slot& s = api_->slots[slot];

    NV_ENC_REGISTERED_PTR registered = nullptr;
    for (const auto& [texture, ptr] : api_->registrations)
        if (texture == input) registered = ptr;

    if (!registered) {
        NV_ENC_REGISTER_RESOURCE reg{};
        reg.version = NV_ENC_REGISTER_RESOURCE_VER;
        reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        reg.resourceToRegister = input;
        reg.width = config_.format.width;
        reg.height = config_.format.height;
        reg.bufferFormat = direct_rgb_ ? NV_ENC_BUFFER_FORMAT_ARGB : NV_ENC_BUFFER_FORMAT_NV12;
        reg.bufferUsage = NV_ENC_INPUT_IMAGE;
        if (const NVENCSTATUS rs = api_->fn.nvEncRegisterResource(api_->encoder, &reg);
            rs != NV_ENC_SUCCESS) {
            std::scoped_lock lock(mutex_);
            free_slots_.push_back(slot);
            return Status::Fail(std::string("nvEncRegisterResource -> ") + NvStatusName(rs));
        }
        registered = reg.registeredResource;
        api_->registrations.emplace_back(input, registered);
    }

    NV_ENC_MAP_INPUT_RESOURCE map{};
    map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    map.registeredResource = registered;
    if (const NVENCSTATUS ms = api_->fn.nvEncMapInputResource(api_->encoder, &map);
        ms != NV_ENC_SUCCESS) {
        std::scoped_lock lock(mutex_);
        free_slots_.push_back(slot);
        return Status::Fail(std::string("nvEncMapInputResource -> ") + NvStatusName(ms));
    }
    s.mapped = map.mappedResource;
    s.submitted = Now100ns();

    NV_ENC_PIC_PARAMS pic{};
    pic.version = NV_ENC_PIC_PARAMS_VER;
    pic.inputBuffer = map.mappedResource;
    pic.bufferFmt = map.mappedBufferFmt;
    pic.inputWidth = config_.format.width;
    pic.inputHeight = config_.format.height;
    pic.outputBitstream = s.bitstream;
    pic.completionEvent = s.event;
    pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pic.inputTimeStamp = static_cast<uint64_t>(timestamp);
    if (force_idr_.exchange(false))
        pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;

    ++stats_.frames_submitted;
    const NVENCSTATUS enc = api_->fn.nvEncEncodePicture(api_->encoder, &pic);
    if (enc != NV_ENC_SUCCESS) {
        api_->fn.nvEncUnmapInputResource(api_->encoder, s.mapped);
        s.mapped = nullptr;
        ++stats_.frames_dropped;
        std::scoped_lock lock(mutex_);
        free_slots_.push_back(slot);
        return Status::Fail(std::string("nvEncEncodePicture -> ") + NvStatusName(enc));
    }

    {
        std::scoped_lock lock(mutex_);
        inflight_.push_back(slot);
    }
    cv_.notify_all();
    return Status::Ok();
}

Status NvencEncoder::RequestKeyframe() {
    force_idr_ = true;
    return Status::Ok();
}

Status NvencEncoder::Flush() {
    if (!open_) return Status::Ok();
    std::unique_lock lock(mutex_);
    if (!cv_.wait_for(lock, std::chrono::seconds(3), [this] { return inflight_.empty(); }))
        RF_WARN("NVENC still had frames in flight after 3 s");
    return Status::Ok();
}

void NvencEncoder::DestroySession() {
    if (open_) Flush();

    running_ = false;
    cv_.notify_all();
    if (output_thread_.joinable()) output_thread_.join();

    api_.reset();
    for (auto& input : inputs_) input.Reset();
    free_slots_.clear();
    inflight_.clear();
    last_input_ = nullptr;
    next_input_ = 0;
    open_ = false;
}

void NvencEncoder::Close() { DestroySession(); }

#endif

}
