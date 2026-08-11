#include "rf/audio/AacEncoder.h"

#include <mfapi.h>
#include <mferror.h>
#include <wmcodecdsp.h>

#include <algorithm>

#include "rf/core/Log.h"

#pragma comment(lib, "wmcodecdspuuid.lib")

using Microsoft::WRL::ComPtr;

namespace rf {

AacEncoder::~AacEncoder() { Close(); }

Status AacEncoder::Open(const AudioFormat& format, std::uint32_t bitrate_bps, Ticks100ns epoch,
                        const std::function<void(PacketPtr)>& on_packet) {
    format_ = format;
    epoch_ = epoch;
    on_packet_ = on_packet;

    RF_HR(::CoCreateInstance(CLSID_AACMFTEncoder, nullptr, CLSCTX_INPROC_SERVER,
                             IID_PPV_ARGS(&transform_)));

    ComPtr<IMFMediaType> in_type;
    RF_HR(MFCreateMediaType(&in_type));
    RF_HR(in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
    RF_HR(in_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM));
    RF_HR(in_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, format.sample_rate));
    RF_HR(in_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, format.channels));
    RF_HR(in_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
    RF_HR(in_type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, format.channels * 2));
    RF_HR(in_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                             format.sample_rate * format.channels * 2));
    RF_HR(transform_->SetInputType(0, in_type.Get(), 0));

    ComPtr<IMFMediaType> out_type;
    RF_HR(MFCreateMediaType(&out_type));
    RF_HR(out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
    RF_HR(out_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC));
    RF_HR(out_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, format.sample_rate));
    RF_HR(out_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, format.channels));
    RF_HR(out_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
    RF_HR(out_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bitrate_bps / 8));
    RF_HR(transform_->SetOutputType(0, out_type.Get(), 0));

    RF_HR(transform_->GetOutputCurrentType(0, &output_type_));

    RF_HR(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
    open_ = true;
    RF_INFO("AAC encoder up: {} Hz {} ch {} kbps", format.sample_rate, format.channels,
            bitrate_bps / 1000);
    return Status::Ok();
}

Status AacEncoder::Feed(const float* interleaved, std::uint32_t frames, Ticks100ns timestamp) {
    if (!open_ || !interleaved || frames == 0) return Status::Ok();

    const std::size_t samples = static_cast<std::size_t>(frames) * format_.channels;
    convert_.resize(samples);
    for (std::size_t i = 0; i < samples; ++i) {
        const float v = std::clamp(interleaved[i], -1.0f, 1.0f);
        convert_[i] = static_cast<std::int16_t>(v * 32767.0f);
    }

    ComPtr<IMFMediaBuffer> buffer;
    const DWORD bytes = static_cast<DWORD>(samples * sizeof(std::int16_t));
    RF_HR(MFCreateMemoryBuffer(bytes, &buffer));
    BYTE* dst = nullptr;
    RF_HR(buffer->Lock(&dst, nullptr, nullptr));
    std::memcpy(dst, convert_.data(), bytes);
    RF_HR(buffer->Unlock());
    RF_HR(buffer->SetCurrentLength(bytes));

    ComPtr<IMFSample> sample;
    RF_HR(MFCreateSample(&sample));
    RF_HR(sample->AddBuffer(buffer.Get()));
    RF_HR(sample->SetSampleTime(timestamp - epoch_));
    RF_HR(sample->SetSampleDuration(static_cast<LONGLONG>(frames) * kOneSecond100ns /
                                    format_.sample_rate));

    RF_HR(transform_->ProcessInput(0, sample.Get(), 0));
    return Drain();
}

Status AacEncoder::Drain() {
    MFT_OUTPUT_STREAM_INFO info{};
    RF_HR(transform_->GetOutputStreamInfo(0, &info));

    for (;;) {
        ComPtr<IMFMediaBuffer> buffer;
        RF_HR(MFCreateMemoryBuffer(std::max<DWORD>(info.cbSize, 4096), &buffer));
        ComPtr<IMFSample> sample;
        RF_HR(MFCreateSample(&sample));
        RF_HR(sample->AddBuffer(buffer.Get()));

        MFT_OUTPUT_DATA_BUFFER out{};
        out.pSample = sample.Get();
        DWORD status = 0;
        const HRESULT hr = transform_->ProcessOutput(0, 1, &out, &status);
        if (out.pEvents) out.pEvents->Release();
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return Status::Ok();
        if (FAILED(hr)) return Status::Fail(hr, "AAC ProcessOutput");

        ComPtr<IMFMediaBuffer> contig;
        RF_HR(sample->ConvertToContiguousBuffer(&contig));
        BYTE* data = nullptr;
        DWORD length = 0;
        RF_HR(contig->Lock(&data, nullptr, &length));

        auto packet = std::make_shared<Packet>();
        packet->kind = MediaKind::Audio;
        packet->track = 1;
        packet->keyframe = true;
        packet->data.assign(data, data + length);
        contig->Unlock();

        LONGLONG pts = 0, duration = 0;
        sample->GetSampleTime(&pts);
        sample->GetSampleDuration(&duration);
        packet->pts = pts;
        packet->dts = pts;
        packet->duration = duration;

        if (on_packet_) on_packet_(std::move(packet));
    }
}

void AacEncoder::Close() {
    if (transform_ && open_) {
        transform_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        Drain();
        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }
    transform_.Reset();
    output_type_.Reset();
    open_ = false;
}

}
