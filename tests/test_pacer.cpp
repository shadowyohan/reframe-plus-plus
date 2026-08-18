#include "rf/engine/Recorder.h"

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
