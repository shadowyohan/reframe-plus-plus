#include "rf/integrations/SdkHost.h"

#include <algorithm>
#include <format>

namespace rf::integrations {
namespace {

bool IsValidName(std::string_view name) {
    return !name.empty() && name.size() <= SdkHost::kMaxNameBytes &&
           std::none_of(name.begin(), name.end(),
                        [](char c) { return static_cast<unsigned char>(c) < 0x20; });
}

std::string TagField(const std::vector<std::string>& tags) {
    return tags.empty() ? std::string{} : " tag=" + JoinTags(tags);
}

std::string Error(std::string_view code) { return std::format("err {}", code); }

}

void SdkHost::Connected(ClientId client, std::uint32_t pid) { clients_[client].pid = pid; }

void SdkHost::Disconnected(ClientId client) {
    clients_.erase(client);
    QueueStateIfChanged();
}

std::string SdkHost::Handle(ClientId id, std::string_view line, Ticks100ns now) {
    const auto found = clients_.find(id);
    if (found == clients_.end()) return Error("internal");
    Client& client = found->second;

    const auto command = ParseCommand(line);
    if (!command) return Error("bad-syntax");

    std::string reply;
    if (command->name == "hello")
        reply = Hello(id, client, *command);
    else if (!client.greeted)
        reply = Error("no-hello");
    else if (command->name == "auto")
        reply = Auto(client, *command);
    else if (command->name == "clip")
        reply = Clip(client, *command, now);
    else if (command->name == "ping")
        reply = "ok";
    else
        reply = Error("unknown-command");

    QueueStateIfChanged();
    return reply;
}

std::string SdkHost::Hello(ClientId id, Client& client, const Command& command) {
    const auto name = command.Get("name");
    const auto version = command.GetNumber("v");
    if (!name || !IsValidName(*name) || !version) return Error("bad-argument");
    if (*version != kProtocolVersion) return Error("unsupported-version");

    client.name = std::string(*name);
    if (!client.greeted) {
        client.order = next_order_++;
        if (overlay_open_) outgoing_.push_back({id, "event overlay opened"});
    }
    client.greeted = true;
    if (allowed_ && greeted_pids_.insert(client.pid).second) greetings_.push_back(client.name);

    return std::format("ok v={} server={} armed={} seconds={}", kProtocolVersion,
                       Quote(server_version_), armed() ? 1 : 0, user_seconds_);
}

std::string SdkHost::Auto(Client& client, const Command& command) {
    const auto seconds = command.GetNumber("seconds");
    if (command.Has("seconds") && !seconds) return Error("bad-argument");

    if (command.arg == "on") {
        if (!client.auto_on) client.order = next_order_++;
        client.auto_on = true;
    } else if (command.arg == "off") {
        client.auto_on = false;
    } else if (!command.arg.empty() || !seconds) {
        return Error("bad-argument");
    }

    if (seconds) client.seconds = std::clamp(*seconds, kMinSeconds, kMaxSeconds);
    if (client.auto_on && client.seconds == 0)
        client.seconds = std::clamp(user_seconds_, kMinSeconds, kMaxSeconds);

    return std::format("ok armed={} seconds={}", armed() ? 1 : 0, seconds_for(client));
}

std::string SdkHost::Clip(Client& client, const Command& command, Ticks100ns now) {
    if (!allowed_) return Error("disabled");
    if (!armed()) return Error("not-armed");

    const auto pre = command.GetNumber("pre");
    const auto post = command.GetNumber("post");
    const auto tag = command.Get("tag");
    if ((command.Has("pre") && !pre) || (command.Has("post") && !post) ||
        (tag && !IsValidTag(*tag)))
        return Error("bad-argument");

    const std::uint32_t pre_seconds = std::clamp(pre.value_or(seconds_for(client)), 1u, kMaxSeconds);
    const std::uint32_t post_seconds = std::min(post.value_or(0u), kMaxPostSeconds);
    if (client.clips.Add(pre_seconds, post_seconds, tag.value_or(""), now) ==
        ClipScheduler::Result::RateLimited)
        return Error("rate-limited");
    return "ok queued";
}

void SdkHost::SetAllowed(bool allowed) {
    allowed_ = allowed;
    QueueStateIfChanged();
}

void SdkHost::SetUserReplay(bool enabled, std::uint32_t seconds) {
    user_enabled_ = enabled;
    user_seconds_ = seconds;
    QueueStateIfChanged();
}

std::vector<DueClip> SdkHost::TakeDueClips(Ticks100ns now) {
    std::vector<DueClip> due;
    const std::uint32_t longest = buffer_seconds();
    for (auto& [id, client] : clients_) {
        auto clip = client.clips.TakeDue(now, longest);
        if (!clip) continue;
        due.push_back({id, client.pid, client.name, clip->seconds, std::move(clip->tags)});
    }
    return due;
}

std::vector<std::string> SdkHost::TakeGreetings() { return std::exchange(greetings_, {}); }

std::vector<Outgoing> SdkHost::TakeOutgoing() { return std::exchange(outgoing_, {}); }

bool SdkHost::armed_by_apps() const {
    return allowed_ && std::any_of(clients_.begin(), clients_.end(),
                       [](const auto& entry) { return entry.second.auto_on; });
}

std::uint32_t SdkHost::wanted_seconds() const {
    if (!allowed_) return 0;
    std::uint32_t wanted = 0;
    for (const auto& [id, client] : clients_)
        if (client.auto_on) wanted = std::max(wanted, client.seconds);
    return wanted;
}

std::uint32_t SdkHost::audio_pid() const {
    if (!allowed_) return 0;
    const Client* chosen = nullptr;
    for (const auto& [id, client] : clients_) {
        if (!client.greeted) continue;
        const bool better = !chosen || (client.auto_on && !chosen->auto_on) ||
                            (client.auto_on == chosen->auto_on && client.order > chosen->order);
        if (better) chosen = &client;
    }
    return chosen ? chosen->pid : 0;
}

std::uint32_t SdkHost::buffer_seconds() const {
    const std::uint32_t wanted = wanted_seconds();
    return std::max(user_seconds_, wanted ? wanted + kMaxPostSeconds : 0u);
}

std::uint32_t SdkHost::seconds_for(const Client& client) const {
    return client.auto_on ? client.seconds : user_seconds_;
}

void SdkHost::QueueStateIfChanged() {
    if (armed() == announced_armed_ && user_seconds_ == announced_seconds_) return;
    announced_armed_ = armed();
    announced_seconds_ = user_seconds_;

    QueueForGreeted(
        std::format("event state armed={} seconds={}", announced_armed_ ? 1 : 0, user_seconds_));
}

void SdkHost::SetOverlayOpen(bool open) {
    if (open == overlay_open_) return;
    overlay_open_ = open;
    QueueForGreeted(open ? "event overlay opened" : "event overlay closed");
}

void SdkHost::QueueForGreeted(const std::string& line) {
    for (const auto& [id, client] : clients_)
        if (client.greeted) outgoing_.push_back({id, line});
}

std::string SdkHost::SavedEvent(std::string_view path, std::uint32_t seconds,
                                const std::vector<std::string>& tags) {
    return std::format("event saved path={} seconds={}{}", Quote(path), seconds, TagField(tags));
}

std::string SdkHost::FailedEvent(std::string_view reason, const std::vector<std::string>& tags) {
    return std::format("event failed reason={}{}", reason, TagField(tags));
}

}
