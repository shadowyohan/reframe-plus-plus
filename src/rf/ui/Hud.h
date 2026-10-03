#pragma once
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "rf/ui/Assets.h"
#include "rf/ui/Widgets.h"

namespace rf::ui {

class Hud {
public:
    enum class Kind {
        ReplaySaved,
        ReplayArmed,
        RecordingStarted,
        RecordingStopped,
        Error,
        TrackLimit,
        Download,
        AppSupport,
        Processing,
        Update,
    };

    static constexpr std::uint32_t kCustomCorner = 4;

    struct Badges {
        bool mic = false;
        bool mic_muted = false;
        bool replay = false;
        std::uint32_t corner = 3;
        std::uint32_t record_corner = 0;
        ImVec2 badges_at{0.0f, 0.0f};
        ImVec2 record_at{0.0f, 0.0f};
        float scale = 1.0f;
        float opacity = 1.0f;
    };

    void Push(Kind kind, std::string text, std::string thumb_key = {}, std::string app = {});
    void PushTrackLimit(std::string app);
    void PushProcessing(std::uint64_t id, std::string text, std::string app);
    void PushUpdate(std::string text, std::string version, bool can_download);
    void SetProcessingProgress(std::uint64_t id, float progress);
    void FinishProcessing(std::uint64_t id, Kind kind, std::string text, std::string thumb_key,
                          std::string app);
    void SetDownload(std::string text, std::string product, float progress,
                     std::string icon = "nvidia");
    void EndDownload();
    void SetRecording(bool recording, double seconds, std::string app = {});
    void SetChrome(bool indicator, bool stop_button);
    void SetBadges(const Badges& badges) { badges_ = badges; }

    void Draw(UiContext& ctx, ImVec2 screen, const TextureCache& textures);

    [[nodiscard]] bool busy() const { return needs_top() || badges_.mic || badges_.replay; }

    [[nodiscard]] bool needs_top() const { return !toasts_.empty() || pill_.value() > 0.01f; }

    std::function<void()> on_stop;
    std::function<void(const Badges&)> on_layout_moved;

    void SetLayoutEditing(bool editing) { editing_layout_ = editing; }
    std::function<void()> on_download_update;

    [[nodiscard]] ImVec4 pill_rect() const { return pill_rect_; }

    [[nodiscard]] const std::vector<ImVec4>& hit_rects() const { return hit_rects_; }

    [[nodiscard]] const std::vector<ImVec4>& button_rects() const { return button_rects_; }

private:
    struct Toast {
        Kind kind = Kind::ReplaySaved;
        std::string text;
        std::string app;
        std::string thumb_key;
        std::string subtitle;
        std::string icon_name;
        bool offers_download = false;
        float progress = -1.0f;
        std::uint64_t id = 0;
        float lifetime = 0.0f;
        bool sticky = false;
        float age = 0.0f;
        bool leaving = false;
        Spring slide{600.0f};
        Spring icon{0.0f};
    };

    void DrawToastIcon(UiContext& ctx, const Toast& toast, ImVec2 card_pos,
                       const TextureCache& textures);

    void DrawBadges(UiContext& ctx, ImVec2 screen, bool& moved);
    ImVec2 DragGroup(UiContext& ctx, std::uint32_t id, ImVec2 pos, ImVec2 size, ImVec2 screen,
                     ImVec2 margin, bool& moved);
    void DrawDownloadButton(UiContext& ctx, Toast& toast, ImVec2 card_pos);

    std::deque<Toast> toasts_;
    Spring pill_{0.0f};
    ImVec4 pill_rect_{0, 0, 0, 0};
    std::vector<ImVec4> hit_rects_;
    std::vector<ImVec4> button_rects_;
    bool recording_ = false;
    double recording_seconds_ = 0.0;
    std::string recording_app_;
    Badges badges_;
    bool show_indicator_ = true;
    bool show_stop_ = true;
    bool editing_layout_ = false;
    ImVec2 grab_{0.0f, 0.0f};
};

}
