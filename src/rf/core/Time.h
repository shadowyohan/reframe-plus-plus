#pragma once
#include <windows.h>

#include <cstdint>

namespace rf {

using Ticks100ns = std::int64_t;

inline std::int64_t QpcFrequency() {
    static const std::int64_t freq = [] {
        LARGE_INTEGER f{};
        ::QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    return freq;
}

inline std::int64_t QpcNow() {
    LARGE_INTEGER t{};
    ::QueryPerformanceCounter(&t);
    return t.QuadPart;
}

inline Ticks100ns QpcTo100ns(std::int64_t qpc) {

    const std::int64_t f = QpcFrequency();
    return (qpc / f) * 10'000'000 + ((qpc % f) * 10'000'000) / f;
}

inline Ticks100ns Now100ns() { return QpcTo100ns(QpcNow()); }

constexpr Ticks100ns kOneSecond100ns = 10'000'000;

constexpr Ticks100ns MsTo100ns(std::int64_t ms) { return ms * 10'000; }
constexpr double Ticks100nsToMs(Ticks100ns t) { return static_cast<double>(t) / 10'000.0; }

void PreciseSleepUntil(Ticks100ns deadline_100ns);

class MmcssScope {
public:
    explicit MmcssScope(const wchar_t* task_name);
    ~MmcssScope();
    MmcssScope(const MmcssScope&) = delete;
    MmcssScope& operator=(const MmcssScope&) = delete;

private:
    HANDLE handle_ = nullptr;
};

}
