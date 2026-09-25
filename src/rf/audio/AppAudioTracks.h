#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "rf/audio/ProcessAudioTrack.h"

namespace rf {

struct AudioApp {
    std::uint32_t root_pid = 0;
    std::string name;
};

class AppAudioTracks {
public:
    ~AppAudioTracks();

    Status Start(std::uint32_t slots, std::uint32_t first_track, std::uint32_t bitrate_bps,
                 Ticks100ns epoch, const std::function<void(PacketPtr)>& on_packet);
    void Stop();

    void Feed(std::uint32_t frames, Ticks100ns timestamp);

    [[nodiscard]] std::uint32_t track_count() const {
        return static_cast<std::uint32_t>(slots_.size());
    }
    [[nodiscard]] std::vector<std::string> track_names() const;
    [[nodiscard]] std::vector<bool> AudibleSince(Ticks100ns pts) const;

    std::vector<std::string> TakeOverflowedApps();

    [[nodiscard]] static std::vector<AudioApp> ActiveAudioApps();

    [[nodiscard]] static std::uint32_t PickSlot(const std::vector<std::uint32_t>& assigned_roots,
                                                std::uint32_t root_pid, std::uint32_t slots);

private:
    struct Slot {
        ProcessAudioTrack track;
        std::uint32_t root_pid = 0;
        std::string name;
    };

    void WatchLoop();
    void Assign(const AudioApp& app);

    std::vector<std::unique_ptr<Slot>> slots_;
    mutable std::mutex slots_mutex_;
    std::set<std::uint32_t> overflowed_;
    std::vector<std::string> overflow_queue_;

    std::thread watcher_;
    std::atomic<bool> watching_{false};
    std::mutex wake_mutex_;
    std::condition_variable wake_;
};

}
