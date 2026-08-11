#include "rf/encode/MfEncoder.h"

#include <codecapi.h>
#include <icodecapi.h>
#include <mferror.h>
#include <mfobjects.h>

#include <algorithm>

#include "rf/core/Log.h"
#include "rf/core/Strings.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "strmiids.lib")

using Microsoft::WRL::ComPtr;

namespace rf {
namespace {

GUID SubtypeFor(Codec c) {
    switch (c) {
        case Codec::H264: return MFVideoFormat_H264;
        case Codec::HEVC: return MFVideoFormat_HEVC;
        case Codec::AV1:  return MFVideoFormat_AV1;
    }
    return MFVideoFormat_H264;
}

Status SetRatio(IMFMediaType* type, const GUID& key, UINT32 num, UINT32 den) {
    RF_HR(MFSetAttributeRatio(type, key, num, den));
    return Status::Ok();
}

Status SetSize(IMFMediaType* type, const GUID& key, UINT32 w, UINT32 h) {
    RF_HR(MFSetAttributeSize(type, key, w, h));
    return Status::Ok();
}

void TrySetCodecApi(IMFTransform* transform, const GUID& api, VARIANT value) {
    ComPtr<ICodecAPI> codec;
    if (SUCCEEDED(transform->QueryInterface(IID_PPV_ARGS(&codec)))) {

        const HRESULT hr = codec->SetValue(&api, &value);
        if (FAILED(hr)) RF_DEBUG("ICodecAPI knob rejected: 0x{:08X}", static_cast<unsigned>(hr));
    }
    ::VariantClear(&value);
}

VARIANT VarU32(UINT32 v) {
    VARIANT var;
    ::VariantInit(&var);
    var.vt = VT_UI4;
    var.ulVal = v;
    return var;
}

VARIANT VarBool(bool v) {
    VARIANT var;
    ::VariantInit(&var);
    var.vt = VT_BOOL;
    var.boolVal = v ? VARIANT_TRUE : VARIANT_FALSE;
    return var;
}

}

MfEncoder::MfEncoder(D3DDevicePtr device) : device_(std::move(device)) {}

MfEncoder::~MfEncoder() { Close(); }

Status MfEncoder::SelectTransform() {
    MFT_REGISTER_TYPE_INFO output_info{MFMediaType_Video, SubtypeFor(config_.format.codec)};
    MFT_REGISTER_TYPE_INFO input_info{MFMediaType_Video, MFVideoFormat_NV12};

    IMFActivate** activates = nullptr;
    UINT32 count = 0;

    const UINT32 flags = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT |
                         MFT_ENUM_FLAG_SORTANDFILTER;

    RF_HR(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, &input_info, &output_info, &activates,
                    &count));
    if (count == 0) {
        if (activates) ::CoTaskMemFree(activates);
        return Status::Fail(MF_E_TOPO_CODEC_NOT_FOUND,
                            "no hardware video encoder MFT for the requested codec");
    }

    Status result = Status::Fail("no MFT could be activated");
    for (UINT32 i = 0; i < count; ++i) {
        ComPtr<IMFTransform> candidate;
        if (SUCCEEDED(activates[i]->ActivateObject(IID_PPV_ARGS(&candidate)))) {
            LPWSTR friendly = nullptr;
            UINT32 len = 0;
            if (SUCCEEDED(activates[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &friendly,
                                                           &len)) &&
                friendly) {
                name_ = ToUtf8(friendly);
                ::CoTaskMemFree(friendly);
            }
            transform_ = std::move(candidate);
            result = Status::Ok();
            break;
        }
    }

    for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
    ::CoTaskMemFree(activates);

    if (!result.ok()) return result;

    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(transform_->GetAttributes(&attrs))) {
        UINT32 is_async = 0;
        attrs->GetUINT32(MF_TRANSFORM_ASYNC, &is_async);
        if (is_async) RF_HR(attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE));

        attrs->SetUINT32(MF_SA_D3D11_AWARE, TRUE);
        attrs->SetUINT32(MF_LOW_LATENCY, config_.low_latency ? TRUE : FALSE);
    }

    RF_INFO("selected encoder MFT: {}", name_);
    return Status::Ok();
}

Status MfEncoder::BindD3DManager() {
    RF_HR(MFCreateDXGIDeviceManager(&dxgi_reset_token_, &dxgi_manager_));
    RF_HR(dxgi_manager_->ResetDevice(device_->device(), dxgi_reset_token_));
    RF_HR(transform_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                     reinterpret_cast<ULONG_PTR>(dxgi_manager_.Get())));
    return Status::Ok();
}

