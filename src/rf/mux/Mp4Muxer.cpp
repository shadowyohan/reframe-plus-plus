#include "rf/mux/Mp4Muxer.h"

#include <algorithm>

#include <mfapi.h>
#include <mferror.h>

#include "rf/core/Log.h"

#pragma comment(lib, "mfreadwrite.lib")

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

}

Mp4Muxer::~Mp4Muxer() { Close(); }

Status Mp4Muxer::Open(const std::filesystem::path& file, const VideoFormat& video,
                      const CodecPrivate& codec_private, IMFMediaType* aac_type,
                      std::uint32_t audio_tracks) {
    path_ = file;
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);

    ComPtr<IMFAttributes> attrs;
    RF_HR(MFCreateAttributes(&attrs, 4));

    RF_HR(attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE));
    RF_HR(attrs->SetUINT32(MF_LOW_LATENCY, FALSE));
    RF_HR(attrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4));

    RF_HR(MFCreateSinkWriterFromURL(file.c_str(), nullptr, attrs.Get(), &writer_));

    ComPtr<IMFMediaType> video_type;
    RF_HR(MFCreateMediaType(&video_type));
    RF_HR(video_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    RF_HR(video_type->SetGUID(MF_MT_SUBTYPE, SubtypeFor(video.codec)));
    RF_HR(video_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    RF_HR(MFSetAttributeSize(video_type.Get(), MF_MT_FRAME_SIZE, video.width, video.height));
    RF_HR(MFSetAttributeRatio(video_type.Get(), MF_MT_FRAME_RATE, video.fps_num, video.fps_den));
    RF_HR(MFSetAttributeRatio(video_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
    if (!codec_private.data.empty()) {
        RF_HR(video_type->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, codec_private.data.data(),
                                  static_cast<UINT32>(codec_private.data.size())));
    }
    RF_HR(writer_->AddStream(video_type.Get(), &video_stream_));

    RF_HR(writer_->SetInputMediaType(video_stream_, video_type.Get(), nullptr));

    if (aac_type) {
        audio_tracks_ = std::min<std::uint32_t>(std::max<std::uint32_t>(audio_tracks, 1), 2);
        for (std::uint32_t i = 0; i < audio_tracks_; ++i) {
            RF_HR(writer_->AddStream(aac_type, &audio_stream_[i]));
            RF_HR(writer_->SetInputMediaType(audio_stream_[i], aac_type, nullptr));
        }
    }

    RF_HR(writer_->BeginWriting());
    started_ = true;
    RF_INFO("muxing to {}", path_.string());
    return Status::Ok();
}

Status Mp4Muxer::WritePacket(const Packet& packet) {
    if (!started_) return Status::Fail("muxer not open");

    if (packet.kind == MediaKind::Video && !seen_keyframe_) {
        if (!packet.keyframe) return Status::Ok();
        seen_keyframe_ = true;
        base_pts_ = packet.pts;
    }

    if (packet.kind == MediaKind::Audio && (!seen_keyframe_ || packet.pts < base_pts_))
        return Status::Ok();

    ComPtr<IMFMediaBuffer> buffer;
    RF_HR(MFCreateMemoryBuffer(static_cast<DWORD>(packet.data.size()), &buffer));

    BYTE* dst = nullptr;
    RF_HR(buffer->Lock(&dst, nullptr, nullptr));
    std::memcpy(dst, packet.data.data(), packet.data.size());
    RF_HR(buffer->Unlock());
    RF_HR(buffer->SetCurrentLength(static_cast<DWORD>(packet.data.size())));

    ComPtr<IMFSample> sample;
    RF_HR(MFCreateSample(&sample));
    RF_HR(sample->AddBuffer(buffer.Get()));
    RF_HR(sample->SetSampleTime(packet.pts - base_pts_));
    RF_HR(sample->SetSampleDuration(packet.duration));
    if (packet.keyframe) RF_HR(sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE));

    if (packet.kind == MediaKind::Audio && audio_tracks_ == 0) return Status::Ok();

    const std::uint32_t track = packet.track < audio_tracks_ ? packet.track : 0;
    const DWORD stream =
        packet.kind == MediaKind::Video ? video_stream_ : audio_stream_[track];

    RF_HR(writer_->WriteSample(stream, sample.Get()));
    bytes_written_ += packet.data.size();
    return Status::Ok();
}

Status Mp4Muxer::Close() {
    if (!writer_) return Status::Ok();
    if (started_) {

        const HRESULT hr = writer_->Finalize();
        if (FAILED(hr)) RF_ERROR("SinkWriter::Finalize failed 0x{:08X}", static_cast<unsigned>(hr));
        started_ = false;
    }
    writer_.Reset();
    RF_INFO("closed {} ({} bytes)", path_.string(), bytes_written_);
    return Status::Ok();
}

}
