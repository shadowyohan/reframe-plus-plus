#pragma once
#include <atomic>
#include <filesystem>
#include <functional>
#include <thread>

#include "rf/core/Status.h"

namespace rf {

enum class RtxGeneration { Unsupported, Turing, Ampere, Ada, Blackwell };

[[nodiscard]] RtxGeneration RtxGenerationFromComputeCapability(int major, int minor);
[[nodiscard]] RtxGeneration DetectRtxGeneration();
[[nodiscard]] const wchar_t* MaxineInstallerUrl(RtxGeneration generation);

class MaxineSetup {
public:
    ~MaxineSetup();

    Status Start(const std::filesystem::path& download_dir);

    [[nodiscard]] bool running() const { return running_.load(); }
    [[nodiscard]] float progress() const { return progress_.load(); }

    std::function<void(const Status&)> on_finished;

private:
    void Run(std::wstring url, std::filesystem::path target);

    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<float> progress_{0.0f};
};

}
