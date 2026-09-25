#include "rf/mux/ClipCropper.h"

#include <windows.h>

#include <d3d11_4.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <format>
#include <map>

#include "rf/core/Log.h"
#include "rf/core/Strings.h"
#include "rf/gpu/D3DDevice.h"
#include "rf/mux/TrackNames.h"

using Microsoft::WRL::ComPtr;

namespace rf {
namespace {

constexpr std::uint8_t kBlackLuma = 16;
constexpr std::uint8_t kBlackChroma = 128;
constexpr std::uint32_t kFallbackBitrateKbps = 20'000;
constexpr double kMinBitrateShare = 0.35;
constexpr DWORD kPooledFrames = 16;

PixelRect FullFrame(std::uint32_t width, std::uint32_t height) {
    return {0, 0, static_cast<std::int32_t>(width), static_cast<std::int32_t>(height)};
}

PixelRect EvenInside(PixelRect rect, std::uint32_t width, std::uint32_t height) {
    rect.left = std::clamp(rect.left, 0, static_cast<std::int32_t>(width)) & ~1;
    rect.top = std::clamp(rect.top, 0, static_cast<std::int32_t>(height)) & ~1;
    rect.right = std::clamp(rect.right, rect.left, static_cast<std::int32_t>(width));
    rect.bottom = std::clamp(rect.bottom, rect.top, static_cast<std::int32_t>(height));
    rect.right = rect.left + (rect.width() & ~1);
    rect.bottom = rect.top + (rect.height() & ~1);
    return rect;
}

ComPtr<IMFMediaType> VideoType(const GUID& subtype, UINT32 width, UINT32 height, UINT32 fps_num,
                               UINT32 fps_den) {
    ComPtr<IMFMediaType> type;
    if (FAILED(::MFCreateMediaType(&type))) return nullptr;
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, subtype);
    type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    ::MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, width, height);
    ::MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, fps_num, fps_den);
    ::MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    return type;
}

void CopyColorDescription(IMFMediaType* from, IMFMediaType* to) {
    struct Field {
        const GUID& key;
        UINT32 fallback;
    };
    const Field fields[] = {{MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709},
                            {MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235},
                            {MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709},
                            {MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709}};
    for (const Field& field : fields)
        to->SetUINT32(field.key, ::MFGetAttributeUINT32(from, field.key, field.fallback));
}

DWORD FindStream(IMFSourceReader* reader, const GUID& major) {
    for (DWORD index = 0;; ++index) {
        ComPtr<IMFMediaType> type;
        if (FAILED(reader->GetNativeMediaType(index, 0, &type))) return MAXDWORD;
        GUID found{};
        if (SUCCEEDED(type->GetGUID(MF_MT_MAJOR_TYPE, &found)) && found == major) return index;
    }
}

std::string EncoderName(IMFSinkWriter* writer, DWORD stream) {
    ComPtr<IMFSinkWriterEx> extended;
    if (FAILED(writer->QueryInterface(IID_PPV_ARGS(&extended)))) return "an unknown encoder";
    for (DWORD index = 0;; ++index) {
        GUID category{};
        ComPtr<IMFTransform> transform;
        if (FAILED(extended->GetTransformForStream(stream, index, &category, &transform)))
            return "an unknown encoder";
        if (category != MFT_CATEGORY_VIDEO_ENCODER) continue;
        ComPtr<IMFAttributes> attributes;
        wchar_t name[128] = L"the software encoder";
        if (SUCCEEDED(transform->GetAttributes(&attributes)) && attributes)
            attributes->GetString(MFT_FRIENDLY_NAME_Attribute, name, ARRAYSIZE(name), nullptr);
        return ToUtf8(name);
    }
}

