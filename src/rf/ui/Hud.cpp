#include "rf/ui/Hud.h"

#include "rf/core/Lang.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace rf::ui {
namespace {

using namespace theme;

constexpr float kToastLifetime = 3.5f;
constexpr float kUpdateToastLifetime = 10.0f;
constexpr float kDownloadButtonW = 92.0f;
constexpr float kDownloadButtonH = 34.0f;

constexpr float kIconDelaySaved = 0.18f;
constexpr float kIconDelay = 0.13f;
constexpr float kTextDelay = 0.20f;
constexpr float kTextFade = 0.161f;
constexpr int kMaxToasts = 3;

constexpr float kDegToRad = 3.14159265f / 180.0f;

constexpr ImU32 kNvidiaGreen = IM_COL32(0x76, 0xb9, 0x00, 255);
constexpr ImU32 kProgressTrack = IM_COL32(255, 255, 255, 26);
constexpr float kProgressH = 6.0f;
constexpr float kDetailGap = 5.0f;

bool HasWideIcon(Hud::Kind kind) {
    return kind == Hud::Kind::TrackLimit || kind == Hud::Kind::Download;
}

struct Word {
    std::string text;
    bool accent = false;
    float width = 0.0f;
};

std::vector<Word> SplitAccented(UiContext& ctx, const std::string& original,
                                const std::string& app) {
    const std::string text(Tr(std::string_view(original)));
    const std::size_t app_begin = app.empty() ? std::string::npos : text.find(app);
    const std::size_t app_end = app_begin == std::string::npos ? 0 : app_begin + app.size();

    std::vector<Word> words;
    for (std::size_t i = 0; i < text.size();) {
        std::size_t end = text.find(' ', i);
        if (end == std::string::npos) end = text.size();

        Word w;
        w.text = text.substr(i, end - i);
        w.accent = app_begin != std::string::npos && i >= app_begin && i < app_end;
        w.width = MeasureText(ctx, Font::Toast, w.text.c_str()).x;
        words.push_back(std::move(w));
        i = end + 1;
    }
    return words;
}

float DrawAccentedText(UiContext& ctx, const std::vector<Word>& words, float x, float center_y,
                       float max_width, float line_height, bool draw, ImU32 accent = kAppAccent) {
    const float space = MeasureText(ctx, Font::Toast, " ").x;

    int lines = 1;
    float run = 0.0f;
    for (const Word& w : words) {
        const float advance = run > 0.0f ? space + w.width : w.width;
        if (run > 0.0f && run + advance > max_width) {
            ++lines;
            run = w.width;
        } else {
            run += advance;
        }
    }
    const float total_h = lines * line_height;
    if (!draw) return total_h;

    float pen_x = x;
    float pen_y = center_y - total_h * 0.5f;
    run = 0.0f;
    for (const Word& w : words) {
        const float advance = run > 0.0f ? space + w.width : w.width;
        if (run > 0.0f && run + advance > max_width) {
            pen_x = x;
            pen_y += line_height;
            run = 0.0f;
        } else if (run > 0.0f) {
            pen_x += space;
        }
        DrawText(ctx, Font::Toast, ImVec2(pen_x, pen_y), w.accent ? accent : kText,
                 w.text.c_str());
        pen_x += w.width;
        run += advance;
    }
    return total_h;
}

const char* IconFor(Hud::Kind kind) {
    switch (kind) {
        case Hud::Kind::ReplaySaved:      return "repeat-circle";
        case Hud::Kind::ReplayArmed:      return "repeat-circle";
        case Hud::Kind::RecordingStarted: return "record-dot";
        case Hud::Kind::RecordingStopped: return "video-circle";
        case Hud::Kind::Error:            return "setting-3";
        case Hud::Kind::TrackLimit:       return "warning-hex";
        case Hud::Kind::Download:         return "nvidia";
        case Hud::Kind::AppSupport:       return "code";
        case Hud::Kind::Processing:       return "repeat-circle";
        case Hud::Kind::Update:           return "refresh-circle";
    }
    return "repeat-circle";
}

float IconDelayFor(Hud::Kind kind) {
    return kind == Hud::Kind::ReplaySaved ? kIconDelaySaved : kIconDelay;
}

const SpringParams& IconSpringFor(Hud::Kind kind) {
    switch (kind) {
        case Hud::Kind::ReplaySaved:      return spring::kSavedThumb;
        case Hud::Kind::ReplayArmed:      return spring::kArmedSpin;
        case Hud::Kind::RecordingStarted: return spring::kSoft;
        default:                          return spring::kQuick;
    }
}

}

