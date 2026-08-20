#include "rf/encode/MfEncoder.h"

#include <codecapi.h>
#include <icodecapi.h>
#include <mferror.h>
#include <mfobjects.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

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

    if (::GetEnvironmentVariableW(L"REFRAME_FORCE_CPU_ENCODER", nullptr, 0) == 0)
        RF_HR(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, &input_info, &output_info, &activates,
                        &count));

    if (count == 0) {
        if (activates) ::CoTaskMemFree(activates);
        activates = nullptr;

        const UINT32 cpu_flags = MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT |
                                 MFT_ENUM_FLAG_TRANSCODE_ONLY | MFT_ENUM_FLAG_SORTANDFILTER;
        RF_HR(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, cpu_flags, &input_info, &output_info,
                        &activates, &count));
        if (count == 0) {
            if (activates) ::CoTaskMemFree(activates);
            return Status::Fail(MF_E_TOPO_CODEC_NOT_FOUND,
                                "no video encoder for the requested codec");
        }
        software_ = true;
        RF_WARN("this GPU has no hardware encoder - encoding on the processor instead");
    }

    const std::wstring wanted_vendor = [&] -> std::wstring {
        switch (device_ ? device_->info().vendor : GpuVendor::Unknown) {
            case GpuVendor::Nvidia: return L"VEN_10DE";
            case GpuVendor::Amd:    return L"VEN_1002";
            case GpuVendor::Intel:  return L"VEN_8086";
            default:                return L"";
        }
    }();

    auto vendor_of = [](IMFActivate* activate) -> std::wstring {
        LPWSTR id = nullptr;
        UINT32 len = 0;
        if (FAILED(activate->GetAllocatedString(MFT_ENUM_HARDWARE_VENDOR_ID_Attribute, &id, &len)) ||
            !id)
            return {};
        std::wstring out = id;
        ::CoTaskMemFree(id);
        return out;
    };

    Status result = Status::Fail("no MFT could be activated");
    for (int pass = 0; pass < 2 && !result.ok(); ++pass) {

        const bool vendor_must_match = pass == 0 && !software_ && !wanted_vendor.empty();

        for (UINT32 i = 0; i < count; ++i) {
            if (vendor_must_match && vendor_of(activates[i]) != wanted_vendor) continue;

            ComPtr<IMFTransform> candidate;
            if (SUCCEEDED(activates[i]->ActivateObject(IID_PPV_ARGS(&candidate)))) {
                LPWSTR friendly = nullptr;
                UINT32 len = 0;
                if (SUCCEEDED(activates[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute,
                                                               &friendly, &len)) &&
                    friendly) {
                    name_ = ToUtf8(friendly);
                    ::CoTaskMemFree(friendly);
                }
                transform_ = std::move(candidate);
                result = Status::Ok();
                break;
            }
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

        if (!software_) attrs->SetUINT32(MF_SA_D3D11_AWARE, TRUE);
        attrs->SetUINT32(MF_LOW_LATENCY, FALSE);
    }

    RF_INFO("selected encoder MFT: {}", name_);
    return Status::Ok();
}

Status MfEncoder::CreateEncoderDevice() {
    ComPtr<IDXGIAdapter> adapter = device_->adapter();
    if (!adapter) return Status::Fail("no adapter for the encoder device");

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};

    RF_HR(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels,
                            ARRAYSIZE(levels), D3D11_SDK_VERSION, &encoder_device_, nullptr,
                            &encoder_context_));

    if (ComPtr<ID3D10Multithread> mt; SUCCEEDED(encoder_device_.As(&mt))) mt->SetMultithreadProtected(TRUE);
    return Status::Ok();
}

Status MfEncoder::CreateSharedInputs() {
    const auto& f = config_.format;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = f.width;
    td.Height = f.height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    for (int i = 0; i < kInputTextures; ++i) {
        RF_HR(device_->device()->CreateTexture2D(&td, nullptr, &inputs_[i]));

        ComPtr<IDXGIResource> resource;
        RF_HR(inputs_[i].As(&resource));
        HANDLE shared = nullptr;
        RF_HR(resource->GetSharedHandle(&shared));
        RF_HR(encoder_device_->OpenSharedResource(shared, IID_PPV_ARGS(&encoder_inputs_[i])));
    }
    return Status::Ok();
}

