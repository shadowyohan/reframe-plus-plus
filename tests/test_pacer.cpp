#include "rf/engine/Recorder.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "test_framework.h"

using rf::Recorder;
using rf::Ticks100ns;

namespace {

constexpr Ticks100ns kPeriod60 = 166'667;

struct Pacer {
    Ticks100ns deadline = 0;
    int accepted = 0;

    void Offer(Ticks100ns ts) {
        if (!Recorder::PacerAccepts(ts, deadline, kPeriod60)) return;
        ++accepted;
        deadline += kPeriod60;
        if (deadline <= ts) deadline = ts + kPeriod60;
    }
};

}

TEST(Pacer_TakesJitteredFramesAtTheTargetRate) {

    Pacer pacer;
    for (int i = 0; i < 100; ++i) {
        const Ticks100ns wobble = (i % 2) ? 10'000 : -10'000;
        pacer.Offer(i * kPeriod60 + wobble);
    }
    CHECK_EQ(pacer.accepted, 100);
}

TEST(Pacer_SamplesAFastGameDownToTheTargetRate) {

    constexpr Ticks100ns kGameFrame = 33'333;
    Pacer pacer;
    for (int i = 0; i < 1500; ++i) pacer.Offer(i * kGameFrame);
    CHECK_EQ(pacer.accepted, 301);
}

TEST(Pacer_RejectsWhatIsTrulyTooEarly) {
    CHECK(Recorder::PacerAccepts(kPeriod60 - kPeriod60 / 4, kPeriod60, kPeriod60));
    CHECK(!Recorder::PacerAccepts(kPeriod60 / 4, kPeriod60, kPeriod60));
}

TEST(Pacer_EvensOutA75HzSourceInto60FpsFrames) {

    constexpr Ticks100ns kSource = 133'690;

    Pacer pacer;
    Ticks100ns slot = 0;
    std::vector<Ticks100ns> emitted;

    for (int i = 0; i < 600; ++i) {
        const Ticks100ns arrived = static_cast<Ticks100ns>(i) * kSource;
        if (!Recorder::PacerAccepts(arrived, pacer.deadline, kPeriod60)) continue;

        slot = emitted.empty() ? arrived
                               : Recorder::SmoothedSlot(slot + kPeriod60, arrived, kPeriod60);
        emitted.push_back(slot);

        pacer.deadline += kPeriod60;
        if (pacer.deadline <= arrived) pacer.deadline = arrived + kPeriod60;
    }

    CHECK(emitted.size() > 400);

    Ticks100ns worst = 0;
    for (std::size_t i = 2; i < emitted.size(); ++i) {
        const Ticks100ns gap = emitted[i] - emitted[i - 1];
        worst = std::max(worst, std::abs(gap - kPeriod60));
    }

    CHECK(worst < kPeriod60 / 10);
}

TEST(Pacer_FollowsASourceThatDriftsAwayFromTheTarget) {

    constexpr Ticks100ns kSlowSource = 166'834;

    Ticks100ns slot = 0;
    for (int i = 1; i < 4000; ++i) {
        const Ticks100ns arrived = static_cast<Ticks100ns>(i) * kSlowSource;
        slot = Recorder::SmoothedSlot(slot + kPeriod60, arrived, kPeriod60);
    }

    const Ticks100ns arrived_last = static_cast<Ticks100ns>(3999) * kSlowSource;
    CHECK(std::abs(slot - arrived_last) < kPeriod60);
}
