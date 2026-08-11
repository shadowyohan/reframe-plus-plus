#pragma once
#include <imgui.h>

#include <string>

#include "rf/ui/Assets.h"
#include "rf/ui/Spring.h"
#include "rf/ui/Theme.h"

namespace rf::ui {

struct UiContext {
    ImDrawList* dl = nullptr;
    AnimStore* anim = nullptr;
    const IconSet* icons = nullptr;
    const FontSet* fonts = nullptr;

    float dt = 1.0f / 60.0f;
    float dpi = 1.0f;

    ImVec2 mouse{};
    bool mouse_down = false;
    bool mouse_pressed = false;
    bool mouse_released = false;
    float wheel = 0.0f;

    bool interactive = true;

    ImVec2 offset{};
    float alpha = 1.0f;

    bool wants_pointer = false;

    std::vector<ImVec4> reserved;

    void Reserve(ImVec2 min, ImVec2 max) { reserved.push_back(ImVec4(min.x, min.y, max.x, max.y)); }
    [[nodiscard]] bool Reserved(ImVec2 point) const;

    [[nodiscard]] ImU32 Fade(ImU32 col) const;
    [[nodiscard]] ImVec2 At(ImVec2 p) const { return ImVec2(p.x + offset.x, p.y + offset.y); }
};

struct Interaction {
    bool hovered = false;
    bool held = false;
    bool clicked = false;
    float hover = 0.0f;
    float press = 0.0f;
};

void DrawText(UiContext& ctx, Font font, ImVec2 pos, ImU32 col, const char* text);
void DrawTextCentered(UiContext& ctx, Font font, ImVec2 center, ImU32 col, const char* text);
void DrawTextRight(UiContext& ctx, Font font, ImVec2 right_center, ImU32 col, const char* text);
ImVec2 MeasureText(UiContext& ctx, Font font, const char* text);

void DrawIcon(UiContext& ctx, const char* name, ImVec2 top_left, float size, ImU32 tint,
              float scale = 1.0f, float rotation = 0.0f);

void DrawPanel(UiContext& ctx, ImVec2 min, ImVec2 max, float radius);

void DrawBadge(UiContext& ctx, ImVec2 pos, const char* text);

void DrawThumbBadge(UiContext& ctx, ImVec2 pos, const char* text);

Interaction Hit(UiContext& ctx, std::uint32_t id, ImVec2 min, ImVec2 max,
                bool honour_reserved = true);

Interaction Row(UiContext& ctx, std::uint32_t id, ImVec2 pos, ImVec2 size, bool clickable = false,
                bool honour_reserved = true);

void RowIcon(UiContext& ctx, const Interaction& row, ImVec2 row_pos, const char* icon);
void RowTitle(UiContext& ctx, ImVec2 row_pos, const char* text);
void RowSubtitle(UiContext& ctx, ImVec2 row_pos, const char* text, float y = theme::kRowSubY);

bool Toggle(UiContext& ctx, std::uint32_t id, ImVec2 pos, bool& value);
bool Slider(UiContext& ctx, std::uint32_t id, ImVec2 row_pos, float row_w, float& value, float min,
            float max, const char* value_text);

bool SliderBar(UiContext& ctx, std::uint32_t id, ImVec2 pos, float width, float height,
               float& value, float min, float max);

bool Stepper(UiContext& ctx, std::uint32_t id, ImVec2 row_pos, float row_w, float y_center,
             int& index, int count, const char* value_text);
bool CircleButton(UiContext& ctx, std::uint32_t id, ImVec2 pos, float size, const char* icon,
                  bool flip_horizontal = false, bool honour_reserved = true);
bool Chevron(UiContext& ctx, std::uint32_t id, ImVec2 pos, float size);

float WarningRow(UiContext& ctx, ImVec2 pos, float width, ImU32 accent, ImU32 background,
                 const char* text);

ImVec2 MeasureTextWrapped(UiContext& ctx, Font font, float wrap_width, const char* text);
void DrawTextWrapped(UiContext& ctx, Font font, ImVec2 pos, float wrap_width, ImU32 col,
                     const char* text);

class ScrollArea {
public:
    void Begin(UiContext& ctx, std::uint32_t id, ImVec2 min, ImVec2 max, float content_height);
    void End(UiContext& ctx);
    [[nodiscard]] float offset() const { return offset_; }

private:
    ImVec2 min_{}, max_{};
    float offset_ = 0.0f;
    std::uint32_t id_ = 0;
};

}