void Hud::Push(Kind kind, std::string text, std::string thumb_key, std::string app) {
    Toast toast;
    toast.kind = kind;
    toast.text = std::move(text);
    toast.app = std::move(app);
    toast.thumb_key = std::move(thumb_key);
    toast.lifetime = kToastLifetime;
    toast.slide.Reset(600.0f);
    toast.slide.SetTarget(0.0f);
    toast.icon.Reset(0.0f);
    toasts_.push_back(std::move(toast));

    while (toasts_.size() > kMaxToasts) {
        const auto evictable = std::find_if(toasts_.begin(), toasts_.end(),
                                            [](const Toast& t) { return !t.sticky; });
        if (evictable == toasts_.end()) break;
        toasts_.erase(evictable);
    }
}

void Hud::PushTrackLimit(std::string app) {
    Push(Kind::TrackLimit, TrFormat("Звук из {} не записывается на свою дорожку.", app), {}, app);
    toasts_.back().subtitle = "Достигнут лимит дорожек.";
}

void Hud::PushProcessing(std::uint64_t id, std::string text, std::string app) {
    Push(Kind::Processing, std::move(text), {}, std::move(app));
    toasts_.back().id = id;
    toasts_.back().sticky = true;
    toasts_.back().progress = 0.0f;
}

void Hud::PushUpdate(std::string text, std::string version, bool can_download) {
    Push(Kind::Update, std::move(text), {}, std::move(version));
    toasts_.back().lifetime = kUpdateToastLifetime;
    toasts_.back().offers_download = can_download;
}

void Hud::SetProcessingProgress(std::uint64_t id, float progress) {
    for (Toast& toast : toasts_)
        if (toast.id == id && toast.kind == Kind::Processing)
            toast.progress = std::clamp(progress, toast.progress, 1.0f);
}

void Hud::FinishProcessing(std::uint64_t id, Kind kind, std::string text, std::string thumb_key,
                           std::string app) {
    const auto it = std::find_if(toasts_.begin(), toasts_.end(),
                                 [id](const Toast& t) { return t.id == id && !t.leaving; });
    if (it == toasts_.end()) {
        Push(kind, std::move(text), std::move(thumb_key), std::move(app));
        return;
    }
    it->kind = kind;
    it->text = std::move(text);
    it->thumb_key = std::move(thumb_key);
    it->app = std::move(app);
    it->progress = -1.0f;
    it->sticky = false;
    it->age = 0.0f;
    it->lifetime = kToastLifetime;
    it->icon.Reset(0.0f);
}

void Hud::SetDownload(std::string text, std::string product, float progress, std::string icon) {
    auto it = std::find_if(toasts_.begin(), toasts_.end(),
                           [](const Toast& t) { return t.kind == Kind::Download && t.sticky; });
    if (it == toasts_.end()) {
        Push(Kind::Download, text, {}, product);
        it = std::prev(toasts_.end());
        it->sticky = true;
    }
    it->text = std::move(text);
    it->app = std::move(product);
    it->icon_name = std::move(icon);
    it->progress = std::clamp(progress, 0.0f, 1.0f);
    it->subtitle = std::format("{} / 100%", static_cast<int>(it->progress * 100.0f + 0.5f));
}

