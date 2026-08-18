#pragma once
#include <filesystem>
#include <string>

#include "rf/capture/IVideoCapture.h"
#include "rf/core/Media.h"
#include "rf/encode/IVideoEncoder.h"

namespace rf {

inline constexpr float kUiScaleMin = 0.5f;
inline constexpr float kUiScaleMax = 2.0f;

struct Settings {

    CaptureBackend capture_backend = CaptureBackend::Auto;
    bool capture_cursor = true;
    bool capture_focused_window_only = false;

    std::int32_t gpu_luid_low = 0;
    std::int32_t gpu_luid_high = 0;

    [[nodiscard]] bool has_gpu_override() const { return gpu_luid_low || gpu_luid_high; }

    std::string capture_monitor;

    bool monitor_follow_cursor = false;

    std::uint32_t monitor_switch_delay_ms = 3000;

    float ui_scale = 1.0f;
    std::uint32_t fps = 60;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    EncoderBackend encoder_backend = EncoderBackend::Auto;
    Codec codec = Codec::H264;
    RateControl rate_control = RateControl::CBR;
    std::uint32_t bitrate_kbps = 20'000;
    std::uint32_t keyframe_interval_ms = 2'000;
    bool hdr = false;

    bool replay_enabled = true;
    std::uint32_t replay_seconds = 300;

    std::uint32_t replay_max_memory_mb = 8192;

    std::uint32_t quality = 2;
    std::uint32_t resolution = 0;
    std::uint32_t fps_option = 1;

    bool record_system_audio = true;
    bool record_microphone = false;
    std::uint32_t audio_bitrate_kbps = 192;
    float system_volume = 1.0f;
    float mic_volume = 1.0f;
    float mic_gain = 1.0f;
    std::string mic_device;

    std::uint32_t audio_tracks = 0;
    [[nodiscard]] bool separate_audio_tracks() const { return audio_tracks != 0; }

    bool disk_limit_enabled = true;
    std::uint32_t disk_limit_gb = 200;
    std::filesystem::path temp_dir;

    std::filesystem::path output_dir;
    std::string filename_pattern = "{game}_{date}_{time}";

    std::uint32_t hotkey_save_replay_vk = 0x79;
    std::uint32_t hotkey_save_replay_mods = 0x0001 ;
    std::uint32_t hotkey_toggle_record_vk = 0x78;
    std::uint32_t hotkey_toggle_record_mods = 0x0001;
    std::uint32_t hotkey_overlay_vk = 0x5A;
    std::uint32_t hotkey_overlay_mods = 0x0001;

    static Settings Defaults();
    static Settings Load(const std::filesystem::path& file);
    bool Save(const std::filesystem::path& file) const;

    [[nodiscard]] std::uint32_t ResolvedFps() const;
    [[nodiscard]] std::uint32_t ResolvedHeight() const;

    [[nodiscard]] static std::uint32_t BitrateForQuality(std::uint32_t quality);

    void ApplyQualityPreset();

    bool ClampFpsToDisplay(double display_hz);

    bool fps_clamped_to_display = false;
};

inline constexpr const char* kQualityNames[] = {"Низкое", "Среднее", "Высокое", "Своё"};

inline constexpr std::uint32_t kQualityCustom = 3;

inline constexpr std::uint32_t kQualityMbps[] = {10, 15, 20};

inline constexpr std::uint32_t kBitrateMinKbps = 5'000;
inline constexpr std::uint32_t kBitrateMaxKbps = 30'000;

inline constexpr const char* kResolutionNames[] = {"Экран", "720p HD", "1080p HD", "1440p QHD",
                                                   "2160p 4K"};
inline constexpr std::uint32_t kResolutionHeights[] = {0, 720, 1080, 1440, 2160};
inline constexpr const char* kFpsNames[] = {"30 FPS", "60 FPS", "120 FPS", "144 FPS"};
inline constexpr std::uint32_t kFpsValues[] = {30, 60, 120, 144};
inline constexpr const char* kAudioTrackNames[] = {"Одна дорожка", "Раздельно"};

}
