#include "rf/ui/Widgets.h"

#include <algorithm>
#include <cmath>

namespace rf::ui {
namespace {

bool Inside(ImVec2 p, ImVec2 min, ImVec2 max) {
    return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y;
}

ImU32 WithAlpha(ImU32 col, float alpha) {
    const float a = static_cast<float>((col >> IM_COL32_A_SHIFT) & 0xFF) * alpha;
    return (col & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
}

ImU32 LerpColor(ImU32 a, ImU32 b, float t) {
    const ImVec4 ca = ImGui::ColorConvertU32ToFloat4(a);
    const ImVec4 cb = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(Lerp(ca.x, cb.x, t), Lerp(ca.y, cb.y, t),
                                                 Lerp(ca.z, cb.z, t), Lerp(ca.w, cb.w, t)));
}

}

ImU32 UiContext::Fade(ImU32 col) const { return WithAlpha(col, alpha); }

bool UiContext::Reserved(ImVec2 point) const {
    for (const ImVec4& r : reserved)
        if (point.x >= r.x && point.x <= r.z && point.y >= r.y && point.y <= r.w) return true;
    return false;
}

ImVec2 MeasureTextWrapped(UiContext& ctx, Font font, float wrap_width, const char* text) {
    ImFont* f = ctx.fonts->Get(font);
    if (!f) return ImVec2(0, 0);
    return f->CalcTextSizeA(f->FontSize, FLT_MAX, wrap_width, text);
}

void DrawTextWrapped(UiContext& ctx, Font font, ImVec2 pos, float wrap_width, ImU32 col,
                     const char* text) {
    ImFont* f = ctx.fonts->Get(font);
    if (!f) return;
    ctx.dl->AddText(f, f->FontSize, ctx.At(pos), ctx.Fade(col), text, nullptr, wrap_width);
}

float WarningRow(UiContext& ctx, ImVec2 pos, float width, ImU32 accent, ImU32 background,
                 const char* text) {
    constexpr float kPad = 10.0f, kGap = 10.0f, kIcon = 24.0f;

    const float text_w = width - kPad - kIcon - kGap - kPad;
    const ImVec2 text_size = MeasureTextWrapped(ctx, Font::Warning, text_w, text);
    const float height = std::max(kIcon, text_size.y) + 2 * kPad;

    const ImVec2 min = ctx.At(pos);
    const ImVec2 max = ImVec2(min.x + width, min.y + height);
    ctx.dl->AddRectFilled(min, max, ctx.Fade(background), theme::kRowRadius);

    DrawIcon(ctx, "danger", ImVec2(pos.x + kPad, pos.y + kPad), kIcon, accent);
    DrawTextWrapped(ctx, Font::Warning, ImVec2(pos.x + kPad + kIcon + kGap, pos.y + kPad), text_w,
                    accent, text);
    return height;
}

void DrawText(UiContext& ctx, Font font, ImVec2 pos, ImU32 col, const char* text) {
    ImFont* f = ctx.fonts->Get(font);
    if (!f) return;
    ctx.dl->AddText(f, f->FontSize, ctx.At(pos), ctx.Fade(col), text);
}

ImVec2 MeasureText(UiContext& ctx, Font font, const char* text) {
    ImFont* f = ctx.fonts->Get(font);
    if (!f) return ImVec2(0, 0);
    return f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, text);
}

void DrawTextCentered(UiContext& ctx, Font font, ImVec2 center, ImU32 col, const char* text) {
    const ImVec2 size = MeasureText(ctx, font, text);
    DrawText(ctx, font, ImVec2(center.x - size.x * 0.5f, center.y - size.y * 0.5f), col, text);
}

void DrawTextRight(UiContext& ctx, Font font, ImVec2 right_center, ImU32 col, const char* text) {
    const ImVec2 size = MeasureText(ctx, font, text);
    DrawText(ctx, font, ImVec2(right_center.x - size.x, right_center.y - size.y * 0.5f), col, text);
}

