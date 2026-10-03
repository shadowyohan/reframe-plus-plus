#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "rf/mux/ClipCropper.h"
#include "rf/mux/TrackNames.h"
#include "rf/ui/Widgets.h"

namespace rf {
class VideoPlayer;
class TrackMixer;
}

namespace rf::ui {

struct ClipEdit {
    std::filesystem::path file;
    bool replace_original = false;
    double trim_start = 0.0;
    double trim_end = 0.0;
    std::vector<AudioEdit> tracks;
    std::vector<std::string> kept_names;
    bool first_kept_is_full_mix = false;
    std::vector<ClipMarker> markers;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

class ClipEditor {
public:
    struct Hooks {
        VideoPlayer* player = nullptr;
        TrackMixer* mixer = nullptr;
        float player_volume = 1.0f;
        std::function<bool()> is_favorite;
        std::function<void()> on_toggle_favorite;
        std::function<void(const ClipEdit&)> on_save;
    };

    void Open(Hooks hooks, const std::filesystem::path& file, std::vector<ClipMarker> markers);
    void Close();

    [[nodiscard]] bool open() const { return open_; }
    [[nodiscard]] bool visible() const { return open_ || rise_.value() < hide_to_ - 10.0f; }
    [[nodiscard]] bool covering() const { return open_ && rise_.settled(); }
    [[nodiscard]] bool dirty() const;

    void Escape();

    void Draw(UiContext& ctx, ImVec2 screen, std::vector<ImVec4>& panels);

private:
    static constexpr float kHiddenOffset = 1000.0f;

    enum class Dialog { None, ConfirmClose, ChooseSave };

    struct TrackRow {
        std::size_t index = 0;
        std::string name;
        float gain = 1.0f;
        float gain_before_mute = 1.0f;
        bool removed = false;
        bool core = false;
    };

    void RequestClose();
    void DrawHeader(UiContext& ctx, ImVec2 min, float width);
    void DrawVideo(UiContext& ctx, ImVec2 min, ImVec2 max);
    void DrawTracksPanel(UiContext& ctx, ImVec2 min, ImVec2 max);
    void DrawControls(UiContext& ctx, ImVec2 min, float width);
    void DrawTimeline(UiContext& ctx, ImVec2 min, ImVec2 max);
    void DrawDialog(UiContext& ctx, ImVec2 screen, std::vector<ImVec4>& panels);
    void SyncPlayback();
    void TogglePlay();
    void SeekTo(double seconds);
    void Save(bool replace_original);
    [[nodiscard]] double duration() const;

    Hooks hooks_;
    bool open_ = false;
    std::filesystem::path file_;
    std::vector<ClipMarker> markers_;
    std::vector<TrackRow> tracks_;
    double trim_start_ = 0.0;
    double trim_end_ = 0.0;
    float volume_ = 1.0f;
    bool tracks_panel_ = false;
    bool playing_ = false;
    Dialog dialog_ = Dialog::None;
    Spring rise_{kHiddenOffset};
    Spring dialog_in_{0.0f};
    Spring tracks_in_{0.0f};
    float opened_for_ = 0.0f;
    float hide_to_ = kHiddenOffset;
};

}
