#pragma once
#include <atomic>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

#include "rf/core/Status.h"

namespace rf {

class TrackMixer {
public:
    static constexpr float kMaxGain = 2.0f;

    ~TrackMixer();

    void Open(const std::filesystem::path& file, std::size_t tracks);
    void Close();

    void SetGain(std::size_t track, float gain);
    void SetMasterVolume(float volume) { master_.store(volume, std::memory_order_relaxed); }
    void Play(double from_seconds);
    void Pause();
    void Seek(double seconds);

    [[nodiscard]] bool playing() const { return playing_.load(std::memory_order_relaxed); }
    [[nodiscard]] double position() const { return position_.load(std::memory_order_relaxed); }

private:
    struct Track;

    void Loop();
    Status OpenTracks(std::vector<Track>& tracks) const;
    static void Fill(Track& track, std::size_t samples);
    static void SeekTrack(Track& track, double seconds);

    std::filesystem::path file_;
    std::size_t track_count_ = 0;
    std::unique_ptr<std::atomic<float>[]> gains_;
    std::atomic<float> master_{1.0f};
    std::atomic<bool> running_{false};
    std::atomic<bool> playing_{false};
    std::atomic<double> seek_to_{-1.0};
    std::atomic<double> position_{0.0};
    std::thread thread_;
};

}
