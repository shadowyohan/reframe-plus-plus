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

    void Push(Kind kind, std::string text, std::string thumb_key = {}, std::string app = {});
    void SetRecording(bool recording, double seconds, std::string app = {});

    void Draw(UiContext& ctx, ImVec2 screen, const TextureCache& textures);

    [[nodiscard]] bool busy() const { return !toasts_.empty() || pill_.value() > 0.01f; }

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

    std::deque<Toast> toasts_;
    Spring pill_{0.0f};
    ImVec4 pill_rect_{0, 0, 0, 0};
    std::vector<ImVec4> hit_rects_;
    bool recording_ = false;
    double recording_seconds_ = 0.0;
    std::string recording_app_;
};

}
