#include "rf/engine/ForegroundTracker.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <format>
#include <map>
#include <vector>

#include "rf/core/Strings.h"

#pragma comment(lib, "version.lib")

namespace rf {
namespace {

constexpr Ticks100ns kHistory = static_cast<Ticks100ns>(6) * 3600 * kOneSecond100ns;
constexpr std::size_t kMaxRuns = 20'000;
constexpr std::size_t kMaxCachedNames = 256;

bool IsShellClass(HWND window) {
    wchar_t name[32] = {};
    ::GetClassNameW(window, name, ARRAYSIZE(name));
    const std::wstring_view cls(name);
    return cls == L"Progman" || cls == L"WorkerW" || cls == L"Shell_TrayWnd" ||
           cls == L"Shell_SecondaryTrayWnd";
}

std::wstring ExePath(DWORD pid) {
    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    wchar_t path[MAX_PATH * 2] = {};
    DWORD size = ARRAYSIZE(path);
    const bool ok = ::QueryFullProcessImageNameW(process, 0, path, &size) != 0;
    ::CloseHandle(process);
    return ok ? std::wstring(path, size) : std::wstring{};
}

std::wstring VersionString(const std::vector<BYTE>& info, WORD language, WORD codepage,
                           const wchar_t* key) {
    const std::wstring query = std::format(L"\\StringFileInfo\\{:04x}{:04x}\\{}", language, codepage, key);
    wchar_t* value = nullptr;
    UINT length = 0;
    if (!::VerQueryValueW(info.data(), query.c_str(), reinterpret_cast<void**>(&value), &length) ||
        length <= 1)
        return {};
    return std::wstring(value, length - 1);
}

std::wstring DescribedName(const std::wstring& exe) {
    DWORD unused = 0;
    const DWORD size = ::GetFileVersionInfoSizeW(exe.c_str(), &unused);
    if (size == 0) return {};
    std::vector<BYTE> info(size);
    if (!::GetFileVersionInfoW(exe.c_str(), 0, size, info.data())) return {};

    struct Translation {
        WORD language;
        WORD codepage;
    };
    Translation* translations = nullptr;
    UINT length = 0;
    if (!::VerQueryValueW(info.data(), L"\\VarFileInfo\\Translation",
                          reinterpret_cast<void**>(&translations), &length) ||
        length < sizeof(Translation))
        return {};

    for (const wchar_t* key : {L"FileDescription", L"ProductName"}) {
        std::wstring name = VersionString(info, translations[0].language, translations[0].codepage, key);
        while (!name.empty() && std::iswspace(name.back())) name.pop_back();
        if (!name.empty() && name.size() <= 40) return name;
    }
    return {};
}

std::string ReadableAppName(const std::wstring& exe) {
    if (std::wstring described = DescribedName(exe); !described.empty()) return ToUtf8(described);
    std::wstring stem = std::filesystem::path(exe).stem().wstring();
    if (!stem.empty()) stem[0] = static_cast<wchar_t>(std::towupper(stem[0]));
    return ToUtf8(stem);
}

bool IsDesktopShell(const std::wstring& exe) {
    std::wstring name = std::filesystem::path(exe).filename().wstring();
    std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return std::towlower(c); });
    return name == L"explorer.exe" || name == L"searchhost.exe" || name == L"startmenuexperiencehost.exe" ||
           name == L"shellexperiencehost.exe" || name == L"lockapp.exe";
}

}

void ForegroundTracker::Record(Ticks100ns now, std::string app) {
    std::scoped_lock lock(mutex_);
    if (runs_.empty() || runs_.back().app != app) runs_.push_back({now, std::move(app)});
    while (runs_.size() > 1 && (runs_.size() > kMaxRuns || runs_[1].start < now - kHistory))
        runs_.pop_front();
}

std::string ForegroundTracker::Dominant(Ticks100ns from, Ticks100ns to) const {
    if (to <= from) return {};
    std::scoped_lock lock(mutex_);
    std::map<std::string, Ticks100ns> time_by_app;
    for (std::size_t i = 0; i < runs_.size(); ++i) {
        const Ticks100ns begin = std::max(runs_[i].start, from);
        const Ticks100ns end = std::min(i + 1 < runs_.size() ? runs_[i + 1].start : to, to);
        if (end > begin) time_by_app[runs_[i].app] += end - begin;
    }

    const Ticks100ns needed = static_cast<Ticks100ns>(static_cast<double>(to - from) * kDominantShare);
    for (const auto& [app, time] : time_by_app)
        if (!app.empty() && time > needed) return app;
    return {};
}

void ForegroundTracker::Clear() {
    std::scoped_lock lock(mutex_);
    runs_.clear();
}

std::string ForegroundAppNamer::AppOn(void* monitor) {
    HWND window = ::GetForegroundWindow();
    if (!window || ::IsIconic(window) || IsShellClass(window)) return {};
    if (monitor && ::MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST) != monitor) return {};

    DWORD pid = 0;
    ::GetWindowThreadProcessId(window, &pid);
    if (pid == 0 || pid == ::GetCurrentProcessId()) return {};

    if (const auto known = names_by_pid_.find(pid); known != names_by_pid_.end()) return known->second;
    if (names_by_pid_.size() > kMaxCachedNames) names_by_pid_.clear();

    const std::wstring exe = ExePath(pid);
    std::string name = exe.empty() || IsDesktopShell(exe) ? std::string{} : ReadableAppName(exe);
    names_by_pid_.emplace(pid, name);
    return name;
}

}
