#include "rf/engine/Settings.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "test_framework.h"

using rf::Settings;

namespace {

std::filesystem::path TempFile() {
    return std::filesystem::temp_directory_path() / "reframe-settings-roundtrip.ini";
}

Settings Modified() {
    Settings s = Settings::Defaults();
    s.capture_backend = rf::CaptureBackend::WindowsGraphicsCapture;
    s.capture_cursor = false;
    s.capture_focused_window_only = true;
    s.gpu_luid_low = 0x1234;
    s.gpu_luid_high = 0x5678;
    s.capture_monitor = "\\\\.\\DISPLAY2";
    s.monitor_follow_cursor = true;
    s.monitor_switch_delay_ms = 7500;
    s.ui_scale = 1.75f;
    s.fps_option = 2;
    s.resolution = 3;
    s.quality = 1;
    s.bitrate_kbps = 17'000;
    s.keyframe_interval_ms = 1'500;
    s.codec = rf::Codec::HEVC;
    s.hdr = true;
    s.replay_enabled = false;
    s.replay_seconds = 420;
    s.replay_max_memory_mb = 4096;
    s.record_system_audio = false;
    s.record_microphone = true;
    s.audio_tracks = 1;
    s.audio_bitrate_kbps = 256;
    s.system_volume = 0.5f;
    s.mic_volume = 0.25f;
    s.mic_gain = 1.5f;
    s.mic_device = "{0.0.1.00000000}.{some-device-id}";
    s.disk_limit_enabled = false;
    s.disk_limit_gb = 321;
    s.filename_pattern = "{game}-{date}";
    s.hotkey_overlay_vk = 0x5A;
    s.hotkey_overlay_mods = 3;
    s.hotkey_save_replay_vk = 0x71;
    s.hotkey_save_replay_mods = 1;
    s.hotkey_toggle_record_vk = 0x72;
    s.hotkey_toggle_record_mods = 2;
    s.language = rf::Language::English;
    s.show_record_indicator = false;
    s.show_stop_button = false;
    s.show_mic_indicator = false;
    s.show_replay_indicator = false;
    s.hud_corner = 1;
    s.hud_badge_scale = 1.5f;
    s.hud_opacity = 0.4f;
    s.replay_in_memory = true;
    return s;
}

}

TEST(Settings_SurviveSaveAndLoad) {
    const Settings written = Modified();
    CHECK(written.Save(TempFile()));

    const Settings read = Settings::Load(TempFile());

    CHECK(read.capture_backend == written.capture_backend);
    CHECK(read.capture_cursor == written.capture_cursor);
    CHECK(read.capture_focused_window_only == written.capture_focused_window_only);
    CHECK_EQ(read.gpu_luid_low, written.gpu_luid_low);
    CHECK_EQ(read.gpu_luid_high, written.gpu_luid_high);
    CHECK(read.capture_monitor == written.capture_monitor);
    CHECK(read.monitor_follow_cursor == written.monitor_follow_cursor);
    CHECK_EQ(read.monitor_switch_delay_ms, written.monitor_switch_delay_ms);
    CHECK(read.ui_scale > 1.74f && read.ui_scale < 1.76f);
    CHECK_EQ(read.fps_option, written.fps_option);
    CHECK_EQ(read.resolution, written.resolution);
    CHECK_EQ(read.quality, written.quality);
    CHECK_EQ(read.bitrate_kbps, written.bitrate_kbps);
    CHECK_EQ(read.keyframe_interval_ms, written.keyframe_interval_ms);
    CHECK(read.codec == written.codec);
    CHECK(read.hdr == written.hdr);
    CHECK(read.replay_enabled == written.replay_enabled);
    CHECK_EQ(read.replay_seconds, written.replay_seconds);
    CHECK_EQ(read.replay_max_memory_mb, written.replay_max_memory_mb);
    CHECK(read.record_system_audio == written.record_system_audio);
    CHECK(read.record_microphone == written.record_microphone);
    CHECK_EQ(read.audio_tracks, written.audio_tracks);
    CHECK_EQ(read.audio_bitrate_kbps, written.audio_bitrate_kbps);
    CHECK(read.mic_device == written.mic_device);
    CHECK(read.disk_limit_enabled == written.disk_limit_enabled);
    CHECK_EQ(read.disk_limit_gb, written.disk_limit_gb);
    CHECK(read.filename_pattern == written.filename_pattern);
    CHECK(read.output_dir == written.output_dir);
    CHECK(read.temp_dir == written.temp_dir);
    CHECK_EQ(read.hotkey_overlay_vk, written.hotkey_overlay_vk);
    CHECK_EQ(read.hotkey_overlay_mods, written.hotkey_overlay_mods);
    CHECK_EQ(read.hotkey_save_replay_vk, written.hotkey_save_replay_vk);
    CHECK_EQ(read.hotkey_save_replay_mods, written.hotkey_save_replay_mods);
    CHECK_EQ(read.hotkey_toggle_record_vk, written.hotkey_toggle_record_vk);
    CHECK_EQ(read.hotkey_toggle_record_mods, written.hotkey_toggle_record_mods);
    CHECK(read.language == written.language);
    CHECK(read.show_record_indicator == written.show_record_indicator);
    CHECK(read.show_stop_button == written.show_stop_button);
    CHECK(read.show_mic_indicator == written.show_mic_indicator);
    CHECK(read.show_replay_indicator == written.show_replay_indicator);
    CHECK_EQ(read.hud_corner, written.hud_corner);
    CHECK(read.hud_badge_scale > 1.49f && read.hud_badge_scale < 1.51f);
    CHECK(read.hud_opacity > 0.39f && read.hud_opacity < 0.41f);
    CHECK(read.replay_in_memory == written.replay_in_memory);

    std::error_code ec;
    std::filesystem::remove(TempFile(), ec);
}

TEST(Settings_EveryFieldIsWrittenOut) {
    const Settings written = Modified();
    CHECK(written.Save(TempFile()));

    std::ifstream file(TempFile());
    std::string body((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    for (const char* key : {"capture_monitor", "monitor_follow_cursor", "monitor_switch_delay_ms",
                            "ui_scale_pct", "record_microphone", "audio_tracks", "gpu_luid_low",
                            "language", "show_record_indicator", "show_stop_button",
                            "show_mic_indicator", "show_replay_indicator", "hud_corner",
                            "hud_badge_scale_pct", "hud_opacity_pct", "replay_in_memory"})
        CHECK(body.find(std::string(key) + "=") != std::string::npos);

    std::error_code ec;
    std::filesystem::remove(TempFile(), ec);
}
