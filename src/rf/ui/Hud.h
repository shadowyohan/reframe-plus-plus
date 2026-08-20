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
    enum class Kind { ReplaySaved, ReplayArmed, RecordingStarted, RecordingStopped, Error };

    struct Badges {
        bool mic = false;
        bool mic_muted = false;
        bool replay = false;
        std::uint32_t corner = 3;
        float scale = 1.0f;
        float opacity = 1.0f;
    };

    void Push(Kind kind, std::string text, std::string thumb_key = {}, std::string app = {});
    void SetRecording(bool recording, double seconds, std::string app = {});
    void SetChrome(bool indicator, bool stop_button);
    void SetBadges(const Badges& badges) { badges_ = badges; }

    void Draw(UiContext& ctx, ImVec2 screen, const TextureCache& textures);

    [[nodiscard]] bool busy() const { return needs_top() || badges_.mic || badges_.replay; }

    [[nodiscard]] bool needs_top() const { return !toasts_.empty() || pill_.value() > 0.01f; }

    std::function<void()> on_stop;

    [[nodiscard]] ImVec4 pill_rect() const { return pill_rect_; }

    [[nodiscard]] const std::vector<ImVec4>& hit_rects() const { return hit_rects_; }

private:
    struct Toast {
        Kind kind = Kind::ReplaySaved;
        std::string text;
        std::string app;
        std::string thumb_key;
        float age = 0.0f;
        bool leaving = false;
        Spring slide{600.0f};
        Spring icon{0.0f};
    };

    void DrawToastIcon(UiContext& ctx, const Toast& toast, ImVec2 card_pos,
                       const TextureCache& textures);

    void DrawBadges(UiContext& ctx, ImVec2 screen);

    std::deque<Toast> toasts_;
    Spring pill_{0.0f};
    ImVec4 pill_rect_{0, 0, 0, 0};
    std::vector<ImVec4> hit_rects_;
    bool recording_ = false;
    double recording_seconds_ = 0.0;
    std::string recording_app_;
    Badges badges_;
    bool show_indicator_ = true;
    bool show_stop_ = true;
};

}
