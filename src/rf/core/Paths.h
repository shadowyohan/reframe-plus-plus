#pragma once
#include <filesystem>

namespace rf::paths {

std::filesystem::path DataDir();

std::filesystem::path LogDir();

std::filesystem::path SettingsFile();

std::filesystem::path DefaultVideoDir();

}