Status MfEncoder::BindD3DManager() {
    RF_HR(MFCreateDXGIDeviceManager(&dxgi_reset_token_, &dxgi_manager_));
    RF_HR(dxgi_manager_->ResetDevice(encoder_device_ ? encoder_device_.Get() : device_->device(),
                                     dxgi_reset_token_));
    RF_HR(transform_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                     reinterpret_cast<ULONG_PTR>(dxgi_manager_.Get())));
    return Status::Ok();
}

Status MfEncoder::SetInputFormat(const GUID& subtype) {
    const auto& f = config_.format;

    ComPtr<IMFMediaType> in_type;
    RF_HR(MFCreateMediaType(&in_type));
    RF_HR(in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    RF_HR(in_type->SetGUID(MF_MT_SUBTYPE, subtype));
    RF_HR(in_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    RF_TRY(SetSize(in_type.Get(), MF_MT_FRAME_SIZE, f.width, f.height));
    RF_TRY(SetRatio(in_type.Get(), MF_MT_FRAME_RATE, f.fps_num, f.fps_den));
    RF_TRY(SetRatio(in_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
    RF_HR(transform_->SetInputType(input_stream_, in_type.Get(), 0));
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

    direct_rgb_ = !software_ && SetInputFormat(MFVideoFormat_ARGB32).ok();
    if (!direct_rgb_) RF_TRY(SetInputFormat(MFVideoFormat_NV12));

    RF_INFO("encoder input: {}", direct_rgb_  ? "BGRA direct"
                                : software_ ? "NV12 in system memory"
                                            : "NV12 via the video processor");

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
    if (!software_) {
        if (!config.device_is_dedicated) {
            if (auto s = CreateEncoderDevice(); !s.ok())
                RF_WARN("no separate device for the encoder ({}) - sharing the capture one",
                        s.str());
        }
        RF_TRY(BindD3DManager());
    }
    RF_TRY(ConfigureTypes());

    if (!direct_rgb_)
        RF_TRY(converter_.Init(device_, config.format.width, config.format.height,
                               DXGI_FORMAT_B8G8R8A8_UNORM, config.format.width,
                               config.format.height, DXGI_FORMAT_NV12, config.format.color));

    if (direct_rgb_ && encoder_device_) RF_TRY(CreateSharedInputs());
    if (direct_rgb_ && !encoder_device_) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = config.format.width;
        td.Height = config.format.height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        for (auto& input : inputs_) RF_HR(device_->device()->CreateTexture2D(&td, nullptr, &input));
    }

    if (software_) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = config.format.width;
        td.Height = config.format.height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_NV12;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        RF_HR(device_->device()->CreateTexture2D(&td, nullptr, &readback_));
    }

    if (!software_) RF_HR(transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0));
    RF_HR(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
    RF_HR(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));

    if (software_) {
        running_ = true;
        return Status::Ok();
    }

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

    Ticks100ns last_report = Now100ns();
    int need_input_events = 0, have_output_events = 0, fed = 0;

    while (running_.load(std::memory_order_relaxed)) {
        ComPtr<IMFMediaEvent> event;

        if (Now100ns() - last_report > 5 * kOneSecond100ns) {
            RF_DEBUG("mft events in {:.1f}s: needinput {}, haveoutput {}, fed {}, pending {}",
                    static_cast<double>(Now100ns() - last_report) / kOneSecond100ns,
                    need_input_events, have_output_events, fed, pending_.size());
            last_report = Now100ns();
            need_input_events = have_output_events = fed = 0;
        }

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
                ++need_input_events;
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
                if (sample) {
                    ++fed;
                    if (auto s = FeedSample(sample.Get()); !s.ok())
                        RF_WARN("ProcessInput failed: {}", s.str());
                }
                break;
            }
            case METransformHaveOutput: {
                ++have_output_events;
                bool produced = false;
                if (auto s = DrainOutput(produced); !s.ok()) RF_WARN("ProcessOutput: {}", s.str());
                break;
            }
            case METransformDrainComplete:

                RF_DEBUG("encoder drain complete - restarting the stream");
                transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

                need_input_.store(0, std::memory_order_relaxed);
                draining_.store(false, std::memory_order_release);
                break;
            default:
                break;
        }
    }
}

Status MfEncoder::FeedSample(IMFSample* sample) {

    if (draining_.load(std::memory_order_acquire)) return Status::Ok();

    if (force_keyframe_.exchange(false)) {
        sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
        TrySetCodecApi(transform_.Get(), CODECAPI_AVEncVideoForceKeyFrame, VarU32(1));
    }
    RF_HR(transform_->ProcessInput(input_stream_, sample, 0));
    ++stats_.frames_submitted;
    return Status::Ok();
}