void DrawIcon(UiContext& ctx, const char* name, ImVec2 top_left, float size, ImU32 tint,
              float scale, float rotation) {
    ImTextureID tex = ctx.icons->Get(name);
    if (!tex) return;

    const ImVec2 native = ctx.icons->Size(name);
    const float aspect = (native.x > 0 && native.y > 0) ? native.x / native.y : 1.0f;

    ImVec2 half(size * aspect * 0.5f * scale, size * 0.5f * scale);
    const ImVec2 center = ctx.At(ImVec2(top_left.x + size * aspect * 0.5f, top_left.y + size * 0.5f));
    const ImU32 col = ctx.Fade(tint);

    if (rotation == 0.0f) {
        ctx.dl->AddImage(tex, ImVec2(center.x - half.x, center.y - half.y),
                         ImVec2(center.x + half.x, center.y + half.y), ImVec2(0, 0), ImVec2(1, 1),
                         col);
        return;
    }

    const float c = std::cos(rotation), s = std::sin(rotation);
    auto rot = [&](float x, float y) {
        return ImVec2(center.x + x * c - y * s, center.y + x * s + y * c);
    };
    ctx.dl->AddImageQuad(tex, rot(-half.x, -half.y), rot(half.x, -half.y), rot(half.x, half.y),
                         rot(-half.x, half.y), ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1),
                         ImVec2(0, 1), col);
}

void DrawPanel(UiContext& ctx, ImVec2 min, ImVec2 max, float radius) {
    const ImVec2 a = ctx.At(min);
    const ImVec2 b = ctx.At(max);
    ctx.dl->AddRectFilled(a, b, ctx.Fade(theme::kPanelBg), radius);
    ctx.dl->AddRect(a, b, ctx.Fade(theme::kPanelBorder), radius, 0, 1.0f);
}

void DrawBadge(UiContext& ctx, ImVec2 pos, const char* text) {
    const ImVec2 size = MeasureText(ctx, Font::BadgeItalic, text);
    const ImVec2 a = ctx.At(pos);
    const ImVec2 b = ImVec2(a.x + size.x + 6.0f, a.y + size.y + 4.0f);
    ctx.dl->AddRectFilled(a, b, ctx.Fade(theme::kBadgeBg), 5.0f);
    DrawText(ctx, Font::BadgeItalic, ImVec2(pos.x + 3.0f, pos.y + 2.0f), theme::kText, text);
}

void DrawThumbBadge(UiContext& ctx, ImVec2 pos, const char* text) {
    const ImVec2 size = MeasureText(ctx, Font::Badge, text);
    const ImVec2 a = ctx.At(pos);
    const ImVec2 b = ImVec2(a.x + size.x + 10.0f, a.y + size.y + 4.0f);

    ctx.dl->AddRectFilled(a, b, ctx.Fade(IM_COL32(0, 0, 0, 110)), 18.0f);
    ctx.dl->AddRectFilled(a, b, ctx.Fade(IM_COL32(255, 255, 255, 10)), 18.0f);
    DrawText(ctx, Font::Badge, ImVec2(pos.x + 5.0f, pos.y + 2.0f), theme::kText, text);
}

Interaction Hit(UiContext& ctx, std::uint32_t id, ImVec2 min, ImVec2 max, bool honour_reserved) {
    Interaction out;
    const ImVec2 a = ctx.At(min);
    const ImVec2 b = ctx.At(max);

    out.hovered = ctx.interactive && Inside(ctx.mouse, a, b) &&
                  !(honour_reserved && ctx.Reserved(ctx.mouse));
    out.held = out.hovered && ctx.mouse_down;
    if (out.hovered && ctx.mouse_pressed) ctx.active_id = id;
    out.clicked = out.hovered && ctx.mouse_released && ctx.active_id == id;
    if (out.hovered) ctx.wants_pointer = true;

    Spring& hover = ctx.anim->Get(id);
    hover.SetTarget(out.hovered ? 1.0f : 0.0f);
    out.hover = hover.Update(ctx.dt, spring::kMenu);

    Spring& press = ctx.anim->Get(id ^ 0x9E3779B9u);
    press.SetTarget(out.held ? 1.0f : 0.0f);
    out.press = press.Update(ctx.dt, spring::kBouncy);

    return out;
}

