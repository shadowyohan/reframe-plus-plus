#include "rf/engine/ForegroundTracker.h"

#include "test_framework.h"

using rf::ForegroundTracker;
using rf::Ticks100ns;

namespace {
constexpr Ticks100ns kSecond = 10'000'000;
}

TEST(Foreground_NamesTheAppThatFilledMostOfTheClip) {
    ForegroundTracker tracker;
    tracker.Record(0, "");
    tracker.Record(2 * kSecond, "Discord");
    tracker.Record(9 * kSecond, "");
    CHECK(tracker.Dominant(0, 10 * kSecond) == "Discord");
}

TEST(Foreground_StaysGenericBelowSixtyPercent) {
    ForegroundTracker tracker;
    tracker.Record(0, "Discord");
    tracker.Record(5 * kSecond, "Telegram");
    CHECK(tracker.Dominant(0, 10 * kSecond).empty());
}

TEST(Foreground_OnlyCountsTheClipWindow) {
    ForegroundTracker tracker;
    tracker.Record(0, "Discord");
    tracker.Record(100 * kSecond, "Minecraft");
    CHECK(tracker.Dominant(95 * kSecond, 120 * kSecond) == "Minecraft");
    CHECK(tracker.Dominant(0, 90 * kSecond) == "Discord");
}

TEST(Foreground_TheDesktopNeverWins) {
    ForegroundTracker tracker;
    tracker.Record(0, "");
    CHECK(tracker.Dominant(0, 10 * kSecond).empty());
}
