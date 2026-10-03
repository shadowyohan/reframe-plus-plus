#include "rf/audio/WasapiCapture.h"

#include "test_framework.h"

using rf::SilenceFramesOwed;
using rf::Ticks100ns;

namespace {
constexpr Ticks100ns kMs = 10'000;
}

TEST(Silence_NothingOwedWhileAudioKeepsUp) {
    CHECK_EQ(SilenceFramesOwed(1'000 * kMs, 1'005 * kMs, 48'000), 0u);
}

TEST(Silence_CoversTheWholeQuietGap) {
    Ticks100ns expected = 0;
    const Ticks100ns now = 250 * kMs;
    std::uint64_t filled = 0;
    while (const std::uint32_t frames = SilenceFramesOwed(expected, now, 48'000)) {
        filled += frames;
        expected += static_cast<Ticks100ns>(frames) * 10'000'000 / 48'000;
    }
    CHECK(filled >= 48'000 * 240 / 1000);
    CHECK(filled <= 48'000 * 250 / 1000);
}

TEST(Silence_ComesInBoundedChunks) {
    CHECK_EQ(SilenceFramesOwed(0, 5'000 * kMs, 48'000), 4'800u);
}
