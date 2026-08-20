#include "rf/core/Lang.h"

#include <cstring>

#include "test_framework.h"

using rf::Language;
using rf::SetLanguage;
using rf::Tr;
using rf::TrFormat;

TEST(Lang_RussianIsThePassThrough) {
    SetLanguage(Language::Russian);
    CHECK(std::strcmp(Tr("Настройки"), "Настройки") == 0);
    CHECK(TrFormat("{} ГБ", 12) == "12 ГБ");
}

TEST(Lang_EnglishTranslatesWhatItKnows) {
    SetLanguage(Language::English);
    CHECK(std::strcmp(Tr("Настройки"), "Settings") == 0);
    CHECK(std::strcmp(Tr("Индикатор микрофона"), "Microphone indicator") == 0);
    CHECK(TrFormat("{} ГБ", 12) == "12 GB");

    CHECK(std::strcmp(Tr("Discord"), "Discord") == 0);
    SetLanguage(Language::Russian);
}
