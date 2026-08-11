#include "rf/core/Paths.h"

#include <windows.h>
#include <knownfolders.h>
#include <shlobj_core.h>

#include <wrl/client.h>

namespace rf::paths {
namespace {

std::filesystem::path KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    if (FAILED(::SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &raw))) return {};
    std::filesystem::path p{raw};
    ::CoTaskMemFree(raw);
    return p;
}

}

std::filesystem::path DataDir() { return KnownFolder(FOLDERID_LocalAppData) / L"Reframe"; }
std::filesystem::path LogDir() { return DataDir() / L"logs"; }
std::filesystem::path SettingsFile() { return DataDir() / L"settings.ini"; }
std::filesystem::path DefaultVideoDir() { return KnownFolder(FOLDERID_Videos) / L"Reframe"; }

}