Interaction Row(UiContext& ctx, std::uint32_t id, ImVec2 pos, ImVec2 size, bool clickable,
                bool honour_reserved) {
    Interaction it =
        Hit(ctx, id, pos, ImVec2(pos.x + size.x, pos.y + size.y), honour_reserved);
    if (!clickable) {

        it.clicked = false;
    }

    const float scale = 1.0f - 0.006f * it.press;
    const ImVec2 center(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f);
    const ImVec2 half(size.x * 0.5f * scale, size.y * 0.5f * scale);

    const ImU32 bg = LerpColor(LerpColor(theme::kRowBg, theme::kRowBgHover, it.hover),
                               theme::kRowBgActive, it.press);

    ctx.dl->AddRectFilled(ctx.At(ImVec2(center.x - half.x, center.y - half.y)),
                          ctx.At(ImVec2(center.x + half.x, center.y + half.y)), ctx.Fade(bg),
                          theme::kRowRadius);
    return it;
}

void RowIcon(UiContext& ctx, const Interaction& row, ImVec2 row_pos, const char* icon) {

    const float scale = 1.0f + 0.10f * row.hover - 0.06f * row.press;
    DrawIcon(ctx, icon, ImVec2(row_pos.x + theme::kRowIconX, row_pos.y + theme::kRowIconY),
             theme::kRowIcon, theme::kText, scale);
}

void RowTitle(UiContext& ctx, ImVec2 row_pos, const char* text) {
    DrawText(ctx, Font::Body, ImVec2(row_pos.x + theme::kRowTextX, row_pos.y + theme::kRowTitleY),
             theme::kText, text);
}

void RowSubtitle(UiContext& ctx, ImVec2 row_pos, const char* text, float y) {

    constexpr float kControlMargin = 105.0f;
    const float room = theme::kPanelW - 2 * theme::kPanelPad - theme::kRowTextX - kControlMargin;

    ImFont* font = ctx.fonts->Get(Font::Small);
    const ImVec2 pos(row_pos.x + theme::kRowTextX, row_pos.y + y);
    if (!font || font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, text).x <= room) {
        DrawText(ctx, Font::Small, pos, theme::kTextMuted, text);
        return;
    }

    ctx.dl->PushClipRect(ctx.At(ImVec2(pos.x, pos.y - 4.0f)),
                         ctx.At(ImVec2(pos.x + room, pos.y + font->FontSize + 4.0f)), true);
    DrawText(ctx, Font::Small, pos, theme::kTextMuted, text);
    ctx.dl->PopClipRect();
}

bool Toggle(UiContext& ctx, std::uint32_t id, ImVec2 pos, bool& value) {
    const ImVec2 size(theme::kToggleW, theme::kToggleH);
    Interaction it = Hit(ctx, id, pos, ImVec2(pos.x + size.x, pos.y + size.y));

    bool changed = false;
    if (it.clicked) {
        value = !value;
        changed = true;
    }

    Spring& knob = ctx.anim->Get(id ^ 0x5BF03635u, value ? 1.0f : 0.0f);
    knob.SetTarget(value ? 1.0f : 0.0f);
    const float t = knob.Update(ctx.dt, spring::kBouncy);

    const float radius = size.y * 0.5f;
    const ImU32 track = LerpColor(theme::kTrack, theme::kAccent, std::clamp(t, 0.0f, 1.0f));
    ctx.dl->AddRectFilled(ctx.At(pos), ctx.At(ImVec2(pos.x + size.x, pos.y + size.y)),
                          ctx.Fade(track), radius);

    const float knob_r = radius - 5.0f + 1.5f * it.press;
    const float travel = size.x - 2.0f * radius;
    const ImVec2 center(pos.x + radius + travel * t, pos.y + radius);
    ctx.dl->AddCircleFilled(ctx.At(center), knob_r, ctx.Fade(IM_COL32(255, 255, 255, 255)), 32);

    return changed;
}

