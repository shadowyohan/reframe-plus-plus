#pragma once
#include <string>
#include <string_view>

namespace rf {

std::string ToUtf8(std::wstring_view text);
std::wstring ToWide(std::string_view text);

}
