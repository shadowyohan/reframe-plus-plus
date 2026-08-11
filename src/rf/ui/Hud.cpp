#include "rf/ui/Hud.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace rf::ui {
namespace {

using namespace theme;

constexpr float kToastLifetime = 3.5f;

constexpr float kIconDelaySaved = 0.18f;
constexpr float kIconDelay = 0.13f;
constexpr float kTextDelay = 0.20f;
constexpr float kTextFade = 0.161f;
constexpr int kMaxToasts = 3;

constexpr float kDegToRad = 3.14159265f / 180.0f;

struct Word {
    std::string text;
    bool accent = false;
    float width = 0.0f;
};

std::vector<Word> SplitAccented(UiContext& ctx, const std::string& text, const std::string& app) {
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
                       float max_width, float line_height, bool draw) {
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
        DrawText(ctx, Font::Toast, ImVec2(pen_x, pen_y), w.accent ? kAppAccent : kText,
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
    toast.slide.Reset(600.0f);
    toast.slide.SetTarget(0.0f);
    toast.icon.Reset(0.0f);
    toasts_.push_back(std::move(toast));

    while (toasts_.size() > kMaxToasts) toasts_.pop_front();
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

        default: {

            ctx.alpha = saved_alpha * std::clamp((toast.age - kIconDelay) / 0.3f, 0.0f, 1.0f);
            DrawIcon(ctx, IconFor(toast.kind), ImVec2(card_pos.x + 20.0f, card_pos.y + 13.5f),
                     60.0f, kText, std::clamp(t, 0.0f, 1.2f));
            break;
        }
    }

    ctx.alpha = saved_alpha;
}

void Hud::Draw(UiContext& ctx, ImVec2 screen, const TextureCache& textures) {

    const float pill = pill_.Update(ctx.dt, spring::kPill);
    pill_rect_ = ImVec4(0, 0, 0, 0);
    hit_rects_.clear();
    if (pill > 0.01f) {
        const std::string time = std::format("{:02}:{:02}", static_cast<int>(recording_seconds_) / 60,
                                             static_cast<int>(recording_seconds_) % 60);
        const ImVec2 text_size = MeasureText(ctx, Font::Toast, time.c_str());

        constexpr float kPad = 10.0f, kGap = 10.0f, kDot = 14.0f, kStopBtn = 24.0f;

        constexpr float kChipPadX = 5.0f, kChipPadY = 2.0f, kChipRadius = 7.0f;
        const bool has_chip = !recording_app_.empty();
        const ImVec2 chip_text =
            has_chip ? MeasureText(ctx, Font::Toast, recording_app_.c_str()) : ImVec2(0, 0);
        const float chip_w = has_chip ? chip_text.x + 2 * kChipPadX : 0.0f;
        const float chip_h = has_chip ? chip_text.y + 2 * kChipPadY : 0.0f;

        const float w = kPad + kDot + kGap + (has_chip ? chip_w + kGap : 0.0f) + text_size.x +
                        kGap + kStopBtn + kPad;

        const ImVec2 center(52.0f + w * 0.5f, 50.0f + kPillH * 0.5f);
        const ImVec2 half(w * 0.5f * pill, kPillH * 0.5f * pill);
        const ImVec2 pmin(center.x - half.x, center.y - half.y);
        const ImVec2 pmax(center.x + half.x, center.y + half.y);
        ctx.dl->AddRectFilled(pmin, pmax, kPanelBg, kPillRadius * pill);
        ctx.dl->AddRect(pmin, pmax, IM_COL32(255, 255, 255, 26), kPillRadius * pill, 0, 1.0f);

        if (pill > 0.5f) {
            const float alpha = std::clamp((pill - 0.5f) * 2.0f, 0.0f, 1.0f);
            const float saved = ctx.alpha;
            ctx.alpha = alpha;

            float x = 52.0f + kPad;
            const float pulse = 0.78f + 0.22f * std::sin(static_cast<float>(recording_seconds_) * 4.0f);
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

            DrawText(ctx, Font::Toast, ImVec2(x, center.y - text_size.y * 0.5f), kText,
                     time.c_str());
            x += text_size.x + kGap;

            const ImVec2 bpos(x, center.y - kStopBtn * 0.5f);
            Interaction it = Hit(ctx, HashId("pill-stop"), bpos,
                                 ImVec2(bpos.x + kStopBtn, bpos.y + kStopBtn));
            const float bs = 1.0f + 0.12f * it.hover - 0.12f * it.press;
            const ImVec2 bc(bpos.x + kStopBtn * 0.5f, bpos.y + kStopBtn * 0.5f);
            ctx.dl->AddCircleFilled(bc, kStopBtn * 0.5f * bs,
                                    ctx.Fade(IM_COL32(217, 217, 217, 51 + static_cast<int>(30 * it.hover))),
                                    32);
            const float sq = 4.5f * bs;
            ctx.dl->AddRectFilled(ImVec2(bc.x - sq, bc.y - sq), ImVec2(bc.x + sq, bc.y + sq),
                                  ctx.Fade(kText), 2.0f);
            if (it.clicked && on_stop) on_stop();

            ctx.alpha = saved;
        }

        pill_rect_ = ImVec4(pmin.x, pmin.y, pmax.x, pmax.y);
        hit_rects_.push_back(pill_rect_);
    }

    float y = kToastMargin;
    for (auto& toast : toasts_) {
        toast.age += ctx.dt;
        if (toast.age > kToastLifetime && !toast.leaving) {
            toast.leaving = true;
            toast.slide.SetTarget(700.0f);
        }

        const float x = toast.slide.Update(ctx.dt, toast.leaving ? spring::kExit : spring::kQuick);
        toast.icon.SetTarget(toast.age > IconDelayFor(toast.kind) ? 1.0f : 0.0f);
        toast.icon.Update(ctx.dt, IconSpringFor(toast.kind));

        const float total_w = kToastBadge + 18.5f + kToastW;
        const float left = screen.x - kToastMargin - total_w + x;
        hit_rects_.push_back(ImVec4(left, y, left + total_w, y + kToastBadge));

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
        const float label_x = card_x + 100.0f;
        const float label_w = kToastW - 100.0f - 20.0f;
        const float line_h = MeasureText(ctx, Font::Toast, "M").y;
        DrawAccentedText(ctx, SplitAccented(ctx, toast.text, toast.app), label_x,
                         card_y + kToastH * 0.5f, label_w, line_h, true);

        ctx.alpha = saved_alpha;
        ctx.offset = saved_offset;

        y += kToastBadge + kToastGapY;
    }

    while (!toasts_.empty() && toasts_.front().leaving && toasts_.front().slide.value() > 690.0f)
        toasts_.pop_front();
}

}