bool SliderBar(UiContext& ctx, std::uint32_t id, ImVec2 pos, float width, float height,
               float& value, float min, float max) {
    const float track_y = pos.y + (height - theme::kTrackH) * 0.5f;
    const ImVec2 track_pos(pos.x, track_y);

    Interaction it = Hit(ctx, id, ImVec2(pos.x - 9.0f, pos.y), ImVec2(pos.x + width + 9.0f, pos.y + height));

    Spring& drag = ctx.anim->Get(id ^ 0x27220A95u);
    const bool dragging = it.held || (drag.target() > 0.5f && ctx.mouse_down);
    drag.SetTarget(dragging ? 1.0f : 0.0f);
    const float drag_t = drag.Update(ctx.dt, spring::kQuick);

    bool changed = false;
    if (dragging && width > 0.0f) {
        const float local = (ctx.mouse.x - ctx.At(track_pos).x) / width;
        const float next = min + std::clamp(local, 0.0f, 1.0f) * (max - min);
        if (next != value) {
            value = next;
            changed = true;
        }
    }

    const float t = (max > min) ? std::clamp((value - min) / (max - min), 0.0f, 1.0f) : 0.0f;

    ctx.dl->AddRectFilled(ctx.At(track_pos),
                          ctx.At(ImVec2(track_pos.x + width, track_pos.y + theme::kTrackH)),
                          ctx.Fade(theme::kTrack), theme::kTrackH * 0.5f);
    ctx.dl->AddRectFilled(ctx.At(track_pos),
                          ctx.At(ImVec2(track_pos.x + width * t, track_pos.y + theme::kTrackH)),
                          ctx.Fade(theme::kAccent), theme::kTrackH * 0.5f);

    const float thumb_r = theme::kThumbSize * 0.5f * (1.0f + 0.15f * it.hover + 0.20f * drag_t);
    const ImVec2 center(track_pos.x + width * t, track_pos.y + theme::kTrackH * 0.5f);
    ctx.dl->AddCircleFilled(ctx.At(center), thumb_r + 1.0f, ctx.Fade(IM_COL32(0, 0, 0, 60)), 24);
    ctx.dl->AddCircleFilled(ctx.At(center), thumb_r, ctx.Fade(theme::kAccent), 24);
    return changed;
}

bool Slider(UiContext& ctx, std::uint32_t id, ImVec2 row_pos, float row_w, float& value, float min,
            float max, const char* value_text) {
    const ImVec2 track_pos(row_pos.x + theme::kRowIconX, row_pos.y + theme::kTrackY);
    const float track_w = theme::kTrackW;

    const ImVec2 hit_min(track_pos.x - 9.0f, track_pos.y - 12.0f);
    const ImVec2 hit_max(track_pos.x + track_w + 9.0f, track_pos.y + theme::kTrackH + 12.0f);
    Interaction it = Hit(ctx, id, hit_min, hit_max);

    Spring& drag = ctx.anim->Get(id ^ 0x27220A95u);
    const bool dragging = it.held || (drag.target() > 0.5f && ctx.mouse_down);
    drag.SetTarget(dragging ? 1.0f : 0.0f);
    const float drag_t = drag.Update(ctx.dt, spring::kQuick);

    bool changed = false;
    if (dragging) {
        const float local = (ctx.mouse.x - ctx.At(track_pos).x) / track_w;
        const float next = min + std::clamp(local, 0.0f, 1.0f) * (max - min);
        if (next != value) {
            value = next;
            changed = true;
        }
    }

    const float t = (max > min) ? std::clamp((value - min) / (max - min), 0.0f, 1.0f) : 0.0f;

    ctx.dl->AddRectFilled(ctx.At(track_pos),
                          ctx.At(ImVec2(track_pos.x + track_w, track_pos.y + theme::kTrackH)),
                          ctx.Fade(theme::kTrack), theme::kTrackH * 0.5f);
    ctx.dl->AddRectFilled(ctx.At(track_pos),
                          ctx.At(ImVec2(track_pos.x + track_w * t, track_pos.y + theme::kTrackH)),
                          ctx.Fade(theme::kAccent), theme::kTrackH * 0.5f);

    const float thumb_r = theme::kThumbSize * 0.5f * (1.0f + 0.15f * it.hover + 0.20f * drag_t);
    const ImVec2 center(track_pos.x + track_w * t, track_pos.y + theme::kTrackH * 0.5f);
    ctx.dl->AddCircleFilled(ctx.At(center), thumb_r + 1.0f, ctx.Fade(IM_COL32(0, 0, 0, 60)), 24);
    ctx.dl->AddCircleFilled(ctx.At(center), thumb_r, ctx.Fade(theme::kAccent), 24);

    DrawTextRight(ctx, Font::Small, ImVec2(row_pos.x + row_w - 15.0f, track_pos.y + 3.0f),
                  theme::kTextMuted, value_text);
    return changed;
}

