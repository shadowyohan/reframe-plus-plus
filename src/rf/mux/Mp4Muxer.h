#pragma once
#include <mfidl.h>
#include <mfreadwrite.h>

#include <filesystem>

#include <wrl/client.h>

#include "rf/core/Media.h"
#include "rf/core/Status.h"

namespace rf {

class Mp4Muxer {
public:
    ~Mp4Muxer();

    Status Open(const std::filesystem::path& file, const VideoFormat& video,
                const CodecPrivate& codec_private, IMFMediaType* aac_type,
                std::uint32_t audio_tracks = 1);

    Status WritePacket(const Packet& packet);
    Status Close();

    [[nodiscard]] std::uint64_t bytes_written() const { return bytes_written_; }

private:
    Microsoft::WRL::ComPtr<IMFSinkWriter> writer_;
    DWORD video_stream_ = 0;
    DWORD audio_stream_[2] = {};
    std::uint32_t audio_tracks_ = 0;
    bool started_ = false;
    bool seen_keyframe_ = false;

    std::int64_t base_pts_ = 0;
    std::uint64_t bytes_written_ = 0;
    std::filesystem::path path_;
};

}
