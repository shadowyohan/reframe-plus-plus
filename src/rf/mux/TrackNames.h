#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "rf/core/Status.h"

namespace rf {

Status WriteAudioTrackNames(const std::filesystem::path& file,
                            const std::vector<std::string>& audio_track_names,
                            bool first_track_is_full_mix = false);

[[nodiscard]] bool FirstAudioTrackIsFullMix(const std::filesystem::path& file);
[[nodiscard]] std::vector<std::string> ReadAudioTrackNames(const std::filesystem::path& file);
Status RepairDurations(const std::filesystem::path& file);

[[nodiscard]] std::vector<std::size_t> MediaFoundationAudioOrder(const std::filesystem::path& file);

struct ClipMarker {
    std::uint32_t ms = 0;
    std::string tag;

    bool operator==(const ClipMarker&) const = default;
};

Status WriteClipMarkers(const std::filesystem::path& file, const std::vector<ClipMarker>& markers);
[[nodiscard]] std::vector<ClipMarker> ReadClipMarkers(const std::filesystem::path& file);

}