std::filesystem::path CurrentExecutable() {
    wchar_t path[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

struct GpuDevice {
    ComPtr<ID3D11Device> device;
    ComPtr<IMFDXGIDeviceManager> manager;
};

Status CreateGpuDevice(GpuDevice& gpu) {
    RF_HR(::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                              D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                              &gpu.device, nullptr, nullptr));
    ComPtr<ID3D11Multithread> multithread;
    if (SUCCEEDED(gpu.device.As(&multithread))) multithread->SetMultithreadProtected(TRUE);

    UINT token = 0;
    RF_HR(::MFCreateDXGIDeviceManager(&token, &gpu.manager));
    RF_HR(gpu.manager->ResetDevice(gpu.device.Get(), token));
    return Status::Ok();
}

struct Clip {
    ComPtr<IMFSourceReader> reader;
    ComPtr<IMFSinkWriter> writer;
    ComPtr<IMFMediaType> encoder_input;
    struct AudioStream {
        DWORD source = 0;
        DWORD target = 0;
        ComPtr<IMFMediaType> type;
    };

    DWORD video_index = MAXDWORD;
    DWORD video_stream = 0;
    std::vector<AudioStream> audio;
    UINT32 decoded_width = 0;
    UINT32 decoded_height = 0;
    UINT32 fallback_pitch = 0;
    UINT32 fps_num = 60;
    UINT32 fps_den = 1;
    LONGLONG duration = 0;
    PixelRect output;
};

Status OpenClip(const CropJob& job, const std::filesystem::path& temp, IMFDXGIDeviceManager* gpu,
                Clip& clip) {
    ComPtr<IMFAttributes> reader_attributes;
    RF_HR(::MFCreateAttributes(&reader_attributes, 3));
    if (gpu) {
        RF_HR(reader_attributes->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, gpu));
        RF_HR(reader_attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE));
        RF_HR(reader_attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE));
    } else {
        RF_HR(reader_attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE));
    }
    RF_HR(::MFCreateSourceReaderFromURL(job.raw.c_str(), reader_attributes.Get(), &clip.reader));

    PROPVARIANT duration;
    ::PropVariantInit(&duration);
    if (SUCCEEDED(clip.reader->GetPresentationAttribute(
            static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &duration)) &&
        duration.vt == VT_UI8)
        clip.duration = static_cast<LONGLONG>(duration.uhVal.QuadPart);
    ::PropVariantClear(&duration);

    clip.video_index = FindStream(clip.reader.Get(), MFMediaType_Video);
    if (clip.video_index == MAXDWORD) return Status::Fail("the clip has no video");

    ComPtr<IMFMediaType> native_video;
    RF_HR(clip.reader->GetNativeMediaType(clip.video_index, 0, &native_video));
    GUID codec{};
    RF_HR(native_video->GetGUID(MF_MT_SUBTYPE, &codec));
    ::MFGetAttributeRatio(native_video.Get(), MF_MT_FRAME_RATE, &clip.fps_num, &clip.fps_den);

    RF_HR(clip.reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE));
    RF_HR(clip.reader->SetStreamSelection(clip.video_index, TRUE));
    ComPtr<IMFMediaType> nv12;
    RF_HR(::MFCreateMediaType(&nv12));
    RF_HR(nv12->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    RF_HR(nv12->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));
    RF_HR(clip.reader->SetCurrentMediaType(clip.video_index, nullptr, nv12.Get()));
    ComPtr<IMFMediaType> decoded;
    RF_HR(clip.reader->GetCurrentMediaType(clip.video_index, &decoded));
    RF_HR(::MFGetAttributeSize(decoded.Get(), MF_MT_FRAME_SIZE, &clip.decoded_width,
                               &clip.decoded_height));
    clip.fallback_pitch =
        ::MFGetAttributeUINT32(decoded.Get(), MF_MT_DEFAULT_STRIDE, clip.decoded_width);

    for (DWORD index = 0;; ++index) {
        Clip::AudioStream stream{index};
        if (FAILED(clip.reader->GetNativeMediaType(index, 0, &stream.type))) break;
        GUID major{};
        if (FAILED(stream.type->GetGUID(MF_MT_MAJOR_TYPE, &major)) || major != MFMediaType_Audio)
            continue;
        if (SUCCEEDED(clip.reader->SetStreamSelection(index, TRUE)) &&
            SUCCEEDED(clip.reader->SetCurrentMediaType(index, nullptr, stream.type.Get())))
            clip.audio.push_back(std::move(stream));
    }

    clip.output = CropOutputSize(job.samples, job.frame_width, job.frame_height);
    const auto out_width = static_cast<UINT32>(clip.output.width());
    const auto out_height = static_cast<UINT32>(clip.output.height());

    ComPtr<IMFAttributes> writer_attributes;
    RF_HR(::MFCreateAttributes(&writer_attributes, 4));
    RF_HR(writer_attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4));
    RF_HR(writer_attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE));
    RF_HR(writer_attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE));
    if (gpu) RF_HR(writer_attributes->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, gpu));
    RF_HR(::MFCreateSinkWriterFromURL(temp.c_str(), nullptr, writer_attributes.Get(), &clip.writer));

    ComPtr<IMFMediaType> encoded = VideoType(codec, out_width, out_height, clip.fps_num, clip.fps_den);
    clip.encoder_input =
        VideoType(MFVideoFormat_NV12, out_width, out_height, clip.fps_num, clip.fps_den);
    if (!encoded || !clip.encoder_input) return Status::Fail("cannot describe the cropped video");
    CopyColorDescription(native_video.Get(), encoded.Get());
    CopyColorDescription(native_video.Get(), clip.encoder_input.Get());

    const std::uint32_t kbps = job.bitrate_kbps ? job.bitrate_kbps : kFallbackBitrateKbps;
    const double area_share = static_cast<double>(out_width) * out_height /
                              (static_cast<double>(job.frame_width) * job.frame_height);
    RF_HR(encoded->SetUINT32(
        MF_MT_AVG_BITRATE,
        static_cast<UINT32>(kbps * 1000.0 * std::clamp(area_share, kMinBitrateShare, 1.0))));
    RF_HR(clip.writer->AddStream(encoded.Get(), &clip.video_stream));
    RF_HR(clip.writer->SetInputMediaType(clip.video_stream, clip.encoder_input.Get(), nullptr));

    for (Clip::AudioStream& stream : clip.audio) {
        RF_HR(clip.writer->AddStream(stream.type.Get(), &stream.target));
        RF_HR(clip.writer->SetInputMediaType(stream.target, stream.type.Get(), nullptr));
    }
    RF_HR(clip.writer->BeginWriting());
    RF_INFO("cropping {} to {}x{} on the {} with {}", job.raw.filename().string(), out_width,
            out_height, gpu ? "GPU" : "CPU", EncoderName(clip.writer.Get(), clip.video_stream));
    return Status::Ok();
}

