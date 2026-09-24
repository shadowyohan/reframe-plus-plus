#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include "rf/core/Status.h"

namespace rf {

enum class NoiseSuppression : std::uint32_t { RNNoise, Speex, Maxine };

inline constexpr std::uint32_t kDenoiseFrame = 480;

class NoiseSuppressor {
public:
    virtual ~NoiseSuppressor() = default;
    virtual void Process(float* mono_frame) = 0;
};

Status CreateNoiseSuppressor(NoiseSuppression kind, std::unique_ptr<NoiseSuppressor>& out);

[[nodiscard]] std::filesystem::path MaxineSdkDir();
[[nodiscard]] bool MaxineInstalled();

class MicDenoiser {
public:
    Status Open(NoiseSuppression kind);
    void Close() { engine_.reset(); }
    [[nodiscard]] bool active() const { return engine_ != nullptr; }

    void Process(float* interleaved, std::uint32_t frames, std::uint32_t channels);

private:
    std::unique_ptr<NoiseSuppressor> engine_;
    std::vector<float> pending_;
    std::vector<float> ready_;
};

}