void Hud::EndDownload() {
    for (Toast& toast : toasts_) {
        if (toast.kind != Kind::Download || !toast.sticky) continue;
        toast.sticky = false;
        toast.age = std::max(toast.age, toast.lifetime - 1.5f);
    }
}

void Hud::SetRecording(bool recording, double seconds, std::string app) {
    recording_ = recording;
    recording_seconds_ = seconds;
    recording_app_ = std::move(app);
    pill_.SetTarget(recording ? 1.0f : 0.0f);
}

void Hud::DrawToastIcon(UiContext& ctx, const Toast& toast, ImVec2 card_pos,
                        const TextureCache& textures) {
    const float t = toast.icon.value();
    const float saved_alpha = ctx.alpha;

    switch (toast.kind) {
        case Kind::ReplaySaved: {

            const float alpha = std::clamp((toast.age - kIconDelaySaved) / 0.211f, 0.0f, 1.0f);
            ctx.alpha = saved_alpha * alpha;

            const ImVec2 center(card_pos.x + 20.0f + 27.5f, card_pos.y + kToastH * 0.5f + 20.0f * (1.0f - t));
            const float half = 27.5f * std::max(t, 0.0f);
            const float angle = 60.0f * kDegToRad * (1.0f - t);

            ImTextureID thumb =
                toast.thumb_key.empty() ? ImTextureID{} : textures.Find(toast.thumb_key);
            if (thumb && half > 0.5f) {
                const float c = std::cos(angle), s = std::sin(angle);
                auto rot = [&](float x, float y) {
                    return ImVec2(center.x + x * c - y * s, center.y + x * s + y * c);
                };

                ctx.dl->AddImageQuad(thumb, rot(-half, -half), rot(half, -half), rot(half, half),
                                     rot(-half, half), ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1),
                                     ImVec2(0, 1), ctx.Fade(IM_COL32_WHITE));
            } else {

                DrawIcon(ctx, IconFor(toast.kind), ImVec2(center.x - 27.5f, center.y - 27.5f),
                         55.0f, kText, std::max(t, 0.0f), angle);
            }
            break;
        }

        case Kind::ReplayArmed: {

            ctx.alpha = saved_alpha * std::clamp((toast.age - kIconDelay) / 0.3f, 0.0f, 1.0f);
            DrawIcon(ctx, IconFor(toast.kind), ImVec2(card_pos.x + 20.0f, card_pos.y + 13.5f),
                     60.0f, kText, 1.0f, -180.0f * kDegToRad * (1.0f - t));
            break;
        }

        case Kind::RecordingStarted: {

            ctx.alpha = saved_alpha * std::clamp((toast.age - kIconDelay) / 0.25f, 0.0f, 1.0f);
            const float scale = 3.0f - 2.0f * std::clamp(t, 0.0f, 1.0f);
            DrawIcon(ctx, IconFor(toast.kind), ImVec2(card_pos.x + 25.0f, card_pos.y + 18.5f),
                     50.0f, kDanger, scale);
            break;
        }

        case Kind::Processing: {
            ctx.alpha = saved_alpha * std::clamp((toast.age - kIconDelay) / 0.25f, 0.0f, 1.0f);
            constexpr float kRing = 50.0f;
            DrawProgressRing(ctx,
                             ImVec2(card_pos.x + 20.0f + kRing * 0.5f, card_pos.y + kToastH * 0.5f),
                             toast.progress);
            break;
        }

        case Kind::TrackLimit:
        case Kind::Download: {
            ctx.alpha = saved_alpha * std::clamp((toast.age - kIconDelay) / 0.3f, 0.0f, 1.0f);
            const float height = toast.kind == Kind::Download ? 54.0f : 50.0f;
            DrawIcon(ctx, toast.icon_name.empty() ? IconFor(toast.kind) : toast.icon_name.c_str(),
                     ImVec2(card_pos.x + 20.0f, card_pos.y + (kToastH - height) * 0.5f), height,
                     kText, std::clamp(t, 0.0f, 1.2f));
            break;
        }

        default: {

            ctx.alpha = saved_alpha * std::clamp((toast.age - kIconDelay) / 0.3f, 0.0f, 1.0f);
            DrawIcon(ctx, IconFor(toast.kind), ImVec2(card_pos.x + 20.0f, card_pos.y + 13.5f),
                     60.0f, kText, std::clamp(t, 0.0f, 1.2f));
            break;
        }
    }

    ctx.alpha = saved_alpha;
}

