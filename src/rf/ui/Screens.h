#pragma once
#include <functional>
#include <string>
#include <vector>

#include "rf/engine/Recorder.h"
#include "rf/engine/Settings.h"
#include "rf/gallery/Gallery.h"
#include "rf/player/VideoPlayer.h"
#include "rf/ui/Widgets.h"

namespace rf::ui {

struct AppModel {
    Settings* settings = nullptr;
    Gallery* gallery = nullptr;

    std::vector<GalleryItem> gallery_items;

    double display_hz = 0.0;

    bool recording = false;
    bool replay_armed = false;
    double recording_seconds = 0.0;
    std::uint64_t disk_used_bytes = 0;
    std::uint64_t disk_total_bytes = 0;
    std::string mic_device_label = "Системный по умолчанию";
    std::string build_line = "reframe++ 1.1 © | Build: 512. All rights reserved";

    std::string record_hotkey = "ALT+F9";
    std::string replay_hotkey = "ALT+F10";

    struct MicOption {
        std::string id;
        std::string name;
    };
    std::vector<MicOption> mic_devices;
    std::string mic_selected_id;

    struct GpuOption {
        std::string name;
        std::int32_t luid_low = 0;
        std::int32_t luid_high = 0;
    };
    std::vector<GpuOption> gpus;

    struct MonitorOption {
        std::string name;
        std::string device_name;
    };
    std::vector<MonitorOption> monitors;

    std::function<void()> on_toggle_record;
    std::function<void(const std::string&)> on_pick_mic;
    std::function<void(bool)> on_set_replay;
    std::function<void()> on_save_replay;
    std::function<void()> on_settings_changed;
    std::function<void(const std::filesystem::path&)> on_open_file;
    std::function<void(const std::filesystem::path&)> on_reveal_file;
    std::function<void(const std::filesystem::path&)> on_delete_file;
    std::function<bool(std::filesystem::path&)> on_pick_folder;
    std::function<void()> on_close;

    std::function<bool()> on_hotkeys_changed;

    std::uint32_t pending_vk = 0;
    std::uint32_t pending_mods = 0;

    VideoPlayer* player = nullptr;
};

std::string DescribeHotkey(std::uint32_t mods, std::uint32_t vk);

class Menu {
public:
    enum class Page { Main, Settings, Video, Audio, Disk, Keybinds, Gallery };

    void Open();
    void Close();

    void ToggleOpen();

    [[nodiscard]] bool open() const { return open_; }

    [[nodiscard]] bool visible() const;

    void Draw(UiContext& ctx, ImVec2 screen, AppModel& model, TextureCache& textures);

    void OpenInPlayer(AppModel& model, const std::filesystem::path& file);
    void ClosePlayer(AppModel& model);

    void ClosePlayer();
    [[nodiscard]] bool player_open() const { return player_open_; }

    [[nodiscard]] bool capturing_key() const { return capture_row_ >= 0; }

private:
    void PlayerPanel(UiContext& ctx, ImVec2 screen, AppModel& model);

    void Navigate(Page page, bool forward);
    void DrawPage(UiContext& ctx, Page page, ImVec2 panel_min, ImVec2 panel_max, AppModel& model,
                  TextureCache& textures);

    void PageMain(UiContext&, ImVec2, ImVec2, AppModel&, TextureCache&);
    void PageSettings(UiContext&, ImVec2, ImVec2, AppModel&);
    void PageVideo(UiContext&, ImVec2, ImVec2, AppModel&);
    void PageAudio(UiContext&, ImVec2, ImVec2, AppModel&);
    void PageDisk(UiContext&, ImVec2, ImVec2, AppModel&);
    void PageKeybinds(UiContext&, ImVec2, ImVec2, AppModel&);
    void PageGallery(UiContext&, ImVec2, ImVec2, AppModel&, TextureCache&);

    bool open_ = false;
    Page page_ = Page::Main;
    Page previous_page_ = Page::Main;
    bool mic_list_open_ = false;
    bool gpu_list_open_ = false;
    bool monitor_list_open_ = false;

    Spring slide_{-700.0f};
    Spring transition_{1.0f};
    bool forward_ = true;
    bool transitioning_ = false;

    ScrollArea scroll_;

    ScrollArea page_scroll_;
    float content_bottom_ = 0.0f;
    float page_height_[7] = {};

    VideoPlayer* player_ = nullptr;

    int capture_row_ = -1;

    int rejected_row_ = -1;

    bool player_open_ = false;
    Spring player_slide_{60.0f};
    float volume_ = 1.0f;
};

}