template <class FrameWriter>
Status Pump(Clip& clip, const CropProgress& progress, FrameWriter&& write_frame) {
    auto streams_open = 1 + clip.audio.size();
    while (streams_open > 0) {
        DWORD stream_index = 0, flags = 0;
        LONGLONG time = 0;
        ComPtr<IMFSample> sample;
        RF_HR(clip.reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_ANY_STREAM), 0,
                                      &stream_index, &flags, &time, &sample));
        if (flags & MF_SOURCE_READERF_ERROR) return Status::Fail("decoding the clip failed");
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            --streams_open;
            continue;
        }
        if (!sample) continue;

        if (stream_index == clip.video_index) {
            ComPtr<IMFSample> encoded;
            RF_TRY(write_frame(sample.Get(), time, encoded));
            RF_HR(encoded->SetSampleTime(time));
            LONGLONG duration = 0;
            if (SUCCEEDED(sample->GetSampleDuration(&duration)))
                RF_HR(encoded->SetSampleDuration(duration));
            RF_HR(clip.writer->WriteSample(clip.video_stream, encoded.Get()));
            if (progress && clip.duration > 0)
                progress(static_cast<float>(std::clamp(static_cast<double>(time) / clip.duration, 0.0, 1.0)));
        } else {
            for (const Clip::AudioStream& stream : clip.audio)
                if (stream.source == stream_index)
                    RF_HR(clip.writer->WriteSample(stream.target, sample.Get()));
        }
    }
    RF_HR(clip.writer->Finalize());
    return Status::Ok();
}

class GpuCropper {
public:
    Status Init(const GpuDevice& gpu, const Clip& clip) {
        RF_HR(gpu.device.As(&video_device_));
        gpu.device->GetImmediateContext(&context_);
        RF_HR(context_.As(&video_context_));
        RF_TRY(CreateBlackFrame(gpu.device.Get(), clip));

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
        content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputFrameRate = {clip.fps_num, clip.fps_den};
        content.InputWidth = clip.decoded_width;
        content.InputHeight = clip.decoded_height;
        content.OutputFrameRate = {clip.fps_num, clip.fps_den};
        content.OutputWidth = static_cast<UINT>(clip.output.width());
        content.OutputHeight = static_cast<UINT>(clip.output.height());
        content.Usage = D3D11_VIDEO_USAGE_OPTIMAL_QUALITY;
        RF_HR(video_device_->CreateVideoProcessorEnumerator(&content, &enumerator_));
        RF_HR(video_device_->CreateVideoProcessor(enumerator_.Get(), 0, &processor_));

        D3D11_VIDEO_PROCESSOR_COLOR_SPACE studio_709{};
        studio_709.YCbCr_Matrix = 1;
        studio_709.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        video_context_->VideoProcessorSetStreamColorSpace(processor_.Get(), 0, &studio_709);
        video_context_->VideoProcessorSetOutputColorSpace(processor_.Get(), &studio_709);
        video_context_->VideoProcessorSetStreamFrameFormat(processor_.Get(), 0,
                                                           D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        video_context_->VideoProcessorSetStreamAutoProcessingMode(processor_.Get(), 0, FALSE);
        D3D11_VIDEO_COLOR black{};
        black.YCbCr = {16.0f / 255.0f, 0.5f, 0.5f, 1.0f};
        video_context_->VideoProcessorSetOutputBackgroundColor(processor_.Get(), TRUE, &black);
        const RECT target{0, 0, clip.output.width(), clip.output.height()};
        video_context_->VideoProcessorSetOutputTargetRect(processor_.Get(), TRUE, &target);
        video_context_->VideoProcessorSetStreamDestRect(processor_.Get(), 0, TRUE, &target);

        ComPtr<IMFAttributes> pool;
        RF_HR(::MFCreateAttributes(&pool, 2));
        RF_HR(pool->SetUINT32(MF_SA_D3D11_BINDFLAGS, D3D11_BIND_RENDER_TARGET));
        RF_HR(pool->SetUINT32(MF_SA_D3D11_USAGE, D3D11_USAGE_DEFAULT));
        RF_HR(::MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(&allocator_)));
        RF_HR(allocator_->SetDirectXManager(gpu.manager.Get()));
        RF_HR(allocator_->InitializeSampleAllocatorEx(2, kPooledFrames, pool.Get(),
                                                      clip.encoder_input.Get()));
        return Status::Ok();
    }