Status MfEncoder::DrainOutput(bool& produced_any) {
    produced_any = false;
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
    produced_any = true;
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

    if ((!frame.content_changed || !frame.texture) && last_nv12_)
        return SubmitSurface(last_nv12_.Get(), frame.timestamp);
    if (!frame.texture) return Status::Ok();

    ID3D11Texture2D* surface = frame.texture;
    if (!direct_rgb_) {
        RF_TRY(converter_.Convert(frame.texture, &surface));
    } else {
        const int slot = next_input_;
        next_input_ = (next_input_ + 1) % kInputTextures;

        const D3D11_BOX box{0, 0, 0, config_.format.width, config_.format.height, 1};
        {
            D3DDevice::ContextLock lock(*device_);
            device_->context()->CopySubresourceRegion(inputs_[slot].Get(), 0, 0, 0, 0,
                                                      frame.texture, 0, &box);
            if (encoder_device_) device_->context()->Flush();
        }
        surface = encoder_device_ ? encoder_inputs_[slot].Get() : inputs_[slot].Get();
    }

    last_nv12_ = surface;
    return SubmitSurface(surface, frame.timestamp);
}

Status MfEncoder::PumpSync() {
    for (;;) {
        bool produced = false;
        if (auto s = DrainOutput(produced); !s.ok()) return s;
        if (!produced) return Status::Ok();
    }
}

Status MfEncoder::SubmitOnCpu(ID3D11Texture2D* nv12, Ticks100ns timestamp) {
    const auto& f = config_.format;
    const DWORD plane = f.width * f.height;

    ComPtr<IMFMediaBuffer> buffer;
    RF_HR(MFCreateMemoryBuffer(plane * 3 / 2, &buffer));

    BYTE* dst = nullptr;
    RF_HR(buffer->Lock(&dst, nullptr, nullptr));

    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        D3DDevice::ContextLock lock(*device_);
        device_->context()->CopyResource(readback_.Get(), nv12);
        const HRESULT hr = device_->context()->Map(readback_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
            buffer->Unlock();
            return Status::Fail(hr, "reading the frame back for the processor encoder");
        }

        const auto* src = static_cast<const std::uint8_t*>(mapped.pData);
        for (std::uint32_t y = 0; y < f.height; ++y)
            std::memcpy(dst + y * f.width, src + static_cast<std::size_t>(y) * mapped.RowPitch,
                        f.width);

        const auto* chroma = src + static_cast<std::size_t>(mapped.RowPitch) * f.height;
        for (std::uint32_t y = 0; y < f.height / 2; ++y)
            std::memcpy(dst + plane + y * f.width,
                        chroma + static_cast<std::size_t>(y) * mapped.RowPitch, f.width);

        device_->context()->Unmap(readback_.Get(), 0);
    }

    buffer->Unlock();
    RF_HR(buffer->SetCurrentLength(plane * 3 / 2));

    ComPtr<IMFSample> sample;
    RF_HR(MFCreateSample(&sample));
    RF_HR(sample->AddBuffer(buffer.Get()));

    if (first_pts_ < 0) first_pts_ = timestamp;
    RF_HR(sample->SetSampleTime(timestamp - first_pts_));
    RF_HR(sample->SetSampleDuration(kOneSecond100ns * f.fps_den / std::max(1u, f.fps_num)));

    RF_TRY(FeedSample(sample.Get()));
    return PumpSync();
}

Status MfEncoder::SubmitSurface(ID3D11Texture2D* nv12, Ticks100ns timestamp) {
    if (software_) return SubmitOnCpu(nv12, timestamp);

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

    if (software_) {
        transform_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        RF_TRY(PumpSync());
        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return Status::Ok();
    }

    draining_.store(true, std::memory_order_release);
    if (FAILED(transform_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0))) {
        draining_.store(false, std::memory_order_release);
        return Status::Ok();
    }

    for (int waited = 0; waited < 400 && draining_.load(std::memory_order_acquire); ++waited)
        ::Sleep(1);

    if (draining_.exchange(false, std::memory_order_acq_rel)) {
        RF_WARN("encoder did not report the drain finishing - restarting the stream anyway");
        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        need_input_.store(0, std::memory_order_relaxed);
    }
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
    for (auto& input : encoder_inputs_) input.Reset();
    for (auto& input : inputs_) input.Reset();
    encoder_context_.Reset();
    encoder_device_.Reset();
    readback_.Reset();

    if (mf_started_) {
        MFShutdown();
        mf_started_ = false;
    }
}

}
