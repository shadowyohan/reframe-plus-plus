#pragma once
#include <imgui.h>

namespace rf::ui {

namespace theme {

constexpr ImU32 kPanelBg      = IM_COL32(0x1b, 0x1b, 0x1b, 255);
constexpr ImU32 kPanelBorder  = IM_COL32(255, 255, 255, 13);

constexpr ImU32 kNotchTint    = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kRowBg        = IM_COL32(255, 255, 255, 13);
constexpr ImU32 kRowBgHover   = IM_COL32(255, 255, 255, 26);
constexpr ImU32 kRowBgActive  = IM_COL32(255, 255, 255, 33);
constexpr ImU32 kBadgeBg      = IM_COL32(255, 255, 255, 13);
constexpr ImU32 kText         = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kTextMuted    = IM_COL32(255, 255, 255, 77);
constexpr ImU32 kTextFaint    = IM_COL32(255, 255, 255, 26);
constexpr ImU32 kAccent       = IM_COL32(0x9e, 0xff, 0x96, 255);
constexpr ImU32 kTrack        = IM_COL32(255, 255, 255, 46);
constexpr ImU32 kCircleBtn    = IM_COL32(0xd9, 0xd9, 0xd9, 51);
constexpr ImU32 kThumbFill    = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kDanger       = IM_COL32(0xff, 0x6b, 0x6b, 255);

constexpr ImU32 kWarnRed      = IM_COL32(0xff, 0x52, 0x52, 255);
constexpr ImU32 kWarnRedBg    = IM_COL32(0xff, 0x52, 0x52, 77);
constexpr ImU32 kWarnAmber    = IM_COL32(0xff, 0xbd, 0x52, 255);
constexpr ImU32 kWarnAmberBg  = IM_COL32(0xff, 0xbd, 0x52, 77);

constexpr ImU32 kAppAccent    = IM_COL32(0xa0, 0x99, 0xff, 255);
constexpr ImU32 kAppAccentBg  = IM_COL32(0xa0, 0x99, 0xff, 26);
constexpr ImU32 kScrim        = IM_COL32(0, 0, 0, 102);

constexpr float kScreenPad    = 25.0f;
constexpr float kPanelW       = 438.0f;
constexpr float kPanelRadius  = 42.0f;
constexpr float kPanelPad     = 20.0f;
constexpr float kItemGap      = 10.0f;
constexpr float kHeaderGap    = 20.0f;

constexpr float kLogoBox      = 60.0f;
constexpr float kLogoRadius   = 15.0f;
constexpr float kNotchW       = 11.5f;
constexpr float kNotchH       = 20.57f;
constexpr float kTitlePillRadius = 20.0f;

constexpr float kRowRadius    = 14.0f;
constexpr float kRowIconX     = 10.0f;
constexpr float kRowIconY     = 11.0f;
constexpr float kRowIcon      = 24.0f;
constexpr float kRowTextX     = 44.0f;
constexpr float kRowTitleY    = 10.0f;
constexpr float kRowSubY      = 40.0f;

constexpr float kToggleW      = 85.0f;
constexpr float kToggleH      = 46.0f;
constexpr float kCircleBtnSize = 52.0f;

constexpr float kTrackY       = 82.0f;
constexpr float kTrackW       = 290.0f;
constexpr float kTrackH       = 6.0f;
constexpr float kThumbSize    = 18.0f;

constexpr float kRowH_Simple  = 56.0f;
constexpr float kRowH_TwoLine = 76.0f;
constexpr float kRowH_Slider  = 114.0f;
constexpr float kRowH_Badge   = 90.0f;
constexpr float kRowH_Format  = 100.0f;
constexpr float kGalleryStripH = 116.0f;

constexpr float kToastW       = 434.0f;
constexpr float kToastH       = 87.0f;
constexpr float kToastRadius  = 24.0f;
constexpr float kToastBadge   = 88.0f;
constexpr float kToastGapY    = 22.0f;
constexpr float kToastMargin  = 30.0f;
constexpr float kPillH        = 41.0f;
constexpr float kPillRadius   = 37.0f;

}

enum class Font {
    H1,
    H2,
    Body,
    Small,
    Badge,
    BadgeItalic,
    Tiny,
    Toast,
    Warning,
    WarningBold,
    Micro,
    Count
};

}