    Status Frame(IMFSample* decoded, LONGLONG time, const CropJob& job, ComPtr<IMFSample>& out) {
        const FramePlan plan = PlanFrame(job.samples, time, job.frame_width, job.frame_height);

        RF_HR(allocator_->AllocateSample(&out));
        if (plan.black) {
            ComPtr<ID3D11Texture2D> target;
            UINT slice = 0;
            RF_TRY(TextureOf(out.Get(), target, slice));
            context_->CopySubresourceRegion(target.Get(), slice, 0, 0, 0, black_.Get(), 0, nullptr);
            return Status::Ok();
        }

        ComPtr<ID3D11VideoProcessorInputView> input;
        RF_TRY(InputView(decoded, input));
        ComPtr<ID3D11VideoProcessorOutputView> output;
        RF_TRY(OutputView(out.Get(), output));

        const RECT source{plan.source.left, plan.source.top, plan.source.right, plan.source.bottom};
        video_context_->VideoProcessorSetStreamSourceRect(processor_.Get(), 0, TRUE, &source);
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = input.Get();
        RF_HR(video_context_->VideoProcessorBlt(processor_.Get(), output.Get(), 0, 1, &stream));
        return Status::Ok();
    }

private:
    Status CreateBlackFrame(ID3D11Device* device, const Clip& clip) {
        const auto width = static_cast<UINT>(clip.output.width());
        const auto height = static_cast<UINT>(clip.output.height());
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 3 / 2,
                                         kBlackChroma);
        std::memset(pixels.data(), kBlackLuma, static_cast<std::size_t>(width) * height);

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_NV12;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        const D3D11_SUBRESOURCE_DATA data{pixels.data(), width, 0};
        RF_HR(device->CreateTexture2D(&desc, &data, &black_));
        return Status::Ok();
    }

    static Status TextureOf(IMFSample* sample, ComPtr<ID3D11Texture2D>& texture, UINT& slice) {
        ComPtr<IMFMediaBuffer> buffer;
        RF_HR(sample->GetBufferByIndex(0, &buffer));
        ComPtr<IMFDXGIBuffer> gpu;
        RF_HR(buffer.As(&gpu));
        RF_HR(gpu->GetResource(IID_PPV_ARGS(&texture)));
        RF_HR(gpu->GetSubresourceIndex(&slice));
        return Status::Ok();
    }

    Status InputView(IMFSample* sample, ComPtr<ID3D11VideoProcessorInputView>& view) {
        ComPtr<ID3D11Texture2D> texture;
        UINT slice = 0;
        RF_TRY(TextureOf(sample, texture, slice));
        auto& cached = inputs_[{texture.Get(), slice}];
        if (!cached) {
            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC desc{};
            desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
            desc.Texture2D.ArraySlice = slice;
            RF_HR(video_device_->CreateVideoProcessorInputView(texture.Get(), enumerator_.Get(),
                                                               &desc, &cached.view));
            cached.texture = texture;
        }
        view = cached.view;
        return Status::Ok();
    }

    Status OutputView(IMFSample* sample, ComPtr<ID3D11VideoProcessorOutputView>& view) {
        ComPtr<ID3D11Texture2D> texture;
        UINT slice = 0;
        RF_TRY(TextureOf(sample, texture, slice));
        auto& cached = outputs_[texture.Get()];
        if (!cached) {
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC desc{};
            desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            RF_HR(video_device_->CreateVideoProcessorOutputView(texture.Get(), enumerator_.Get(),
                                                                &desc, &cached.view));
            cached.texture = texture;
        }
        view = cached.view;
        return Status::Ok();
    }

    template <class View>
    struct Cached {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<View> view;
        explicit operator bool() const { return view != nullptr; }
    };

    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11VideoDevice> video_device_;
    ComPtr<ID3D11VideoContext> video_context_;
    ComPtr<ID3D11Texture2D> black_;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
    ComPtr<ID3D11VideoProcessor> processor_;
    ComPtr<IMFVideoSampleAllocatorEx> allocator_;
    std::map<std::pair<ID3D11Texture2D*, UINT>, Cached<ID3D11VideoProcessorInputView>> inputs_;
    std::map<ID3D11Texture2D*, Cached<ID3D11VideoProcessorOutputView>> outputs_;
};

