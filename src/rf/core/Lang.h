#pragma once
#include <format>
#include <string>
#include <string_view>

namespace rf {

enum class Language { Russian, English };

void SetLanguage(Language language);
[[nodiscard]] Language CurrentLanguage();

[[nodiscard]] const char* Tr(const char* russian);
[[nodiscard]] std::string_view Tr(std::string_view russian);

template <class... Args>
[[nodiscard]] std::string TrFormat(const char* russian, const Args&... args) {
    return std::vformat(Tr(russian), std::make_format_args(args...));
}

inline constexpr const char* kLanguageNames[] = {"Русский", "English"};

}
