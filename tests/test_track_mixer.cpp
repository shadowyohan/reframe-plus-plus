#include <windows.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>
#include <thread>

#include "rf/player/TrackMixer.h"

#include "test_framework.h"

TEST(TrackMixer_PlaysEveryTrackAndFollowsSeeks) {
    const auto file = std::filesystem::temp_directory_path() / "reframe-mixer-test.mp4";
    const std::string make = std::format(
        "ffmpeg -v error -y -f lavfi -i sine=frequency=440:duration=6 -f lavfi -i sine=frequency=880:duration=6 "
        "-map 0 -map 1 -c:a aac -ar 48000 -ac 2 \"{}\" 2>nul",
        file.string());
    if (std::system(make.c_str()) != 0 || !std::filesystem::exists(file)) {
        SKIP("ffmpeg is not installed");
        return;
    }

    rf::TrackMixer mixer;
    mixer.Open(file, 2);
    mixer.SetMasterVolume(0.0f);
    mixer.Play(1.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(900));
    const double played = mixer.position();
    CHECK(played > 1.4 && played < 2.2);

    mixer.Seek(4.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(mixer.position() > 4.0 && mixer.position() < 4.6);
    mixer.Close();
    std::filesystem::remove(file);
}