Status TranscodeOnGpu(const CropJob& job, const std::filesystem::path& temp,
                      const CropProgress& progress, bool& has_audio) {
    GpuDevice gpu;
    RF_TRY(CreateGpuDevice(gpu));
    Clip clip;
    RF_TRY(OpenClip(job, temp, gpu.manager.Get(), clip));
    has_audio = !clip.audio.empty();

    GpuCropper cropper;
    RF_TRY(cropper.Init(gpu, clip));
    return Pump(clip, progress, [&](IMFSample* decoded, LONGLONG time, ComPtr<IMFSample>& out) {
        return cropper.Frame(decoded, time, job, out);
    });
}

Status CopyFrameOnCpu(IMFSample* decoded, LONGLONG time, const CropJob& job, const Clip& clip,
                      std::vector<std::uint8_t>& frame, ComPtr<IMFSample>& out) {
    ComPtr<IMFMediaBuffer> buffer;
    RF_HR(decoded->ConvertToContiguousBuffer(&buffer));

    const FramePlan plan = PlanFrame(job.samples, time, job.frame_width, job.frame_height);
    const auto width = static_cast<std::uint32_t>(clip.output.width());
    const auto height = static_cast<std::uint32_t>(clip.output.height());

    ComPtr<IMF2DBuffer2> planar;
    BYTE* scanline = nullptr;
    BYTE* start = nullptr;
    LONG pitch = 0;
    DWORD length = 0;
    if (SUCCEEDED(buffer.As(&planar)) &&
        SUCCEEDED(planar->Lock2DSize(MF2DBuffer_LockFlags_Read, &scanline, &pitch, &start,
                                     &length)) &&
        pitch > 0) {
        const auto row_bytes = static_cast<std::uint32_t>(pitch);
        CopyNv12(scanline, row_bytes, static_cast<std::uint32_t>(length / row_bytes * 2 / 3), plan,
                 frame.data(), width, height);
        planar->Unlock2D();
    } else {
        BYTE* data = nullptr;
        RF_HR(buffer->Lock(&data, nullptr, &length));
        const std::uint32_t rows = clip.fallback_pitch ? length / clip.fallback_pitch * 2 / 3 : 0;
        CopyNv12(data, clip.fallback_pitch, rows, plan, frame.data(), width, height);
        buffer->Unlock();
    }

    ComPtr<IMFMediaBuffer> encoder_input;
    RF_HR(::MFCreateMemoryBuffer(static_cast<DWORD>(frame.size()), &encoder_input));
    BYTE* target = nullptr;
    RF_HR(encoder_input->Lock(&target, nullptr, nullptr));
    std::memcpy(target, frame.data(), frame.size());
    RF_HR(encoder_input->Unlock());
    RF_HR(encoder_input->SetCurrentLength(static_cast<DWORD>(frame.size())));
    RF_HR(::MFCreateSample(&out));
    RF_HR(out->AddBuffer(encoder_input.Get()));
    return Status::Ok();
}

Status TranscodeOnCpu(const CropJob& job, const std::filesystem::path& temp,
                      const CropProgress& progress, bool& has_audio) {
    Clip clip;
    RF_TRY(OpenClip(job, temp, nullptr, clip));
    has_audio = !clip.audio.empty();

    std::vector<std::uint8_t> frame(static_cast<std::size_t>(clip.output.width()) *
                                    clip.output.height() * 3 / 2);
    return Pump(clip, progress, [&](IMFSample* decoded, LONGLONG time, ComPtr<IMFSample>& out) {
        return CopyFrameOnCpu(decoded, time, job, clip, frame, out);
    });
}

}

