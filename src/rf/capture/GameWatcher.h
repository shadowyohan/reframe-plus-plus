#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace rf {

struct GameWindow {
    void* hwnd = nullptr;
    std::uint32_t pid = 0;
    std::string exe;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    [[nodiscard]] bool valid() const { return hwnd != nullptr && pid != 0; }
    [[nodiscard]] bool SameAs(const GameWindow& o) const {
        return pid == o.pid && hwnd == o.hwnd && width == o.width && height == o.height;
    }
};

class GameWatcher {
public:
    ~GameWatcher();

    void Start(std::function<void(const GameWindow&)> on_change);
    void Stop();

    [[nodiscard]] GameWindow current() const;
    [[nodiscard]] bool running() const { return running_.load(std::memory_order_acquire); }

private:
    void Loop();

    std::function<void(const GameWindow&)> on_change_;
    mutable std::mutex mutex_;
    GameWindow current_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    void* wake_ = nullptr;
};

enum class GameVerdict {
    Game,
    NoWindow,
    OwnProcess,
    Shell,
    NotVisible,
    TooSmall,
    SystemProcess,
    KnownNonGame,
    NotAppWindow,
};

const char* ToString(GameVerdict v);

GameVerdict ClassifyWindow(void* hwnd, GameWindow& out, bool strict = false);

bool FindRecordableBelow(GameWindow& out);

}
