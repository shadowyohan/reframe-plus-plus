#include "rf/audio/NoiseSuppressor.h"

#include <cmath>
#include <random>

#include "test_framework.h"

using namespace rf;

namespace {

double RmsAfterDenoising(NoiseSuppression kind) {
    MicDenoiser denoiser;
    if (!denoiser.Open(kind).ok()) return -1.0;

    constexpr std::uint32_t kChannels = 2;
    constexpr std::uint32_t kChunkFrames = 441;
    constexpr int kChunks = 220;
    std::mt19937 random(7);
    std::normal_distribution<float> hiss(0.0f, 0.05f);

    double energy = 0.0;
    std::size_t counted = 0;
    std::vector<float> chunk(kChunkFrames * kChannels);
    for (int n = 0; n < kChunks; ++n) {
        for (std::uint32_t i = 0; i < kChunkFrames; ++i) {
            const float sample = hiss(random);
            for (std::uint32_t c = 0; c < kChannels; ++c) chunk[i * kChannels + c] = sample;
        }
        denoiser.Process(chunk.data(), kChunkFrames, kChannels);
        if (n < kChunks / 2) continue;
        for (float v : chunk) energy += static_cast<double>(v) * v;
        counted += chunk.size();
    }
    return std::sqrt(energy / static_cast<double>(counted));
}

}

TEST(NoiseSuppressor_RnnoiseSilencesSteadyHiss) {
    const double rms = RmsAfterDenoising(NoiseSuppression::RNNoise);
    CHECK(rms >= 0.0);
    CHECK(rms < 0.05 * 0.3);
}

TEST(NoiseSuppressor_SpeexSilencesSteadyHiss) {
    const double rms = RmsAfterDenoising(NoiseSuppression::Speex);
    CHECK(rms >= 0.0);
    CHECK(rms < 0.05 * 0.5);
}

TEST(NoiseSuppressor_OutputKeepsTheChunkLength) {
    MicDenoiser denoiser;
    CHECK(denoiser.Open(NoiseSuppression::Speex).ok());
    std::vector<float> chunk(37 * 2, 0.25f);
    for (int n = 0; n < 50; ++n) denoiser.Process(chunk.data(), 37, 2);
    CHECK(denoiser.active());
}