void Hud::DrawDownloadButton(UiContext& ctx, Toast& toast, ImVec2 card_pos) {
    const ImVec2 min(card_pos.x + kToastW - 20.0f - kDownloadButtonW,
                     card_pos.y + (kToastH - kDownloadButtonH) * 0.5f);
    const ImVec2 max(min.x + kDownloadButtonW, min.y + kDownloadButtonH);
    button_rects_.push_back(ImVec4(min.x, min.y, max.x, max.y));

    const Interaction it = Hit(ctx, HashId("update-download"), min, max);
    const float grow = 2.0f * it.hover - 2.0f * it.press;
    ctx.dl->AddRectFilled(ImVec2(min.x - grow, min.y - grow), ImVec2(max.x + grow, max.y + grow),
                          ctx.Fade(kAppAccent), 10.0f);
    const char* label = Tr("Скачать");
    const ImVec2 size = MeasureText(ctx, Font::Body, label);
    DrawText(ctx, Font::Body,
             ImVec2(min.x + (kDownloadButtonW - size.x) * 0.5f,
                    min.y + (kDownloadButtonH - size.y) * 0.5f),
             kText, label);

    if (it.clicked) {
        toast.offers_download = false;
        toast.age = std::max(toast.age, toast.lifetime);
        if (on_download_update) on_download_update();
    }
}

void Hud::SetChrome(bool indicator, bool stop_button) {
    show_indicator_ = indicator;
    show_stop_ = stop_button;
    if (!indicator) pill_.SetTarget(0.0f);
}

namespace {

constexpr float kBadgeMargin = 12.0f;
constexpr ImVec2 kPillMargin{52.0f, 50.0f};
constexpr float kSnapDistance = 14.0f;
constexpr double kPreviewSeconds = 42.0;

ImVec2 PlaceGroup(ImVec2 screen, ImVec2 size, std::uint32_t corner, ImVec2 at, ImVec2 margin) {
    if (corner == Hud::kCustomCorner)
        return {std::clamp(at.x * screen.x, 0.0f, std::max(0.0f, screen.x - size.x)),
                std::clamp(at.y * screen.y, 0.0f, std::max(0.0f, screen.y - size.y))};
    const bool right = corner == 1 || corner == 3;
    const bool bottom = corner >= 2;
    return {right ? screen.x - margin.x - size.x : margin.x,
            bottom ? screen.y - margin.y - size.y : margin.y};
}

float Snap(float value, std::initializer_list<float> anchors) {
    for (const float anchor : anchors)
        if (std::abs(value - anchor) < kSnapDistance) return anchor;
    return value;
}

void ScaleVertices(ImDrawList* dl, int from, ImVec2 pivot, float scale) {
    if (scale == 1.0f) return;
    for (int i = from; i < dl->VtxBuffer.Size; ++i) {
        ImVec2& p = dl->VtxBuffer[i].pos;
        p = ImVec2(pivot.x + (p.x - pivot.x) * scale, pivot.y + (p.y - pivot.y) * scale);
    }
}

}

