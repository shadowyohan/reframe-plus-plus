#pragma once
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rf/core/Status.h"

namespace rf::integrations {

inline constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\reframe-plus-plus";

class ControlServer {
public:
    struct Incoming {
        enum class Kind { Connected, Line, Closed } kind = Kind::Line;
        std::uint64_t client = 0;
        std::uint32_t pid = 0;
        std::string text;
    };

    ControlServer();
    ~ControlServer();

    Status Start(const std::wstring& pipe_name = kPipeName);
    void Stop();

    [[nodiscard]] std::vector<Incoming> Take();
    void Send(std::uint64_t client, std::string line);

private:
    struct Instance;
    struct Outgoing {
        std::uint64_t client = 0;
        std::string line;
    };

    void Loop();
    Status Listen(bool first);
    void OnSignaled(Instance& instance);
    bool IssueRead(Instance& instance);
    void Deliver(Instance& instance, std::size_t bytes);
    bool Write(Instance& instance, const std::string& line);
    void Close(Instance& instance);
    void Emit(Incoming incoming);

    std::wstring pipe_name_;
    std::wstring security_;
    HANDLE wake_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::vector<std::unique_ptr<Instance>> instances_;
    std::uint64_t next_client_ = 1;

    std::mutex inbox_mutex_;
    std::vector<Incoming> inbox_;
    std::mutex outbox_mutex_;
    std::vector<Outgoing> outbox_;
};

}
