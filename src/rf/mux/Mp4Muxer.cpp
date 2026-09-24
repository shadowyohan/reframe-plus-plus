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
                      std::uint32_t audio_tracks, const std::vector<std::uint32_t>& kept_tracks) {
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
        const std::uint32_t tracks = std::clamp<std::uint32_t>(audio_tracks, 1, kMaxAudioTracks);
        track_to_stream_.assign(tracks, -1);
        for (std::uint32_t track = 0; track < tracks; ++track) {
            const bool kept = kept_tracks.empty() ||
                              std::find(kept_tracks.begin(), kept_tracks.end(), track) !=
                                  kept_tracks.end();
            if (!kept) continue;
            DWORD stream = 0;
            RF_HR(writer_->AddStream(aac_type, &stream));
            RF_HR(writer_->SetInputMediaType(stream, aac_type, nullptr));
            track_to_stream_[track] = static_cast<std::int32_t>(audio_stream_.size());
            audio_stream_.push_back(stream);
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

    if (packet.kind == MediaKind::Audio &&
        (packet.track >= track_to_stream_.size() || track_to_stream_[packet.track] < 0))
        return Status::Ok();

    const DWORD stream = packet.kind == MediaKind::Video
                             ? video_stream_
                             : audio_stream_[static_cast<std::size_t>(track_to_stream_[packet.track])];

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

namespace {

Status WriteTrimmedCopy(const std::filesystem::path& file, const std::filesystem::path& trimmed,
                        const std::vector<std::uint32_t>& kept_tracks) {
    {
        ComPtr<IMFSourceReader> reader;
        RF_HR(MFCreateSourceReaderFromURL(file.c_str(), nullptr, &reader));

        ComPtr<IMFAttributes> attrs;
        RF_HR(MFCreateAttributes(&attrs, 2));
        RF_HR(attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE));
        RF_HR(attrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4));
        ComPtr<IMFSinkWriter> writer;
        RF_HR(MFCreateSinkWriterFromURL(trimmed.c_str(), nullptr, attrs.Get(), &writer));

        std::vector<std::int32_t> reader_to_writer;
        std::uint32_t audio_ordinal = 0;
        for (DWORD index = 0;; ++index) {
            ComPtr<IMFMediaType> type;
            const HRESULT found = reader->GetNativeMediaType(index, 0, &type);
            if (found == MF_E_INVALIDSTREAMNUMBER) break;
            RF_HR(found);

            GUID major{};
            RF_HR(type->GetMajorType(&major));
            bool keep = major == MFMediaType_Video;
            if (major == MFMediaType_Audio) {
                keep = std::find(kept_tracks.begin(), kept_tracks.end(), audio_ordinal) !=
                       kept_tracks.end();
                ++audio_ordinal;
            }

            RF_HR(reader->SetStreamSelection(index, keep));
            reader_to_writer.push_back(-1);
            if (!keep) continue;
            RF_HR(reader->SetCurrentMediaType(index, nullptr, type.Get()));
            DWORD stream = 0;
            RF_HR(writer->AddStream(type.Get(), &stream));
            RF_HR(writer->SetInputMediaType(stream, type.Get(), nullptr));
            reader_to_writer.back() = static_cast<std::int32_t>(stream);
        }

        RF_HR(writer->BeginWriting());
        std::size_t open_streams = static_cast<std::size_t>(
            std::count_if(reader_to_writer.begin(), reader_to_writer.end(),
                          [](std::int32_t stream) { return stream >= 0; }));
        while (open_streams > 0) {
            DWORD index = 0, flags = 0;
            LONGLONG timestamp = 0;
            ComPtr<IMFSample> sample;
            RF_HR(reader->ReadSample(MF_SOURCE_READER_ANY_STREAM, 0, &index, &flags, &timestamp,
                                     &sample));
            if (index >= reader_to_writer.size() || reader_to_writer[index] < 0) continue;
            const auto stream = static_cast<DWORD>(reader_to_writer[index]);

            if (sample) RF_HR(writer->WriteSample(stream, sample.Get()));
            else if (flags & MF_SOURCE_READERF_STREAMTICK) RF_HR(writer->SendStreamTick(stream, timestamp));
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                reader_to_writer[index] = -1;
                --open_streams;
            }
        }
        RF_HR(writer->Finalize());
    }
    return Status::Ok();
}

}

Status RemuxKeepingAudioTracks(const std::filesystem::path& file,
                               const std::vector<std::uint32_t>& kept_tracks) {
    std::filesystem::path trimmed = file;
    trimmed += L".trim.mp4";
    if (auto s = WriteTrimmedCopy(file, trimmed, kept_tracks); !s.ok()) {
        std::error_code ec;
        std::filesystem::remove(trimmed, ec);
        return s;
    }

    constexpr int kReplaceAttempts = 20;
    constexpr DWORD kReplaceRetryMs = 250;
    bool replaced = false;
    for (int attempt = 0; attempt < kReplaceAttempts && !replaced; ++attempt) {
        replaced = ::MoveFileExW(trimmed.c_str(), file.c_str(),
                                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
        if (!replaced) ::Sleep(kReplaceRetryMs);
    }
    if (!replaced) {
        const HRESULT moved = HRESULT_FROM_WIN32(::GetLastError());
        std::error_code ec;
        std::filesystem::remove(trimmed, ec);
        return Status::Fail(moved, "replacing the recording with its trimmed copy");
    }
    return Status::Ok();
}

}