ImVec2 Hud::DragGroup(UiContext& ctx, std::uint32_t id, ImVec2 pos, ImVec2 size, ImVec2 screen,
                      ImVec2 margin, bool& moved) {
    if (!editing_layout_) return pos;
    const Interaction it = Hit(ctx, id, pos, ImVec2(pos.x + size.x, pos.y + size.y), false);
    if (ctx.mouse_pressed && it.hovered) grab_ = ImVec2(ctx.mouse.x - pos.x, ctx.mouse.y - pos.y);
    ctx.dl->AddRect(ImVec2(pos.x - 5.0f, pos.y - 5.0f), ImVec2(pos.x + size.x + 5.0f, pos.y + size.y + 5.0f),
                    IM_COL32(0xa0, 0x99, 0xff, 110 + static_cast<int>(120 * it.hover)), 12.0f, 0, 2.0f);
    if (!ctx.mouse_down || ctx.active_id != id) return pos;

    ImVec2 next(ctx.mouse.x - grab_.x, ctx.mouse.y - grab_.y);
    next.x = Snap(next.x, {margin.x, screen.x - margin.x - size.x, (screen.x - size.x) * 0.5f});
    next.y = Snap(next.y, {margin.y, screen.y - margin.y - size.y, (screen.y - size.y) * 0.5f});
    next.x = std::clamp(next.x, 0.0f, std::max(0.0f, screen.x - size.x));
    next.y = std::clamp(next.y, 0.0f, std::max(0.0f, screen.y - size.y));
    moved = true;
    return next;
}

void Hud::DrawBadges(UiContext& ctx, ImVec2 screen, bool& moved) {
    const bool mic = badges_.mic || editing_layout_;
    const bool replay = badges_.replay || editing_layout_;
    if (!mic && !replay) return;

    constexpr float kBase = 24.0f, kGap = 5.0f;
    const float size = kBase * badges_.scale;
    const int count = (mic ? 1 : 0) + (replay ? 1 : 0);
    const ImVec2 group(size, count * size + (count - 1) * kGap);

    const ImVec2 placed = PlaceGroup(screen, group, badges_.corner, badges_.badges_at,
                                     ImVec2(kBadgeMargin, kBadgeMargin));
    const ImVec2 origin = DragGroup(ctx, HashId("hud-badges"), placed, group, screen,
                                    ImVec2(kBadgeMargin, kBadgeMargin), moved);
    if (origin.x != placed.x || origin.y != placed.y) {
        badges_.corner = kCustomCorner;
        badges_.badges_at = ImVec2(origin.x / screen.x, origin.y / screen.y);
    }

    const float x = origin.x;
    float y = origin.y;
    const float saved = ctx.alpha;
    ctx.alpha = saved * badges_.opacity;

    auto badge = [&](const char* icon, bool slashed) {
        const ImVec2 min(x, y);
        const ImVec2 max(x + size, y + size);
        hit_rects_.push_back(ImVec4(min.x, min.y, max.x, max.y));
        ctx.dl->AddRectFilled(min, max, ctx.Fade(kPanelBg), size * 0.3f);
        ctx.dl->AddRect(min, max, ctx.Fade(IM_COL32(255, 255, 255, 26)), size * 0.3f, 0, 1.0f);

        const float inset = size * 0.17f;
        DrawIcon(ctx, icon, ImVec2(min.x + inset, min.y + inset), size - 2 * inset,
                 slashed ? kDanger : kText);

        if (slashed) {
            const float pad = size * 0.22f;
            ctx.dl->AddLine(ImVec2(min.x + pad, max.y - pad), ImVec2(max.x - pad, min.y + pad),
                            ctx.Fade(kDanger), std::max(1.5f, size * 0.08f));
        }
        y += size + kGap;
    };

    if (mic) badge("microphone", badges_.mic_muted && !editing_layout_);
    if (replay) badge("repeat-circle", false);

    ctx.alpha = saved;
}

