#pragma once
#include <filesystem>
#include <string>
#include <vector>

#include "rf/core/Status.h"

namespace rf {

Status WriteAudioTrackNames(const std::filesystem::path& file,
                            const std::vector<std::string>& audio_track_names,
                            bool first_track_is_full_mix = false);

[[nodiscard]] bool FirstAudioTrackIsFullMix(const std::filesystem::path& file);

}
