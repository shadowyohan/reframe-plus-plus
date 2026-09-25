#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rf/core/Status.h"
#include "rf/core/Time.h"

namespace rf {

struct PixelRect {
    std::int32_t left = 0, top = 0, right = 0, bottom = 0;

    [[nodiscard]] std::int32_t width() const { return right - left; }
    [[nodiscard]] std::int32_t height() const { return bottom - top; }
    [[nodiscard]] bool empty() const { return right <= left || bottom <= top; }
    bool operator==(const PixelRect&) const = default;
};

struct WindowSample {
    Ticks100ns at = 0;
    bool visible = false;
    PixelRect crop;
};

struct FramePlan {
    bool black = true;
    PixelRect source;
};

struct CropJob {
    std::filesystem::path raw;
    std::filesystem::path output;
    std::vector<WindowSample> samples;
    std::uint32_t frame_width = 0;
    std::uint32_t frame_height = 0;
    std::uint32_t bitrate_kbps = 0;
    std::vector<std::string> audio_names;
    bool first_track_is_full_mix = false;
    bool use_gpu = true;
};

[[nodiscard]] bool NeedsCropping(const std::vector<WindowSample>& samples,
                                 std::uint32_t frame_width, std::uint32_t frame_height);

[[nodiscard]] PixelRect CropOutputSize(const std::vector<WindowSample>& samples,
                                       std::uint32_t frame_width, std::uint32_t frame_height);

[[nodiscard]] FramePlan PlanFrame(const std::vector<WindowSample>& samples, Ticks100ns at,
                                  std::uint32_t frame_width, std::uint32_t frame_height);

void CopyNv12(const std::uint8_t* source, std::uint32_t source_pitch,
              std::uint32_t source_rows, const FramePlan& plan, std::uint8_t* target,
              std::uint32_t target_width, std::uint32_t target_height);

using CropProgress = std::function<void(float)>;

Status CropClip(const CropJob& job, const CropProgress& progress = {});

Status WriteCropJob(const std::filesystem::path& file, const CropJob& job);
Status ReadCropJob(const std::filesystem::path& file, CropJob& job);

Status CropInHelperProcess(const CropJob& job, const std::filesystem::path& helper,
                           const CropProgress& progress = {});

int RunCropHelper(const std::filesystem::path& job_file);

class ClipCropper {
public:
    using Done = std::function<void(const Status&, const CropJob&)>;

    ~ClipCropper();

    void Push(CropJob job, CropProgress progress, Done done);
    void Stop();

private:
    struct Work {
        CropJob job;
        CropProgress progress;
        Done done;
    };

    void Loop();

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Work> queue_;
    bool running_ = false;
};

}