void Hud::Draw(UiContext& ctx, ImVec2 screen, const TextureCache& textures) {
    if (editing_layout_) pill_.SetTarget(1.0f);
    const float pill = pill_.Update(ctx.dt, spring::kPill);
    pill_rect_ = ImVec4(0, 0, 0, 0);
    hit_rects_.clear();
    button_rects_.clear();
    bool moved = false;
    DrawBadges(ctx, screen, moved);
    if (pill > 0.01f) {
        const double seconds = editing_layout_ && !recording_ ? kPreviewSeconds : recording_seconds_;
        const int total_seconds = static_cast<int>(seconds);
        const int hours = total_seconds / 3600;
        const std::string time =
            hours > 0 ? std::format("{:02}:{:02}:{:02}", hours, total_seconds / 60 % 60,
                                    total_seconds % 60)
                      : std::format("{:02}:{:02}", total_seconds / 60, total_seconds % 60);

        float digit_w = 0.0f;
        for (char digit = '0'; digit <= '9'; ++digit) {
            const char text[2] = {digit, 0};
            digit_w = std::max(digit_w, MeasureText(ctx, Font::Toast, text).x);
        }
        const float colon_w = MeasureText(ctx, Font::Toast, ":").x;
        const int digits = hours > 0 ? 6 : 4;
        const int colons = hours > 0 ? 2 : 1;

        const ImVec2 text_size(digits * digit_w + colons * colon_w,
                               MeasureText(ctx, Font::Toast, time.c_str()).y);

        constexpr float kPad = 10.0f, kGap = 10.0f, kDot = 14.0f, kStopBtn = 24.0f;

        constexpr float kChipPadX = 5.0f, kChipPadY = 2.0f, kChipRadius = 7.0f;
        const bool has_chip = !recording_app_.empty();
        const ImVec2 chip_text =
            has_chip ? MeasureText(ctx, Font::Toast, recording_app_.c_str()) : ImVec2(0, 0);
        const float chip_w = has_chip ? chip_text.x + 2 * kChipPadX : 0.0f;
        const float chip_h = has_chip ? chip_text.y + 2 * kChipPadY : 0.0f;

        const float w = kPad + kDot + kGap + (has_chip ? chip_w + kGap : 0.0f) + text_size.x +
                        (show_stop_ ? kGap + kStopBtn : 0.0f) + kPad;

        const float scale = badges_.scale;
        const ImVec2 scaled_size(w * scale, kPillH * scale);
        const ImVec2 placed = PlaceGroup(screen, scaled_size, badges_.record_corner, badges_.record_at, kPillMargin);
        const ImVec2 origin = DragGroup(ctx, HashId("hud-pill"), placed, scaled_size, screen, kPillMargin, moved);
        if (origin.x != placed.x || origin.y != placed.y) {
            badges_.record_corner = kCustomCorner;
            badges_.record_at = ImVec2(origin.x / screen.x, origin.y / screen.y);
        }

        const int first_vertex = ctx.dl->VtxBuffer.Size;
        const ImVec2 real_mouse = ctx.mouse;
        ctx.mouse = ImVec2(origin.x + (real_mouse.x - origin.x) / scale,
                           origin.y + (real_mouse.y - origin.y) / scale);
        const float outer_alpha = ctx.alpha;
        ctx.alpha = outer_alpha * badges_.opacity;

        const ImVec2 center(origin.x + w * 0.5f, origin.y + kPillH * 0.5f);
        const ImVec2 half(w * 0.5f * pill, kPillH * 0.5f * pill);
        const ImVec2 pmin(center.x - half.x, center.y - half.y);
        const ImVec2 pmax(center.x + half.x, center.y + half.y);
        ctx.dl->AddRectFilled(pmin, pmax, ctx.Fade(kPanelBg), kPillRadius * pill);
        ctx.dl->AddRect(pmin, pmax, ctx.Fade(IM_COL32(255, 255, 255, 26)), kPillRadius * pill, 0, 1.0f);

        if (pill > 0.5f) {
            const float alpha = std::clamp((pill - 0.5f) * 2.0f, 0.0f, 1.0f);
            const float saved = ctx.alpha;
            ctx.alpha = saved * alpha;

            float x = origin.x + kPad;
            const float pulse = 0.78f + 0.22f * std::sin(static_cast<float>(seconds) * 4.0f);
            ctx.dl->AddCircleFilled(ImVec2(x + kDot * 0.5f, center.y), kDot * 0.5f * pulse,
                                    ctx.Fade(kDanger), 20);
            x += kDot + kGap;

            if (has_chip) {
                const ImVec2 cmin(x, center.y - chip_h * 0.5f);
                const ImVec2 cmax(x + chip_w, center.y + chip_h * 0.5f);
                ctx.dl->AddRectFilled(cmin, cmax, ctx.Fade(kAppAccentBg), kChipRadius);
                DrawText(ctx, Font::Toast, ImVec2(x + kChipPadX, cmin.y + kChipPadY), kAppAccent,
                         recording_app_.c_str());
                x += chip_w + kGap;
            }

            float cell = x;
            for (char ch : time) {
                const char text[2] = {ch, 0};
                const float width = ch == ':' ? colon_w : digit_w;
                const float glyph = MeasureText(ctx, Font::Toast, text).x;
                DrawText(ctx, Font::Toast,
                         ImVec2(cell + (width - glyph) * 0.5f, center.y - text_size.y * 0.5f), kText,
                         text);
                cell += width;
            }
            x += text_size.x + kGap;

            if (show_stop_) {
                const ImVec2 bpos(x, center.y - kStopBtn * 0.5f);
                Interaction it = editing_layout_
                                     ? Interaction{}
                                     : Hit(ctx, HashId("pill-stop"), bpos,
                                           ImVec2(bpos.x + kStopBtn, bpos.y + kStopBtn));
                const float bs = 1.0f + 0.12f * it.hover - 0.12f * it.press;
                const ImVec2 bc(bpos.x + kStopBtn * 0.5f, bpos.y + kStopBtn * 0.5f);
                ctx.dl->AddCircleFilled(
                    bc, kStopBtn * 0.5f * bs,
                    ctx.Fade(IM_COL32(217, 217, 217, 51 + static_cast<int>(30 * it.hover))), 32);
                const float sq = 4.5f * bs;
                ctx.dl->AddRectFilled(ImVec2(bc.x - sq, bc.y - sq), ImVec2(bc.x + sq, bc.y + sq),
                                      ctx.Fade(kText), 2.0f);
                if (it.clicked && on_stop) on_stop();
            }

            ctx.alpha = saved;
        }

        ctx.alpha = outer_alpha;
        ctx.mouse = real_mouse;
        ScaleVertices(ctx.dl, first_vertex, origin, scale);

        pill_rect_ = ImVec4(origin.x + (pmin.x - origin.x) * scale, origin.y + (pmin.y - origin.y) * scale,
                            origin.x + (pmax.x - origin.x) * scale, origin.y + (pmax.y - origin.y) * scale);
        hit_rects_.push_back(pill_rect_);
    }
    if (moved && on_layout_moved) on_layout_moved(badges_);

    float y = kToastMargin;
    for (auto& toast : toasts_) {
        toast.age += ctx.dt;
        if (toast.age > toast.lifetime && !toast.leaving && !toast.sticky) {
            toast.leaving = true;
            toast.slide.SetTarget(700.0f);
        }

        const float x = toast.slide.Update(ctx.dt, toast.leaving ? spring::kExit : spring::kQuick);
        toast.icon.SetTarget(toast.age > IconDelayFor(toast.kind) ? 1.0f : 0.0f);
        toast.icon.Update(ctx.dt, IconSpringFor(toast.kind));

        const float total_w = kToastBadge + 18.5f + kToastW;
        const float left = screen.x - kToastMargin - total_w + x;
        hit_rects_.push_back(ImVec4(left, y, left + total_w, y + kToastBadge));

        const bool hovered = ctx.interactive && ctx.mouse.x >= left && ctx.mouse.x <= left + total_w &&
                             ctx.mouse.y >= y && ctx.mouse.y <= y + kToastBadge;
        if (hovered && toast.offers_download && !toast.leaving)
            toast.age = std::min(toast.age, toast.lifetime - 0.5f);

        ctx.dl->AddRectFilled(ImVec2(left, y), ImVec2(left + kToastBadge, y + kToastBadge),
                              kPanelBg, kToastRadius);
        ctx.dl->AddRect(ImVec2(left, y), ImVec2(left + kToastBadge, y + kToastBadge),
                        IM_COL32(255, 255, 255, 13), kToastRadius, 0, 1.0f);

        const ImVec2 saved_offset = ctx.offset;
        ctx.offset = ImVec2(0, 0);
        DrawIcon(ctx, "logo-mark", ImVec2(left + 20.0f, y + 21.0f), 46.0f, kText);

        const float notch_x = left + kToastBadge;
        DrawIcon(ctx, "logo-notch", ImVec2(notch_x, y + (kToastBadge - 43.27f) * 0.5f), 43.27f,
                 kNotchTint);

        const float card_x = notch_x + 18.5f;
        const float card_y = y + (kToastBadge - kToastH) * 0.5f;
        ctx.dl->AddRectFilled(ImVec2(card_x, card_y), ImVec2(card_x + kToastW, card_y + kToastH),
                              kPanelBg, kToastRadius);
        ctx.dl->AddRect(ImVec2(card_x, card_y), ImVec2(card_x + kToastW, card_y + kToastH),
                        IM_COL32(255, 255, 255, 26), kToastRadius, 0, 1.0f);

        DrawToastIcon(ctx, toast, ImVec2(card_x, card_y), textures);

        const float saved_alpha = ctx.alpha;
        ctx.alpha = std::clamp((toast.age - kTextDelay) / kTextFade, 0.0f, 1.0f);
        const float label_x = card_x + (HasWideIcon(toast.kind) ? 108.0f : 100.0f);
        const float button_w = toast.offers_download ? kDownloadButtonW + 12.0f : 0.0f;
        const float label_w = card_x + kToastW - 22.0f - label_x - button_w;
        const float line_h = MeasureText(ctx, Font::Toast, "M").y;
        const ImU32 accent =
            toast.kind == Kind::Download && toast.icon_name == "nvidia" ? kNvidiaGreen : kAppAccent;
        const auto words = SplitAccented(ctx, toast.text, toast.app);

        if (toast.subtitle.empty()) {
            DrawAccentedText(ctx, words, label_x, card_y + kToastH * 0.5f, label_w, line_h, true,
                             accent);
        } else {
            const float title_h =
                DrawAccentedText(ctx, words, label_x, 0.0f, label_w, line_h, false);
            const float sub_h = MeasureText(ctx, Font::Tiny, "M").y;
            const float bar_h = toast.progress >= 0.0f ? kProgressH + kDetailGap : 0.0f;
            const float block_h = title_h + kDetailGap + bar_h + sub_h;
            float top = card_y + (kToastH - block_h) * 0.5f;

            DrawAccentedText(ctx, words, label_x, top + title_h * 0.5f, label_w, line_h, true,
                             accent);
            top += title_h + kDetailGap;
            if (toast.progress >= 0.0f) {
                ctx.dl->AddRectFilled(ImVec2(label_x, top), ImVec2(label_x + label_w, top + kProgressH),
                                      ctx.Fade(kProgressTrack), 9.0f);
                if (toast.progress > 0.0f)
                    ctx.dl->AddRectFilled(ImVec2(label_x, top),
                                          ImVec2(label_x + label_w * toast.progress, top + kProgressH),
                                          ctx.Fade(kAppAccent), 9.0f);
                top += kProgressH + kDetailGap;
            }
            DrawText(ctx, Font::Tiny, ImVec2(label_x, top), kTextMuted, toast.subtitle.c_str());
        }

        if (toast.offers_download) DrawDownloadButton(ctx, toast, ImVec2(card_x, card_y));

        ctx.alpha = saved_alpha;
        ctx.offset = saved_offset;

        y += kToastBadge + kToastGapY;
    }

    while (!toasts_.empty() && toasts_.front().leaving && toasts_.front().slide.value() > 690.0f)
        toasts_.pop_front();
}

}