bool NeedsCropping(const std::vector<WindowSample>& samples, std::uint32_t frame_width,
                   std::uint32_t frame_height) {
    const PixelRect full = FullFrame(frame_width, frame_height);
    return std::any_of(samples.begin(), samples.end(), [&](const WindowSample& s) {
        return !s.visible || EvenInside(s.crop, frame_width, frame_height) != full;
    });
}

PixelRect CropOutputSize(const std::vector<WindowSample>& samples, std::uint32_t frame_width,
                         std::uint32_t frame_height) {
    for (auto it = samples.rbegin(); it != samples.rend(); ++it) {
        if (!it->visible) continue;
        const PixelRect crop = EvenInside(it->crop, frame_width, frame_height);
        if (crop.width() >= 2 && crop.height() >= 2) return {0, 0, crop.width(), crop.height()};
    }
    return EvenInside(FullFrame(frame_width, frame_height), frame_width, frame_height);
}

FramePlan PlanFrame(const std::vector<WindowSample>& samples, Ticks100ns at,
                    std::uint32_t frame_width, std::uint32_t frame_height) {
    if (samples.empty())
        return {false, EvenInside(FullFrame(frame_width, frame_height), frame_width, frame_height)};

    auto current = std::upper_bound(samples.begin(), samples.end(), at,
                                    [](Ticks100ns t, const WindowSample& s) { return t < s.at; });
    const WindowSample& sample = current == samples.begin() ? samples.front() : *std::prev(current);

    FramePlan plan;
    plan.source = EvenInside(sample.crop, frame_width, frame_height);
    plan.black = !sample.visible || plan.source.empty();
    return plan;
}

void CopyNv12(const std::uint8_t* source, std::uint32_t source_pitch, std::uint32_t source_rows,
              const FramePlan& plan, std::uint8_t* target, std::uint32_t target_width,
              std::uint32_t target_height) {
    const std::size_t luma_bytes = static_cast<std::size_t>(target_width) * target_height;
    std::uint8_t* target_chroma = target + luma_bytes;

    const auto copy_width = std::min<std::uint32_t>(
        target_width, static_cast<std::uint32_t>(std::max(plan.source.width(), 0)));
    const auto copy_height = std::min<std::uint32_t>(
        target_height, static_cast<std::uint32_t>(std::max(plan.source.height(), 0)));
    if (plan.black || copy_width < target_width || copy_height < target_height) {
        std::memset(target, kBlackLuma, luma_bytes);
        std::memset(target_chroma, kBlackChroma, luma_bytes / 2);
    }
    if (plan.black) return;

    const auto left = static_cast<std::size_t>(plan.source.left);
    const auto top = static_cast<std::size_t>(plan.source.top);
    for (std::uint32_t row = 0; row < copy_height; ++row)
        std::memcpy(target + static_cast<std::size_t>(row) * target_width,
                    source + (top + row) * source_pitch + left, copy_width);

    const std::uint8_t* source_chroma =
        source + static_cast<std::size_t>(source_pitch) * source_rows;
    for (std::uint32_t row = 0; row < copy_height / 2; ++row)
        std::memcpy(target_chroma + static_cast<std::size_t>(row) * target_width,
                    source_chroma + (top / 2 + row) * source_pitch + left, copy_width);
}

