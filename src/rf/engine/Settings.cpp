#include "rf/engine/Settings.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>

#include "rf/core/Log.h"
#include "rf/core/Paths.h"
#include "rf/core/Strings.h"

namespace rf {
namespace {

std::map<std::string, std::string> ReadIni(const std::filesystem::path& file) {
    std::map<std::string, std::string> out;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        out[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return out;
}

std::uint32_t GetU32(const std::map<std::string, std::string>& m, const char* key,
                     std::uint32_t fallback) {
    const auto it = m.find(key);
    if (it == m.end()) return fallback;
    try {
        return static_cast<std::uint32_t>(std::stoul(it->second));
    } catch (...) {
        return fallback;
    }
}

bool GetBool(const std::map<std::string, std::string>& m, const char* key, bool fallback) {
    const auto it = m.find(key);
    return it == m.end() ? fallback : (it->second == "1" || it->second == "true");
}

}

Settings Settings::Defaults() {
    Settings s;
    s.output_dir = paths::DefaultVideoDir();
    s.temp_dir = paths::DataDir() / L"temp";
    return s;
}

bool Settings::ClampFpsToDisplay(double display_hz) {
    if (capture_focused_window_only || display_hz <= 10.0) {
        fps_clamped_to_display = false;
        return false;
    }

    const auto index = std::min<std::size_t>(fps_option, std::size(kFpsValues) - 1);
    if (static_cast<double>(kFpsValues[index]) <= display_hz * 1.05) {

        return false;
    }

    std::size_t best = 0;
    for (std::size_t i = 0; i < std::size(kFpsValues); ++i)
        if (static_cast<double>(kFpsValues[i]) <= display_hz * 1.05) best = i;

    const bool changed = best != index;
    fps_option = static_cast<std::uint32_t>(best);
    fps = kFpsValues[best];
    fps_clamped_to_display = true;
    return changed;
}

std::uint32_t Settings::ResolvedFps() const {
    const auto index = std::min<std::size_t>(fps_option, std::size(kFpsValues) - 1);
    return kFpsValues[index];
}

std::uint32_t Settings::BitrateForQuality(std::uint32_t quality) {
    const std::uint32_t index =
        std::min<std::uint32_t>(quality, static_cast<std::uint32_t>(std::size(kQualityMbps) - 1));
    return kQualityMbps[index] * 1000;
}

void Settings::ApplyQualityPreset() {
    if (quality >= kQualityCustom) return;
    bitrate_kbps = BitrateForQuality(quality);
}

std::uint32_t Settings::ResolvedHeight() const {
    const auto index = std::min<std::size_t>(resolution, std::size(kResolutionHeights) - 1);
    return kResolutionHeights[index];
}

Settings Settings::Load(const std::filesystem::path& file) {
    Settings s = Defaults();
    if (!std::filesystem::exists(file)) {
        RF_INFO("no settings file at {} - using defaults", file.string());
        return s;
    }

    const auto m = ReadIni(file);
    s.fps = GetU32(m, "fps", s.fps);
    s.width = GetU32(m, "width", s.width);
    s.height = GetU32(m, "height", s.height);
    s.bitrate_kbps = GetU32(m, "bitrate_kbps", s.bitrate_kbps);
    s.keyframe_interval_ms = GetU32(m, "keyframe_interval_ms", s.keyframe_interval_ms);
    s.codec = static_cast<Codec>(GetU32(m, "codec", static_cast<std::uint32_t>(s.codec)));
    s.capture_backend = static_cast<CaptureBackend>(
        GetU32(m, "capture_backend", static_cast<std::uint32_t>(s.capture_backend)));
    s.encoder_backend = static_cast<EncoderBackend>(
        GetU32(m, "encoder_backend", static_cast<std::uint32_t>(s.encoder_backend)));
    s.capture_cursor = GetBool(m, "capture_cursor", s.capture_cursor);
    s.capture_focused_window_only =
        GetBool(m, "capture_focused_window_only", s.capture_focused_window_only);
    s.gpu_luid_low = static_cast<std::int32_t>(GetU32(m, "gpu_luid_low", 0));
    s.gpu_luid_high = static_cast<std::int32_t>(GetU32(m, "gpu_luid_high", 0));
    s.hdr = GetBool(m, "hdr", s.hdr);
    s.replay_enabled = GetBool(m, "replay_enabled", s.replay_enabled);
    s.replay_seconds = GetU32(m, "replay_seconds", s.replay_seconds);

    s.replay_max_memory_mb =
        std::max(GetU32(m, "replay_max_memory_mb", s.replay_max_memory_mb), 256u);
    s.record_system_audio = GetBool(m, "record_system_audio", s.record_system_audio);
    s.record_microphone = GetBool(m, "record_microphone", s.record_microphone);
    s.audio_bitrate_kbps = GetU32(m, "audio_bitrate_kbps", s.audio_bitrate_kbps);
    s.quality = std::min(GetU32(m, "quality", s.quality), kQualityCustom);

    s.bitrate_kbps = std::clamp(s.bitrate_kbps, kBitrateMinKbps, kBitrateMaxKbps);
    s.resolution = GetU32(m, "resolution", s.resolution);
    s.fps_option = GetU32(m, "fps_option", s.fps_option);
    s.audio_tracks = GetU32(m, "audio_tracks", s.audio_tracks);
    s.system_volume = static_cast<float>(GetU32(m, "system_volume_pct", 100)) / 100.0f;
    s.mic_volume = static_cast<float>(GetU32(m, "mic_volume_pct", 100)) / 100.0f;
    s.mic_gain = static_cast<float>(GetU32(m, "mic_gain_pct", 100)) / 100.0f;
    s.disk_limit_enabled = GetBool(m, "disk_limit_enabled", s.disk_limit_enabled);
    s.disk_limit_gb = GetU32(m, "disk_limit_gb", s.disk_limit_gb);
    s.hotkey_overlay_vk = GetU32(m, "hotkey_overlay_vk", s.hotkey_overlay_vk);
    s.hotkey_overlay_mods = GetU32(m, "hotkey_overlay_mods", s.hotkey_overlay_mods);
    s.hotkey_save_replay_vk = GetU32(m, "hotkey_save_replay_vk", s.hotkey_save_replay_vk);
    s.hotkey_save_replay_mods = GetU32(m, "hotkey_save_replay_mods", s.hotkey_save_replay_mods);
    s.hotkey_toggle_record_vk = GetU32(m, "hotkey_toggle_record_vk", s.hotkey_toggle_record_vk);
    s.hotkey_toggle_record_mods =
        GetU32(m, "hotkey_toggle_record_mods", s.hotkey_toggle_record_mods);

    if (const auto it = m.find("output_dir"); it != m.end() && !it->second.empty())
        s.output_dir = ToWide(it->second);
    if (const auto it = m.find("temp_dir"); it != m.end() && !it->second.empty())
        s.temp_dir = ToWide(it->second);
    if (const auto it = m.find("mic_device"); it != m.end()) s.mic_device = it->second;
    if (const auto it = m.find("capture_monitor"); it != m.end()) s.capture_monitor = it->second;
    s.monitor_follow_cursor = GetBool(m, "monitor_follow_cursor", s.monitor_follow_cursor);
    s.monitor_switch_delay_ms =
        std::min(GetU32(m, "monitor_switch_delay_ms", s.monitor_switch_delay_ms), 10'000u);
    s.ui_scale = std::clamp(static_cast<float>(GetU32(m, "ui_scale_pct", 100)) / 100.0f,
                            kUiScaleMin, kUiScaleMax);
    if (const auto it = m.find("filename_pattern"); it != m.end()) s.filename_pattern = it->second;

    return s;
}

bool Settings::Save(const std::filesystem::path& file) const {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);

    std::ofstream out(file, std::ios::trunc);
    if (!out) {
        RF_ERROR("cannot write settings to {}", file.string());
        return false;
    }

    out << "# Reframe++ settings\n";
    out << "fps=" << fps << "\n";
    out << "width=" << width << "\n";
    out << "height=" << height << "\n";
    out << "codec=" << static_cast<int>(codec) << "\n";
    out << "bitrate_kbps=" << bitrate_kbps << "\n";
    out << "keyframe_interval_ms=" << keyframe_interval_ms << "\n";
    out << "capture_backend=" << static_cast<int>(capture_backend) << "\n";
    out << "encoder_backend=" << static_cast<int>(encoder_backend) << "\n";
    out << "capture_cursor=" << (capture_cursor ? 1 : 0) << "\n";

    out << "capture_focused_window_only=" << (capture_focused_window_only ? 1 : 0) << "\n";
    out << "gpu_luid_low=" << static_cast<std::uint32_t>(gpu_luid_low) << "\n";
    out << "gpu_luid_high=" << static_cast<std::uint32_t>(gpu_luid_high) << "\n";
    out << "hdr=" << (hdr ? 1 : 0) << "\n";
    out << "replay_enabled=" << (replay_enabled ? 1 : 0) << "\n";
    out << "replay_seconds=" << replay_seconds << "\n";
    out << "replay_max_memory_mb=" << replay_max_memory_mb << "\n";
    out << "record_system_audio=" << (record_system_audio ? 1 : 0) << "\n";
    out << "record_microphone=" << (record_microphone ? 1 : 0) << "\n";
    out << "audio_bitrate_kbps=" << audio_bitrate_kbps << "\n";
    out << "quality=" << quality << "\n";
    out << "resolution=" << resolution << "\n";
    out << "fps_option=" << fps_option << "\n";
    out << "audio_tracks=" << audio_tracks << "\n";
    out << "system_volume_pct=" << static_cast<int>(system_volume * 100.0f + 0.5f) << "\n";
    out << "mic_volume_pct=" << static_cast<int>(mic_volume * 100.0f + 0.5f) << "\n";
    out << "mic_gain_pct=" << static_cast<int>(mic_gain * 100.0f + 0.5f) << "\n";
    out << "mic_device=" << mic_device << "\n";
    out << "capture_monitor=" << capture_monitor << "\n";
    out << "monitor_follow_cursor=" << (monitor_follow_cursor ? 1 : 0) << "\n";
    out << "monitor_switch_delay_ms=" << monitor_switch_delay_ms << "\n";
    out << "ui_scale_pct=" << static_cast<std::uint32_t>(ui_scale * 100.0f + 0.5f) << "\n";
    out << "disk_limit_enabled=" << (disk_limit_enabled ? 1 : 0) << "\n";
    out << "disk_limit_gb=" << disk_limit_gb << "\n";
    out << "output_dir=" << ToUtf8(output_dir.wstring()) << "\n";
    out << "temp_dir=" << ToUtf8(temp_dir.wstring()) << "\n";
    out << "filename_pattern=" << filename_pattern << "\n";
    out << "hotkey_overlay_vk=" << hotkey_overlay_vk << "\n";
    out << "hotkey_overlay_mods=" << hotkey_overlay_mods << "\n";
    out << "hotkey_save_replay_vk=" << hotkey_save_replay_vk << "\n";
    out << "hotkey_save_replay_mods=" << hotkey_save_replay_mods << "\n";
    out << "hotkey_toggle_record_vk=" << hotkey_toggle_record_vk << "\n";
    out << "hotkey_toggle_record_mods=" << hotkey_toggle_record_mods << "\n";
    return true;
}

}
