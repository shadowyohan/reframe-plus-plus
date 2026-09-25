#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>

#include "rf/audio/AacEncoder.h"
#include "rf/audio/WasapiCapture.h"

namespace rf {

class ProcessAudioTrack {
public:
    ~ProcessAudioTrack();

    Status Open(std::uint32_t track, std::uint32_t bitrate_bps, Ticks100ns epoch,
                const std::function<void(PacketPtr)>& on_packet);
    Status Capture(std::uint32_t pid);
    void Close();

    void Feed(std::uint32_t frames, Ticks100ns timestamp);

    [[nodiscard]] std::uint32_t track() const { return track_; }
    [[nodiscard]] bool capturing() const { return capture_ != nullptr; }
    [[nodiscard]] bool AudibleSince(Ticks100ns pts) const { return last_audible_.load() >= pts; }
    [[nodiscard]] IMFMediaType* output_type() const { return encoder_.output_type(); }

private:
    void OnAudio(const AudioChunk& chunk);

    static constexpr Ticks100ns kNeverAudible = std::numeric_limits<Ticks100ns>::min();

    AacEncoder encoder_;
    std::unique_ptr<WasapiCapture> capture_;
    std::uint32_t track_ = 0;
    Ticks100ns epoch_ = 0;
    std::mutex fifo_mutex_;
    std::vector<float> fifo_;
    std::vector<float> scratch_;
    std::atomic<Ticks100ns> last_audible_{kNeverAudible};
};

}