Status CropClip(const CropJob& job, const CropProgress& progress) {
    std::filesystem::path temp = job.output;
    temp += L".cropping";

    bool has_audio = false;
    std::error_code ec;
    Status transcoded = job.use_gpu ? TranscodeOnGpu(job, temp, progress, has_audio)
                                    : Status::Fail("the GPU path is switched off");
    if (!transcoded.ok()) {
        if (job.use_gpu) RF_WARN("cropping on the GPU failed ({}) - using the CPU", transcoded.str());
        std::filesystem::remove(temp, ec);
        transcoded = TranscodeOnCpu(job, temp, progress, has_audio);
    }
    if (!transcoded.ok()) {
        std::filesystem::remove(temp, ec);
        return transcoded;
    }
    if (!::MoveFileExW(temp.c_str(), job.output.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        std::filesystem::remove(temp, ec);
        return Status::Fail(HRESULT_FROM_WIN32(::GetLastError()), "replacing the clip");
    }
    std::filesystem::remove(job.raw, ec);

    if (has_audio && !job.audio_names.empty()) {
        if (auto s = WriteAudioTrackNames(job.output, job.audio_names, job.first_track_is_full_mix);
            !s.ok())
            RF_WARN("could not name the audio track of {}: {}", job.output.string(), s.str());
    }
    return Status::Ok();
}

Status WriteCropJob(const std::filesystem::path& file, const CropJob& job) {
    std::string text;
    text += "raw=" + ToUtf8(job.raw.wstring()) + "\n";
    text += "output=" + ToUtf8(job.output.wstring()) + "\n";
    text += std::format("frame={} {}\n", job.frame_width, job.frame_height);
    text += std::format("bitrate={}\n", job.bitrate_kbps);
    for (const std::string& name : job.audio_names) text += "audio=" + name + "\n";
    text += std::format("full_mix={}\n", job.first_track_is_full_mix ? 1 : 0);
    text += std::format("gpu={}\n", job.use_gpu ? 1 : 0);
    for (const WindowSample& s : job.samples)
        text += std::format("sample={} {} {} {} {} {}\n", s.at, s.visible ? 1 : 0, s.crop.left,
                            s.crop.top, s.crop.right, s.crop.bottom);

    std::FILE* out = _wfopen(file.c_str(), L"wb");
    if (!out) return Status::Fail("cannot write the crop job");
    const bool written = std::fwrite(text.data(), 1, text.size(), out) == text.size();
    std::fclose(out);
    return written ? Status::Ok() : Status::Fail("cannot write the crop job");
}

Status ReadCropJob(const std::filesystem::path& file, CropJob& job) {
    std::FILE* in = _wfopen(file.c_str(), L"rb");
    if (!in) return Status::Fail("cannot read the crop job");
    std::string text;
    char chunk[4096];
    for (std::size_t n; (n = std::fread(chunk, 1, sizeof(chunk), in)) > 0;) text.append(chunk, n);
    std::fclose(in);

    job = {};
    std::size_t begin = 0;
    while (begin < text.size()) {
        std::size_t end = text.find('\n', begin);
        if (end == std::string::npos) end = text.size();
        const std::string_view line(text.data() + begin, end - begin);
        begin = end + 1;

        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) continue;
        const std::string_view key = line.substr(0, equals);
        const std::string value(line.substr(equals + 1));

        if (key == "raw") job.raw = ToWide(value);
        else if (key == "output") job.output = ToWide(value);
        else if (key == "audio") job.audio_names.push_back(value);
        else if (key == "full_mix") job.first_track_is_full_mix = value == "1";
        else if (key == "gpu") job.use_gpu = value == "1";
        else if (key == "frame")
            std::sscanf(value.c_str(), "%u %u", &job.frame_width, &job.frame_height);
        else if (key == "bitrate")
            std::sscanf(value.c_str(), "%u", &job.bitrate_kbps);
        else if (key == "sample") {
            WindowSample s;
            long long at = 0;
            int visible = 0;
            if (std::sscanf(value.c_str(), "%lld %d %d %d %d %d", &at, &visible, &s.crop.left,
                            &s.crop.top, &s.crop.right, &s.crop.bottom) == 6) {
                s.at = at;
                s.visible = visible != 0;
                job.samples.push_back(s);
            }
        }
    }
    if (job.raw.empty() || job.output.empty() || !job.frame_width || !job.frame_height)
        return Status::Fail("the crop job is incomplete");
    return Status::Ok();
}

