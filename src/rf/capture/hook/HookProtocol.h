#pragma once

#include <cstdint>

namespace rf::hook {

inline constexpr std::uint32_t kMagic = 0x4B48'4652u;  // 'RFHK'
inline constexpr std::uint32_t kVersion = 1;

// Keyed-mutex keys. The texture starts on kKeyHook, so the hook wins the first
// frame and the host waits until there is something to read.
inline constexpr std::uint64_t kKeyHook = 0;  // hook may write
inline constexpr std::uint64_t kKeyHost = 1;  // host may read

enum class GraphicsApi : std::uint32_t {
    None = 0,
    D3D11 = 1,
};

// Lives in a named section created by the host before injection. Every field is
// written by exactly one side, so plain volatile reads and a release of the
// keyed mutex are enough ordering - there is no lock.
struct alignas(64) SharedState {
    // --- written by the host, read by the hook ---
    std::uint32_t magic;    // kMagic; the hook unloads itself if this is wrong
    std::uint32_t version;  // kVersion
    std::uint32_t capture;  // 1 = publish frames, 0 = stay hooked but idle
    std::uint32_t stop;     // 1 = release everything and go quiet
    // reframe++'s own PID. A clean shutdown sets `stop`; a crash cannot, and a

    std::uint32_t host_pid;

    std::uint32_t api;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t format;
    std::uint32_t shared_handle;
    std::uint32_t texture_serial;
    std::uint64_t frame_index;
    std::int64_t qpc;

    std::uint32_t reserved[8];
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
