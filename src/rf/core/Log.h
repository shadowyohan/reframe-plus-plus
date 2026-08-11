#pragma once
#include <format>
#include <string>
#include <string_view>

namespace rf::log {

enum class Level { Trace, Debug, Info, Warn, Error };

void Init(Level min_level = Level::Info);
void Shutdown();

void SetLevel(Level level);
Level GetLevel();

void Write(Level level, std::string_view file, int line, std::string msg);

namespace detail {
inline bool Enabled(Level level) { return level >= GetLevel(); }
}

}

#define RF_LOG_AT(lvl, ...)                                                        \
    do {                                                                           \
        if (::rf::log::detail::Enabled(lvl))                                       \
            ::rf::log::Write(lvl, __FILE__, __LINE__, std::format(__VA_ARGS__));   \
    } while (0)

#define RF_TRACE(...) RF_LOG_AT(::rf::log::Level::Trace, __VA_ARGS__)
#define RF_DEBUG(...) RF_LOG_AT(::rf::log::Level::Debug, __VA_ARGS__)
#define RF_INFO(...)  RF_LOG_AT(::rf::log::Level::Info,  __VA_ARGS__)
#define RF_WARN(...)  RF_LOG_AT(::rf::log::Level::Warn,  __VA_ARGS__)
#define RF_ERROR(...) RF_LOG_AT(::rf::log::Level::Error, __VA_ARGS__)