static void ForwardHelperProgress(HANDLE output, std::string& pending,
                                  const CropProgress& progress) {
    DWORD available = 0;
    while (::PeekNamedPipe(output, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
        char chunk[256];
        DWORD received = 0;
        if (!::ReadFile(output, chunk, std::min<DWORD>(available, sizeof(chunk)), &received,
                        nullptr) ||
            received == 0)
            return;
        pending.append(chunk, received);
    }
    for (std::size_t end; (end = pending.find('\n')) != std::string::npos;) {
        int percent = 0;
        if (progress && std::sscanf(pending.c_str(), "progress %d", &percent) == 1)
            progress(static_cast<float>(percent) / 100.0f);
        pending.erase(0, end + 1);
    }
}

Status CropInHelperProcess(const CropJob& job, const std::filesystem::path& helper,
                           const CropProgress& progress) {
    constexpr ULONGLONG kHelperTimeoutMs = 10 * 60 * 1000;
    constexpr DWORD kPollMs = 100;
    std::filesystem::path job_file = job.raw;
    job_file += L".job";
    RF_TRY(WriteCropJob(job_file, job));

    SECURITY_ATTRIBUTES inheritable{sizeof(inheritable), nullptr, TRUE};
    HANDLE output = nullptr, helper_output = nullptr;
    std::error_code ec;
    if (!::CreatePipe(&output, &helper_output, &inheritable, 0)) {
        std::filesystem::remove(job_file, ec);
        return Status::Fail(HRESULT_FROM_WIN32(::GetLastError()), "creating the progress pipe");
    }
    ::SetHandleInformation(output, HANDLE_FLAG_INHERIT, 0);

    std::wstring command = L"\"" + helper.wstring() + L"\" --crop \"" + job_file.wstring() + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = helper_output;
    startup.hStdError = helper_output;
    PROCESS_INFORMATION process{};
    const BOOL started = ::CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, TRUE,
                                          CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr,
                                          nullptr, &startup, &process);
    const DWORD start_error = ::GetLastError();
    ::CloseHandle(helper_output);
    if (!started) {
        ::CloseHandle(output);
        std::filesystem::remove(job_file, ec);
        return Status::Fail(HRESULT_FROM_WIN32(start_error), "starting the crop helper");
    }
    ::CloseHandle(process.hThread);

    DWORD exit_code = 1;
    std::string pending;
    const ULONGLONG deadline = ::GetTickCount64() + kHelperTimeoutMs;
    while (true) {
        const DWORD waited = ::WaitForSingleObject(process.hProcess, kPollMs);
        ForwardHelperProgress(output, pending, progress);
        if (waited == WAIT_OBJECT_0) {
            ::GetExitCodeProcess(process.hProcess, &exit_code);
            break;
        }
        if (::GetTickCount64() > deadline) {
            ::TerminateProcess(process.hProcess, 1);
            ::WaitForSingleObject(process.hProcess, 5000);
            break;
        }
    }
    ::CloseHandle(output);
    ::CloseHandle(process.hProcess);

    std::filesystem::remove(job_file, ec);
    std::filesystem::path temp = job.output;
    temp += L".cropping";
    std::filesystem::remove(temp, ec);
    if (exit_code != 0)
        return Status::Fail(std::format("the crop helper exited with 0x{:08X}", exit_code));
    return Status::Ok();
}

int RunCropHelper(const std::filesystem::path& job_file) {
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    ::SetPriorityClass(::GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    LowerProcessGpuPriority();
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(::MFStartup(MF_VERSION, MFSTARTUP_LITE))) return 2;

    HANDLE output = ::GetStdHandle(STD_OUTPUT_HANDLE);
    int reported = -1;
    const auto report = [output, &reported](float progress) {
        const int percent = static_cast<int>(progress * 100.0f);
        if (!output || output == INVALID_HANDLE_VALUE || percent == reported) return;
        reported = percent;
        char line[32];
        const int length = std::snprintf(line, sizeof(line), "progress %d\n", percent);
        DWORD written = 0;
        ::WriteFile(output, line, static_cast<DWORD>(length), &written, nullptr);
    };

    CropJob job;
    const bool cropped = ReadCropJob(job_file, job).ok() && CropClip(job, report).ok();

    ::MFShutdown();
    ::CoUninitialize();
    return cropped ? 0 : 1;
}

ClipCropper::~ClipCropper() { Stop(); }

void ClipCropper::Push(CropJob job, CropProgress progress, Done done) {
    {
        std::scoped_lock lock(mutex_);
        queue_.push_back({std::move(job), std::move(progress), std::move(done)});
        if (!running_) {
            if (thread_.joinable()) thread_.join();
            running_ = true;
            thread_ = std::thread([this] { Loop(); });
        }
    }
    wake_.notify_one();
}

void ClipCropper::Stop() {
    {
        std::scoped_lock lock(mutex_);
        running_ = false;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void ClipCropper::Loop() {
    ::SetThreadDescription(::GetCurrentThread(), L"rf-clip-cropper");
    ::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool media_started = SUCCEEDED(::MFStartup(MF_VERSION, MFSTARTUP_LITE));

    while (true) {
        Work work;
        bool shutting_down = false;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return !queue_.empty() || !running_; });
            if (queue_.empty()) break;
            work = std::move(queue_.front());
            queue_.pop_front();
            shutting_down = !running_;
        }

        Status status = shutting_down ? Status::Fail("reframe++ is closing")
                                      : CropInHelperProcess(work.job, CurrentExecutable(),
                                                            work.progress);
        if (!status.ok()) {
            RF_WARN("clip {} kept uncropped: {}", work.job.output.string(), status.str());
            ::MoveFileExW(work.job.raw.c_str(), work.job.output.c_str(),
                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
        } else {
            RF_INFO("clip {} cropped to the game window", work.job.output.string());
        }
        if (work.done) work.done(status, work.job);
    }

    if (media_started) ::MFShutdown();
    ::CoUninitialize();
    ::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
}

}
