#include <imgui.h>

#include <string>
#include <vector>

#include "rf/core/Lang.h"
#include "rf/ui/Assets.h"
#include "rf/ui/Theme.h"
#include "test_framework.h"

using rf::Language;
using rf::SetLanguage;
using rf::Tr;
using rf::ui::Font;
using rf::ui::FontSet;

namespace {

struct Phrase {
    const char* russian;
    Font font;
    float room;
};

constexpr float kSubtitleRoom =
    rf::ui::theme::kPanelW - 2 * rf::ui::theme::kPanelPad - rf::ui::theme::kRowTextX - 105.0f;
constexpr float kTitleRoom =
    rf::ui::theme::kPanelW - 2 * rf::ui::theme::kPanelPad - rf::ui::theme::kRowTextX - 95.0f;

const Phrase kRows[] = {
    {"Язык", Font::Body, kTitleRoom},
    {"Размер интерфейса", Font::Body, kTitleRoom},
    {"Масштаб меню и подсказок", Font::Small, kSubtitleRoom},
    {"Индикатор записи", Font::Body, kTitleRoom},
    {"Кружок и таймер поверх игры", Font::Small, kSubtitleRoom},
    {"Кнопка остановки", Font::Body, kTitleRoom},
    {"Рядом с индикатором записи", Font::Small, kSubtitleRoom},
    {"Индикатор микрофона", Font::Body, kTitleRoom},
    {"Значок в углу экрана", Font::Small, kSubtitleRoom},
    {"Индикатор повтора", Font::Body, kTitleRoom},
    {"Виден, пока повтор заряжен", Font::Small, kSubtitleRoom},
    {"Расположение", Font::Body, kTitleRoom},
    {"Размер значков", Font::Body, kTitleRoom},
    {"От 60% до 200%", Font::Small, kSubtitleRoom},
    {"Прозрачность", Font::Body, kTitleRoom},
    {"Насколько значки видно", Font::Small, kSubtitleRoom},
    {"Хранить откат в ОЗУ", Font::Body, kTitleRoom},
    {"Без записи на диск, быстрее", Font::Small, kSubtitleRoom},
    {"Настройки хранилища", Font::Small, kSubtitleRoom},
    {"Настройки записи видео", Font::Small, kSubtitleRoom},
    {"Настройки интерфейса", Font::Small, kSubtitleRoom},
    {"Длительность повтора", Font::Body, kTitleRoom},
};

float Width(FontSet& fonts, Font which, const char* text) {
    ImFont* font = fonts.Get(which);
    if (!font) return 0.0f;
    return font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, text).x;
}

}

TEST(Text_RowsFitTheirColumnInBothLanguages) {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1920, 1080);

    FontSet fonts;
    fonts.Load(io, "", 1.0f);

    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    if (!fonts.Get(Font::Body) || !fonts.Get(Font::Small)) {
        SKIP("no fonts embedded in this build");
        ImGui::DestroyContext();
        return;
    }

    for (Language language : {Language::Russian, Language::English}) {
        SetLanguage(language);
        for (const Phrase& row : kRows) {
            const char* text = Tr(row.russian);
            const float used = Width(fonts, row.font, text);
            if (used > row.room)
                ::testing::ReportFailure(__FILE__, __LINE__,
                                         std::string(text) + " needs " + std::to_string(used) +
                                             " px but the row gives " + std::to_string(row.room));
        }
    }

    SetLanguage(Language::Russian);
    ImGui::DestroyContext();
}
