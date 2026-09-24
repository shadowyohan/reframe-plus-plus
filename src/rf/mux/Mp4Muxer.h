#pragma once
#include <mfidl.h>
#include <mfreadwrite.h>

#include <filesystem>
#include <vector>

#include <wrl/client.h>

#include "rf/core/Media.h"
#include "rf/core/Status.h"

namespace rf {

inline constexpr std::uint32_t kMaxAudioTracks = 22;

class Mp4Muxer {
public:
    ~Mp4Muxer();

    Status Open(const std::filesystem::path& file, const VideoFormat& video,
                const CodecPrivate& codec_private, IMFMediaType* aac_type,
                std::uint32_t audio_tracks = 1, const std::vector<std::uint32_t>& kept_tracks = {});

    Status WritePacket(const Packet& packet);
    Status Close();

    [[nodiscard]] std::uint64_t bytes_written() const { return bytes_written_; }
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    Microsoft::WRL::ComPtr<IMFSinkWriter> writer_;
    DWORD video_stream_ = 0;
    std::vector<DWORD> audio_stream_;
    std::vector<std::int32_t> track_to_stream_;
    bool started_ = false;
    bool seen_keyframe_ = false;

    std::int64_t base_pts_ = 0;
    std::uint64_t bytes_written_ = 0;
    std::filesystem::path path_;
};

Status RemuxKeepingAudioTracks(const std::filesystem::path& file,
                               const std::vector<std::uint32_t>& kept_tracks);

}