Status MfEncoder::ConfigureTypes() {
    DWORD in_count = 0, out_count = 0;
    RF_HR(transform_->GetStreamCount(&in_count, &out_count));
    DWORD in_ids[8]{}, out_ids[8]{};
    if (SUCCEEDED(transform_->GetStreamIDs(in_count, in_ids, out_count, out_ids))) {
        input_stream_ = in_ids[0];
        output_stream_ = out_ids[0];
    }

    const auto& f = config_.format;

    ComPtr<IMFMediaType> out_type;
    RF_HR(MFCreateMediaType(&out_type));
    RF_HR(out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    RF_HR(out_type->SetGUID(MF_MT_SUBTYPE, SubtypeFor(f.codec)));
    RF_HR(out_type->SetUINT32(MF_MT_AVG_BITRATE, config_.bitrate_kbps * 1000));
    RF_HR(out_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    RF_TRY(SetSize(out_type.Get(), MF_MT_FRAME_SIZE, f.width, f.height));
    RF_TRY(SetRatio(out_type.Get(), MF_MT_FRAME_RATE, f.fps_num, f.fps_den));
    RF_TRY(SetRatio(out_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
    if (f.codec == Codec::H264)
        out_type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);

    const bool hdr = f.color == ColorSpace::Rec2020Pq;
    out_type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    out_type->SetUINT32(MF_MT_YUV_MATRIX,
                        hdr ? MFVideoTransferMatrix_BT2020_10 : MFVideoTransferMatrix_BT709);
    out_type->SetUINT32(MF_MT_VIDEO_PRIMARIES,
                        hdr ? MFVideoPrimaries_BT2020 : MFVideoPrimaries_BT709);
    out_type->SetUINT32(MF_MT_TRANSFER_FUNCTION,
                        hdr ? MFVideoTransFunc_2084 : MFVideoTransFunc_709);
    RF_HR(transform_->SetOutputType(output_stream_, out_type.Get(), 0));

    ComPtr<IMFMediaType> in_type;
    RF_HR(MFCreateMediaType(&in_type));
    RF_HR(in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    RF_HR(in_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));
    RF_HR(in_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    RF_TRY(SetSize(in_type.Get(), MF_MT_FRAME_SIZE, f.width, f.height));
    RF_TRY(SetRatio(in_type.Get(), MF_MT_FRAME_RATE, f.fps_num, f.fps_den));
    RF_TRY(SetRatio(in_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
    RF_HR(transform_->SetInputType(input_stream_, in_type.Get(), 0));

    switch (config_.rate_control) {
        case RateControl::CBR:
            TrySetCodecApi(transform_.Get(), CODECAPI_AVEncCommonRateControlMode,
                           VarU32(eAVEncCommonRateControlMode_CBR));
            TrySetCodecApi(transform_.Get(), CODECAPI_AVEncCommonMeanBitRate,
                           VarU32(config_.bitrate_kbps * 1000));
            break;
        case RateControl::VBR: {
            const std::uint32_t peak = config_.max_bitrate_kbps
                                           ? config_.max_bitrate_kbps
                                           : config_.bitrate_kbps * 3 / 2;
            TrySetCodecApi(transform_.Get(), CODECAPI_AVEncCommonRateControlMode,
                           VarU32(eAVEncCommonRateControlMode_PeakConstrainedVBR));
            TrySetCodecApi(transform_.Get(), CODECAPI_AVEncCommonMeanBitRate,
                           VarU32(config_.bitrate_kbps * 1000));
            TrySetCodecApi(transform_.Get(), CODECAPI_AVEncCommonMaxBitRate, VarU32(peak * 1000));
            break;
        }
        case RateControl::ConstQp:
            TrySetCodecApi(transform_.Get(), CODECAPI_AVEncCommonRateControlMode,
                           VarU32(eAVEncCommonRateControlMode_Quality));
            TrySetCodecApi(transform_.Get(), CODECAPI_AVEncCommonQuality, VarU32(config_.qp));
            break;
    }

    const std::uint32_t fps = std::max(1u, f.fps_num / std::max(1u, f.fps_den));
    const std::uint32_t gop = std::max(1u, fps * config_.keyframe_interval_ms / 1000);
    TrySetCodecApi(transform_.Get(), CODECAPI_AVEncMPVGOPSize, VarU32(gop));

    const std::uint32_t b = config_.quirks.has(Quirk::Id::DisableBFrames) ? 0 : config_.b_frames;
    TrySetCodecApi(transform_.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, VarU32(b));

    if (config_.low_latency) {
        TrySetCodecApi(transform_.Get(), CODECAPI_AVLowLatencyMode, VarBool(true));
        TrySetCodecApi(transform_.Get(), CODECAPI_AVEncCommonLowLatency, VarBool(true));
    }

    TrySetCodecApi(transform_.Get(), CODECAPI_AVEncCommonQualityVsSpeed,
                   VarU32(std::min(100u, config_.quality_vs_speed)));

    TrySetCodecApi(transform_.Get(), CODECAPI_AVEncVideoForceKeyFrame, VarU32(0));

    ComPtr<IMFMediaType> current;
    if (SUCCEEDED(transform_->GetOutputCurrentType(output_stream_, &current))) {
        UINT32 blob_size = 0;
        if (SUCCEEDED(current->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &blob_size)) &&
            blob_size > 0) {
            codec_private_.data.resize(blob_size);
            current->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, codec_private_.data.data(), blob_size,
                             nullptr);
            RF_DEBUG("codec private data: {} bytes", blob_size);
        }
    }

    RF_INFO("encoder configured: {} {}x{}@{} {} kbps GOP={} B={}", ToString(f.codec), f.width,
            f.height, fps, config_.bitrate_kbps, gop, b);
    return Status::Ok();
}

Status MfEncoder::Open(const EncoderConfig& config, const PacketCallback& on_packet) {
    config_ = config;
    on_packet_ = on_packet;

    if (config.epoch > 0) first_pts_ = config.epoch;

    RF_HR(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    mf_started_ = true;

    RF_TRY(SelectTransform());
    RF_TRY(BindD3DManager());
    RF_TRY(ConfigureTypes());

    RF_TRY(converter_.Init(device_, config.format.width, config.format.height,
                           DXGI_FORMAT_B8G8R8A8_UNORM, config.format.width, config.format.height,
                           DXGI_FORMAT_NV12, config.format.color));

    RF_HR(transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0));
    RF_HR(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
    RF_HR(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));

    RF_TRY(StartEventLoop());
    return Status::Ok();
}

Status MfEncoder::StartEventLoop() {
    RF_HR(transform_->QueryInterface(IID_PPV_ARGS(&events_)));
    running_ = true;
    event_thread_ = std::thread([this] { EventLoop(); });
    return Status::Ok();
}

void MfEncoder::EventLoop() {
    ::SetThreadDescription(::GetCurrentThread(), L"rf-encode-mf");
    MmcssScope mmcss(L"Playback");

    while (running_.load(std::memory_order_relaxed)) {
        ComPtr<IMFMediaEvent> event;

        const HRESULT hr = events_->GetEvent(0, &event);
        if (FAILED(hr)) {
            if (hr != MF_E_SHUTDOWN)
                RF_ERROR("encoder event loop: 0x{:08X}", static_cast<unsigned>(hr));
            break;
        }

        MediaEventType type = MEUnknown;
        event->GetType(&type);

        switch (type) {
            case METransformNeedInput: {

                ComPtr<IMFSample> sample;
                {
                    std::scoped_lock lock(pending_mutex_);
                    if (pending_.empty()) {
                        need_input_.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        sample = pending_.front();
                        pending_.erase(pending_.begin());
                    }
                }
                if (sample)
                    if (auto s = FeedSample(sample.Get()); !s.ok())
                        RF_WARN("ProcessInput failed: {}", s.str());
                break;
            }
            case METransformHaveOutput:
                if (auto s = DrainOutput(); !s.ok()) RF_WARN("ProcessOutput: {}", s.str());
                break;
            case METransformDrainComplete:
                RF_DEBUG("encoder drain complete");
                break;
            default:
                break;
        }
    }
}

Status MfEncoder::FeedSample(IMFSample* sample) {
    if (force_keyframe_.exchange(false)) {
        sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
        TrySetCodecApi(transform_.Get(), CODECAPI_AVEncVideoForceKeyFrame, VarU32(1));
    }
    RF_HR(transform_->ProcessInput(input_stream_, sample, 0));
    ++stats_.frames_submitted;
    return Status::Ok();
}

Status MfEncoder::DrainOutput() {
    MFT_OUTPUT_STREAM_INFO info{};
    RF_HR(transform_->GetOutputStreamInfo(output_stream_, &info));

    MFT_OUTPUT_DATA_BUFFER out{};
    out.dwStreamID = output_stream_;

    ComPtr<IMFSample> sample;
    if (!(info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                          MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
        ComPtr<IMFMediaBuffer> buffer;
        RF_HR(MFCreateMemoryBuffer(info.cbSize, &buffer));
        RF_HR(MFCreateSample(&sample));
        RF_HR(sample->AddBuffer(buffer.Get()));
        out.pSample = sample.Get();
    }

    DWORD status = 0;
    const HRESULT hr = transform_->ProcessOutput(0, 1, &out, &status);
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return Status::Ok();
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        RF_WARN("encoder requested output type renegotiation");
        return Status::Ok();
    }
    if (FAILED(hr)) return Status::Fail(hr, "ProcessOutput");

    ComPtr<IMFSample> produced;
    produced.Attach(out.pSample);
    if (out.pEvents) out.pEvents->Release();
    if (!produced) return Status::Ok();

    ComPtr<IMFMediaBuffer> buffer;
    RF_HR(produced->ConvertToContiguousBuffer(&buffer));

    BYTE* data = nullptr;
    DWORD length = 0;
    RF_HR(buffer->Lock(&data, nullptr, &length));

    auto packet = std::make_shared<Packet>();
    packet->kind = MediaKind::Video;
    packet->track = 0;
    packet->data.assign(data, data + length);
    buffer->Unlock();

    LONGLONG pts = 0, duration = 0;
    produced->GetSampleTime(&pts);
    produced->GetSampleDuration(&duration);
    packet->pts = pts;
    packet->dts = pts;
    packet->duration = duration;

    UINT32 clean = 0;
    produced->GetUINT32(MFSampleExtension_CleanPoint, &clean);
    packet->keyframe = clean != 0;

    ++stats_.frames_encoded;
    stats_.bytes_out += length;

    if (on_packet_) on_packet_(std::move(packet));
    return Status::Ok();
}

Status MfEncoder::Submit(const CapturedFrame& frame) {
    if (!running_) return Status::Fail("encoder not open");

    if (!frame.content_changed && last_nv12_) {
        ID3D11Texture2D* previous = last_nv12_.Get();
        return SubmitSurface(previous, frame.timestamp);
    }

    ID3D11Texture2D* nv12 = nullptr;
    RF_TRY(converter_.Convert(frame.texture, &nv12));
    last_nv12_ = nv12;
    return SubmitSurface(nv12, frame.timestamp);
}

Status MfEncoder::SubmitSurface(ID3D11Texture2D* nv12, Ticks100ns timestamp) {

    ComPtr<IMFMediaBuffer> buffer;
    RF_HR(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12, 0, FALSE, &buffer));

    DWORD length = 0;
    if (ComPtr<IMF2DBuffer> b2d; SUCCEEDED(buffer.As(&b2d))) b2d->GetContiguousLength(&length);
    buffer->SetCurrentLength(length);

    ComPtr<IMFSample> sample;
    RF_HR(MFCreateSample(&sample));
    RF_HR(sample->AddBuffer(buffer.Get()));

    if (first_pts_ < 0) first_pts_ = timestamp;
    const Ticks100ns pts = timestamp - first_pts_;
    RF_HR(sample->SetSampleTime(pts));
    RF_HR(sample->SetSampleDuration(kOneSecond100ns * config_.format.fps_den /
                                    std::max(1u, config_.format.fps_num)));

    const std::size_t max_depth =
        config_.quirks.has(Quirk::Id::EncoderLimitAsyncDepth) ? config_.async_depth : 8;

    bool feed_now = false;
    {
        std::scoped_lock lock(pending_mutex_);
        if (need_input_.load(std::memory_order_relaxed) > 0) {
            need_input_.fetch_sub(1, std::memory_order_relaxed);
            feed_now = true;
        } else if (pending_.size() >= max_depth) {
            ++stats_.frames_dropped;
            RF_DEBUG("encoder backlog {} - dropping a frame", pending_.size());
            return Status::Ok();
        } else {
            pending_.push_back(sample);
            stats_.avg_queue_depth = 0.9 * stats_.avg_queue_depth + 0.1 * pending_.size();
        }
    }

    if (feed_now) RF_TRY(FeedSample(sample.Get()));
    return Status::Ok();
}

Status MfEncoder::RequestKeyframe() {
    force_keyframe_ = true;
    return Status::Ok();
}

Status MfEncoder::Flush() {
    if (!transform_) return Status::Ok();
    RF_HR(transform_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0));
    return Status::Ok();
}

void MfEncoder::Close() {
    running_ = false;
    if (transform_) {
        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        transform_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }
    if (events_) {

        if (ComPtr<IMFShutdown> shutdown; SUCCEEDED(events_.As(&shutdown))) shutdown->Shutdown();
    }
    if (event_thread_.joinable()) event_thread_.join();

    events_.Reset();
    transform_.Reset();
    dxgi_manager_.Reset();

    if (mf_started_) {
        MFShutdown();
        mf_started_ = false;
    }
}

}