bool Stepper(UiContext& ctx, std::uint32_t id, ImVec2 row_pos, float row_w, float y_center,
             int& index, int count, const char* value_text) {
    bool changed = false;

    const ImVec2 value_size = MeasureText(ctx, Font::Small, value_text);
    constexpr float kEdgeInset = 26.0f;
    constexpr float kArrowGap = 20.0f;
    const float right_x = row_pos.x + row_w - kEdgeInset;
    const float value_cx = right_x - kArrowGap - value_size.x * 0.5f;
    const float left_x = value_cx - value_size.x * 0.5f - kArrowGap;

    auto arrow = [&](float x, bool forward, std::uint32_t salt) {
        const ImVec2 pos(x - 14.0f, y_center - 14.0f);
        Interaction it = Hit(ctx, id ^ salt, pos, ImVec2(pos.x + 28.0f, pos.y + 28.0f));
        const float glow = 0.5f + 0.5f * it.hover;
        const float nudge = (forward ? 1.0f : -1.0f) * (2.0f * it.hover + 2.0f * it.press);

        DrawTextCentered(ctx, Font::Body, ImVec2(x + nudge, y_center),
                         WithAlpha(theme::kText, glow), forward ? "›" : "‹");

        if (it.clicked) {
            index = (index + (forward ? 1 : count - 1)) % count;
            changed = true;
        }
    };

    arrow(left_x, false, 0x1111u);
    arrow(right_x, true, 0x2222u);

    Spring& shift = ctx.anim->Get(id ^ 0x3333u);
    if (changed) shift.Reset(index == 0 ? 0.0f : 8.0f);
    shift.SetTarget(0.0f);
    const float dx = shift.Update(ctx.dt, spring::kQuick);

    DrawTextCentered(ctx, Font::Small, ImVec2(value_cx + dx, y_center), theme::kText, value_text);
    return changed;
}

bool CircleButton(UiContext& ctx, std::uint32_t id, ImVec2 pos, float size, const char* icon,
                  bool flip_horizontal, bool honour_reserved) {
    Interaction it = Hit(ctx, id, pos, ImVec2(pos.x + size, pos.y + size), honour_reserved);
    const float scale = 1.0f + 0.08f * it.hover - 0.10f * it.press;
    DrawIcon(ctx, icon, pos, size, theme::kText, scale,
             flip_horizontal ? 3.14159265f : 0.0f);
    return it.clicked;
}

bool Chevron(UiContext& ctx, std::uint32_t id, ImVec2 pos, float size) {
    Interaction it = Hit(ctx, id, pos, ImVec2(pos.x + size, pos.y + size));
    const float nudge = 3.0f * it.hover + 2.0f * it.press;
    DrawTextCentered(ctx, Font::Body, ImVec2(pos.x + size * 0.5f + nudge, pos.y + size * 0.5f),
                     WithAlpha(theme::kText, 0.5f + 0.5f * it.hover), "›");
    return it.clicked;
}

void ScrollArea::Begin(UiContext& ctx, std::uint32_t id, ImVec2 min, ImVec2 max,
                       float content_height) {
    id_ = id;
    min_ = min;
    max_ = max;

    const float view_h = max.y - min.y;
    const float max_scroll = std::max(0.0f, content_height - view_h);

    Spring& scroll = ctx.anim->Get(id);
    if (ctx.interactive && ctx.wheel != 0.0f && Inside(ctx.mouse, ctx.At(min), ctx.At(max)))
        scroll.SetTarget(std::clamp(scroll.target() - ctx.wheel * 60.0f, 0.0f, max_scroll));
    else
        scroll.SetTarget(std::clamp(scroll.target(), 0.0f, max_scroll));

    offset_ = scroll.Update(ctx.dt, spring::kExit);

    ctx.dl->PushClipRect(ctx.At(min), ctx.At(max), true);
    ctx.offset.y -= offset_;
}

void ScrollArea::End(UiContext& ctx) {
    ctx.offset.y += offset_;
    ctx.dl->PopClipRect();
}

}
