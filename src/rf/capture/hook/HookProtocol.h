#pragma once

#include <cstdint>

namespace rf::hook {

inline constexpr std::uint32_t kMagic = 0x4B48'4652u;
inline constexpr std::uint32_t kVersion = 3;

inline constexpr std::uint32_t kSlots = 2;

enum class GraphicsApi : std::uint32_t {
    None = 0,
    D3D11 = 1,
};

struct alignas(64) SharedState {

    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t capture;
    std::uint32_t stop;

    std::uint32_t host_pid;

    std::uint32_t api;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t format;
    std::uint32_t shared_handle[kSlots];
    std::uint32_t write_slot;
    std::uint32_t texture_serial;
    std::uint64_t frame_index;
    std::int64_t qpc;

    std::uint32_t overlay_handle;
    std::uint32_t overlay_serial;
    std::uint32_t overlay_width;
    std::uint32_t overlay_height;
    std::uint32_t overlay_visible;

    std::uint64_t capture_period_qpc;

    std::uint32_t reserved[4];
};

static_assert(sizeof(SharedState) <= 256, "keep the section to a single page");

inline void BuildName(wchar_t (&out)[64], const wchar_t* suffix, std::uint32_t pid) {

    wchar_t* p = out;
    for (const wchar_t* s = L"Local\\reframe-hook-"; *s;) *p++ = *s++;
    for (const wchar_t* s = suffix; *s;) *p++ = *s++;
    *p++ = L'-';
    wchar_t digits[11];
    int n = 0;
    do {
        digits[n++] = static_cast<wchar_t>(L'0' + pid % 10);
        pid /= 10;
    } while (pid);
    while (n) *p++ = digits[--n];
    *p = L'\0';
}

struct Names {
    wchar_t section[64];
    wchar_t frame[64];
    wchar_t ready[64];

    explicit Names(std::uint32_t pid) {
        BuildName(section, L"shm", pid);
        BuildName(frame, L"frame", pid);
        BuildName(ready, L"ready", pid);
    }
};

}
