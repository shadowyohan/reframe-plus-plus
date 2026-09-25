#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "rf/integrations/ClipScheduler.h"
#include "rf/integrations/Protocol.h"

namespace rf::integrations {

using ClientId = std::uint64_t;

struct DueClip {
    ClientId client = 0;
    std::uint32_t pid = 0;
    std::string app;
    std::uint32_t seconds = 0;
    std::vector<std::string> tags;
};

struct Outgoing {
    ClientId client = 0;
    std::string line;
};

class SdkHost {
public:
    static constexpr std::uint32_t kMinSeconds = 5;
    static constexpr std::uint32_t kMaxSeconds = 600;
    static constexpr std::uint32_t kMaxPostSeconds = 30;
    static constexpr std::size_t kMaxNameBytes = 64;

    explicit SdkHost(std::string server_version) : server_version_(std::move(server_version)) {}

    void Connected(ClientId client, std::uint32_t pid);
    void Disconnected(ClientId client);

    [[nodiscard]] std::string Handle(ClientId client, std::string_view line, Ticks100ns now);

    void SetUserReplay(bool enabled, std::uint32_t seconds);
    void SetOverlayOpen(bool open);
    void SetAllowed(bool allowed);

    [[nodiscard]] std::vector<DueClip> TakeDueClips(Ticks100ns now);
    [[nodiscard]] std::vector<std::string> TakeGreetings();
    [[nodiscard]] std::vector<Outgoing> TakeOutgoing();

    [[nodiscard]] bool armed_by_apps() const;
    [[nodiscard]] std::uint32_t wanted_seconds() const;
    [[nodiscard]] std::uint32_t buffer_seconds() const;
    [[nodiscard]] std::uint32_t audio_pid() const;

    [[nodiscard]] static std::string SavedEvent(std::string_view path, std::uint32_t seconds,
                                                const std::vector<std::string>& tags);
    [[nodiscard]] static std::string FailedEvent(std::string_view reason,
                                                 const std::vector<std::string>& tags);

private:
    struct Client {
        std::uint32_t pid = 0;
        std::string name;
        bool greeted = false;
        bool auto_on = false;
        std::uint32_t seconds = 0;
        std::uint64_t order = 0;
        ClipScheduler clips;
    };

    std::string Hello(ClientId id, Client& client, const Command& command);
    std::string Auto(Client& client, const Command& command);
    std::string Clip(Client& client, const Command& command, Ticks100ns now);

    [[nodiscard]] bool armed() const { return user_enabled_ || armed_by_apps(); }
    [[nodiscard]] std::uint32_t seconds_for(const Client& client) const;
    void QueueStateIfChanged();
    void QueueForGreeted(const std::string& line);

    std::string server_version_;
    std::map<ClientId, Client> clients_;
    std::set<std::uint32_t> greeted_pids_;
    std::vector<std::string> greetings_;
    std::vector<Outgoing> outgoing_;
    std::uint64_t next_order_ = 1;

    bool user_enabled_ = false;
    std::uint32_t user_seconds_ = 0;
    bool announced_armed_ = false;
    std::uint32_t announced_seconds_ = 0;
    bool overlay_open_ = false;
    bool allowed_ = true;
};

}
