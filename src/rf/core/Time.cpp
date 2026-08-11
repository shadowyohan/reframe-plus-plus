#include "rf/core/Time.h"

#include <avrt.h>

#include <thread>

#pragma comment(lib, "avrt.lib")

namespace rf {
namespace {

HANDLE HighResTimer() {
    static thread_local HANDLE timer = ::CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    return timer;
}

constexpr Ticks100ns kSpinMargin = 3'000;  // 0.3 ms

}  // namespace

void PreciseSleepUntil(Ticks100ns deadline) {
    const Ticks100ns now = Now100ns();
    if (deadline <= now) return;

    if (HANDLE timer = HighResTimer(); timer) {
        const Ticks100ns wait = deadline - now - kSpinMargin;
        if (wait > 0) {
            LARGE_INTEGER due{};
            due.QuadPart = -wait;  // negative == relative
            if (::SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0))
                ::WaitForSingleObject(timer, INFINITE);
        }
    }

    while (Now100ns() < deadline) ::YieldProcessor();
}

MmcssScope::MmcssScope(const wchar_t* task_name) {
    DWORD index = 0;
    handle_ = ::AvSetMmThreadCharacteristicsW(task_name, &index);
    if (handle_) ::AvSetMmThreadPriority(handle_, AVRT_PRIORITY_HIGH);
}

MmcssScope::~MmcssScope() {
    if (handle_) ::AvRevertMmThreadCharacteristics(handle_);
}

}  // namespace rf
