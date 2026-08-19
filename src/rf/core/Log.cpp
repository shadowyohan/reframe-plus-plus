#include "rf/core/Log.h"

#include <algorithm>
#include <format>

#include <windows.h>

#include <io.h>
#include <share.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

#include "rf/core/Paths.h"

namespace rf::log {
namespace {

std::mutex g_mutex;
std::FILE* g_file = nullptr;
std::atomic<Level> g_level{Level::Info};

constexpr std::string_view LevelTag(Level l) {
    switch (l) {
        case Level::Trace: return "TRC";
        case Level::Debug: return "DBG";
        case Level::Info:  return "INF";
        case Level::Warn:  return "WRN";
        case Level::Error: return "ERR";
    }
    return "???";
}

std::string_view Basename(std::string_view path) {
    const auto pos = path.find_last_of("\\/");
    return pos == std::string_view::npos ? path : path.substr(pos + 1);
}

std::wstring TimeStamp() {
    SYSTEMTIME now{};
    ::GetLocalTime(&now);
    wchar_t text[32];
    _snwprintf_s(text, _TRUNCATE, L"%04u-%02u-%02u_%02u-%02u-%02u", now.wYear, now.wMonth, now.wDay,
                 now.wHour, now.wMinute, now.wSecond);
    return text;
}

void PruneArchives(const std::filesystem::path& dir, std::size_t keep) {
    std::error_code ec;
    std::vector<std::filesystem::path> archives;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const auto name = entry.path().filename().wstring();
        if (name.starts_with(L"reframe-logs-") && entry.path().extension() == L".xz")
            archives.push_back(entry.path());
    }
    if (archives.size() <= keep) return;

    std::sort(archives.begin(), archives.end());
    for (std::size_t i = 0; i + keep < archives.size(); ++i)
        std::filesystem::remove(archives[i], ec);
}

bool RunTar(const std::filesystem::path& working_dir, const std::filesystem::path& archive,
            const std::filesystem::path& folder) {
    wchar_t system_dir[MAX_PATH]{};
    if (!::GetSystemDirectoryW(system_dir, MAX_PATH)) return false;

    const std::wstring tar = std::wstring(system_dir) + L"\\tar.exe";
    if (!std::filesystem::exists(tar)) return false;

    std::wstring command = L"\"" + tar + L"\" -caf \"" + archive.wstring() + L"\" \"" +
                           folder.filename().wstring() + L"\"";

    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};

    if (!::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                          nullptr, working_dir.c_str(), &startup, &process))
        return false;

    ::WaitForSingleObject(process.hProcess, 60'000);
    DWORD code = 1;
    ::GetExitCodeProcess(process.hProcess, &code);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return code == 0;
}

void ArchivePreviousRun(const std::filesystem::path& dir) {
    std::error_code ec;
    const std::wstring stamp = TimeStamp();
    const auto staging = dir / (L"reframe-logs-" + stamp);

    std::vector<std::filesystem::path> stale;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;

        const auto path = entry.path();
        if (path.extension() != L".log") continue;
        if (path.filename().wstring().starts_with(L"reframe-logs-")) continue;
        stale.push_back(path);
    }

    if (stale.empty()) return;

    std::filesystem::create_directories(staging, ec);

    bool anything = false;
    for (const auto& path : stale) {
        std::filesystem::rename(path, staging / path.filename(), ec);
        if (!ec) anything = true;
    }

    if (!anything) {
        std::filesystem::remove(staging, ec);
        return;
    }

    std::thread([dir, staging, stamp] {
        std::error_code ec;
        const auto archive = dir / (L"reframe-logs-" + stamp + L".tar.xz");
        if (RunTar(dir, archive, staging))
            std::filesystem::remove_all(staging, ec);
        PruneArchives(dir, 10);
    }).detach();
}

}

void Init(Level min_level) {
    std::scoped_lock lock(g_mutex);
    g_level.store(min_level, std::memory_order_relaxed);
    if (g_file) return;

    std::error_code ec;
    const auto dir = paths::LogDir();
    std::filesystem::create_directories(dir, ec);

    ArchivePreviousRun(dir);

    const auto current = dir / "reframe.log";
    g_file = _wfsopen(current.c_str(), L"wb", _SH_DENYWR);

    if (!g_file) {
        const auto fallback = dir / std::format("reframe-{}.log", ::GetCurrentProcessId());
        g_file = _wfsopen(fallback.c_str(), L"wb", _SH_DENYWR);
    }

    if (g_file) {
        static constexpr unsigned char kBom[] = {0xEF, 0xBB, 0xBF};
        std::fwrite(kBom, 1, sizeof(kBom), g_file);
        std::fflush(g_file);
        _commit(_fileno(g_file));
    } else {
        ::OutputDebugStringA("reframe++: no log file could be opened\n");
    }
}

void Shutdown() {
    std::scoped_lock lock(g_mutex);
    if (g_file) {
        std::fclose(g_file);
        g_file = nullptr;
    }
}

void SetLevel(Level level) { g_level.store(level, std::memory_order_relaxed); }
Level GetLevel() { return g_level.load(std::memory_order_relaxed); }

void Write(Level level, std::string_view file, int line, std::string msg) {

    SYSTEMTIME now{};
    ::GetLocalTime(&now);

    const std::string line_text =
        std::format("[{:02}:{:02}:{:02}.{:03}] [{}] [{:>5}] {} ({}:{})\n", now.wHour, now.wMinute,
                    now.wSecond, now.wMilliseconds, LevelTag(level), GetCurrentThreadId(), msg,
                    Basename(file), line);

    std::scoped_lock lock(g_mutex);
    if (g_file) {
        std::fputs(line_text.c_str(), g_file);
        std::fflush(g_file);

        static ULONGLONG last_commit = 0;
        const ULONGLONG now_ms = ::GetTickCount64();
        if (level >= Level::Warn || now_ms - last_commit > 500) {
            last_commit = now_ms;
            _commit(_fileno(g_file));
        }
    }
    ::OutputDebugStringA(line_text.c_str());
}

}
