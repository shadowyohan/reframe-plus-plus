#include "rf/core/Log.h"

#include <format>

#include <windows.h>

#include <share.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <mutex>

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

}

void Init(Level min_level) {
    std::scoped_lock lock(g_mutex);
    g_level.store(min_level, std::memory_order_relaxed);
    if (g_file) return;

    std::error_code ec;
    const auto dir = paths::LogDir();
    std::filesystem::create_directories(dir, ec);

    const auto current = dir / "reframe.log";

    constexpr int kHistory = 5;
    std::filesystem::remove(dir / std::format("reframe.{}.log", kHistory), ec);
    for (int i = kHistory; i > 1; --i) {
        std::filesystem::rename(dir / std::format("reframe.{}.log", i - 1),
                                dir / std::format("reframe.{}.log", i), ec);
    }
    std::filesystem::rename(current, dir / "reframe.1.log", ec);

    std::filesystem::remove(dir / "reframe.prev.log", ec);
    std::filesystem::copy_file(dir / "reframe.1.log", dir / "reframe.prev.log", ec);

    g_file = _wfsopen(current.c_str(), L"wb", _SH_DENYWR);

    if (!g_file) {
        const auto fallback = dir / std::format("reframe-{}.log", ::GetCurrentProcessId());
        g_file = _wfsopen(fallback.c_str(), L"wb", _SH_DENYWR);
    }

    if (g_file) {
        static constexpr unsigned char kBom[] = {0xEF, 0xBB, 0xBF};
        std::fwrite(kBom, 1, sizeof(kBom), g_file);
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
    }
    ::OutputDebugStringA(line_text.c_str());
}

}
