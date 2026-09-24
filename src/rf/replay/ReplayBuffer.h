#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "rf/core/Media.h"
#include "rf/core/Status.h"

namespace rf {

class ReplayBuffer {
public:
    ReplayBuffer() = default;
    ~ReplayBuffer();

    struct Stats {
        Ticks100ns duration = 0;
        std::size_t bytes = 0;
        std::size_t video_packets = 0;
        std::size_t audio_packets = 0;
        std::size_t dropped_gops = 0;
        std::size_t queued = 0;
    };

    void Configure(Ticks100ns window, std::size_t max_bytes,
                   const std::filesystem::path& temp_dir, bool in_memory = false);

    void Push(PacketPtr packet);
    void Clear();

    using PacketSink = std::function<Status(PacketPtr)>;

    [[nodiscard]] std::vector<PacketPtr> Snapshot(Ticks100ns window,
                                                  Ticks100ns* covered_to = nullptr) const;

    [[nodiscard]] Status Snapshot(Ticks100ns window, const PacketSink& sink,
                                  Ticks100ns* covered_to = nullptr) const;

    [[nodiscard]] Ticks100ns ClipStart(Ticks100ns window) const;

    void DropThrough(Ticks100ns pts);

    [[nodiscard]] Stats GetStats() const;

private:

    struct Entry {
        Ticks100ns pts = 0, dts = 0, duration = 0;
        std::uint64_t offset = 0;
        std::uint32_t size = 0;
        std::uint32_t segment = 0;
        std::uint32_t track = 0;
        MediaKind kind = MediaKind::Video;
        bool keyframe = false;
    };

    struct Segment {
        std::uint32_t id = 0;
        std::filesystem::path path;

        std::FILE* write_file = nullptr;
        std::FILE* read_file = nullptr;
        std::vector<std::uint8_t> memory;
        std::uint64_t size = 0;
        std::size_t refs = 0;
    };

    void StartWriter();
    void StopWriter();
    void WriterLoop();
    void WriteLocked(const Packet& packet);
    void TrimLocked();
    void CloseSegment(Segment& segment);
    void ReleaseSegment(std::uint32_t id);
    void CloseAll();
    [[nodiscard]] std::deque<Entry>::const_iterator ClipStartLocked(Ticks100ns window) const;

    mutable std::recursive_mutex mutex_;
    std::deque<Entry> video_;
    std::deque<Entry> audio_;
    std::deque<Segment> segments_;
    std::uint32_t next_segment_ = 0;

    std::filesystem::path dir_;
    bool in_memory_ = false;
    Ticks100ns window_ = 5 * 60 * kOneSecond100ns;
    std::size_t max_bytes_ = 8ull * 1024 * 1024 * 1024;
    std::size_t bytes_ = 0;
    std::size_t dropped_gops_ = 0;

    std::thread writer_;
    std::atomic<bool> writing_{false};
    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::condition_variable space_cv_;
    std::deque<PacketPtr> queue_;
};

}
