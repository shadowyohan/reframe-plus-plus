#include <windows.h>

#include <chrono>
#include <string>
#include <thread>

#include "rf/integrations/ClipScheduler.h"
#include "rf/integrations/ControlServer.h"
#include "rf/integrations/Protocol.h"
#include "rf/integrations/SdkHost.h"

#include "test_framework.h"

using namespace rf;
using namespace rf::integrations;

namespace {

constexpr Ticks100ns kSecond = kOneSecond100ns;

SdkHost GreetedHost(ClientId client = 1, std::uint32_t pid = 42) {
    SdkHost host("2.0.0");
    host.SetUserReplay(false, 10);
    host.Connected(client, pid);
    CHECK(host.Handle(client, "hello name=\"My Game\" v=1", 0).starts_with("ok v=1"));
    (void)host.TakeOutgoing();
    return host;
}

}

TEST(Protocol_ParsesNameArgumentAndFields) {
    const auto command = ParseCommand("auto on seconds=30 name=\"My \\\"Game\\\"\"");
    CHECK(command.has_value());
    CHECK(command->name == "auto");
    CHECK(command->arg == "on");
    CHECK(command->GetNumber("seconds") == 30u);
    CHECK(command->Get("name") == "My \"Game\"");
}

TEST(Protocol_KeepsWindowsPathsInQuotes) {
    const auto command = ParseCommand(R"(event saved path="C:\Users\me\clip.mp4")");
    CHECK(command.has_value());
    CHECK(command->Get("path") == R"(C:\Users\me\clip.mp4)");
    CHECK(ParseCommand("x path=" + Quote(R"(C:\a b\c.mp4)"))->Get("path") == R"(C:\a b\c.mp4)");
}

TEST(Protocol_RejectsBrokenLines) {
    CHECK(!ParseCommand("").has_value());
    CHECK(!ParseCommand("hello name=\"unterminated").has_value());
    CHECK(!ParseCommand("clip one two").has_value());
    CHECK(!ParseCommand("clip =3").has_value());
    CHECK(!ParseCommand("clip pre=1 stray").has_value());
    CHECK(!ParseCommand("clip post=3").value().GetNumber("pre").has_value());
    CHECK(!ParseCommand("clip post=-3").value().GetNumber("post").has_value());
}

TEST(Protocol_TagsAreShortAndPlain) {
    CHECK(IsValidTag("kill"));
    CHECK(IsValidTag("head_shot-2"));
    CHECK(!IsValidTag(""));
    CHECK(!IsValidTag("two words"));
    CHECK(!IsValidTag(std::string(33, 'a')));
}

TEST(ClipScheduler_SavesAfterThePostRoll) {
    ClipScheduler clips;
    CHECK(clips.Add(10, 3, "kill", 100 * kSecond) == ClipScheduler::Result::Queued);
    CHECK(!clips.TakeDue(102 * kSecond, 600).has_value());
    const auto due = clips.TakeDue(103 * kSecond, 600);
    CHECK(due.has_value());
    CHECK_EQ(due->seconds, 13u);
    CHECK(due->tags == std::vector<std::string>{"kill"});
    CHECK(!clips.pending());
}

TEST(ClipScheduler_MergesAStreakIntoOneClip) {
    ClipScheduler clips;
    (void)clips.Add(10, 3, "kill", 100 * kSecond);
    (void)clips.Add(10, 3, "kill", 102 * kSecond);
    (void)clips.Add(10, 3, "ace", 104 * kSecond);
    CHECK(!clips.TakeDue(106 * kSecond, 600).has_value());
    const auto due = clips.TakeDue(107 * kSecond, 600);
    CHECK(due.has_value());
    CHECK_EQ(due->seconds, 17u);
    CHECK((due->tags == std::vector<std::string>{"kill", "ace"}));
}

TEST(ClipScheduler_EndlessStreakIsCutAtTheBufferLength) {
    ClipScheduler clips;
    for (int i = 0; i < 30; ++i) (void)clips.Add(10, 3, {}, (100 + i * 2) * kSecond);
    const auto due = clips.TakeDue(140 * kSecond, 30);
    CHECK(due.has_value());
    CHECK_EQ(due->seconds, 30u);
}

TEST(ClipScheduler_LimitsClipsPerMinute) {
    ClipScheduler clips;
    Ticks100ns now = 1000 * kSecond;
    for (std::size_t i = 0; i < ClipScheduler::kMaxClipsPerMinute; ++i) {
        CHECK(clips.Add(5, 0, {}, now) == ClipScheduler::Result::Queued);
        CHECK(clips.TakeDue(now, 600).has_value());
        now += kSecond;
    }
    CHECK(clips.Add(5, 0, {}, now) == ClipScheduler::Result::RateLimited);
    CHECK(clips.Add(5, 0, {}, now + 60 * kSecond) == ClipScheduler::Result::Queued);
}

TEST(SdkHost_EverythingButHelloNeedsHello) {
    SdkHost host("2.0.0");
    host.Connected(1, 42);
    CHECK(host.Handle(1, "auto on", 0) == "err no-hello");
    CHECK(host.Handle(1, "hello name=Game v=2", 0) == "err unsupported-version");
    CHECK(host.Handle(1, "hello v=1", 0) == "err bad-argument");
    CHECK(host.Handle(1, "hello name=Game v=1", 0).starts_with("ok v=1 server=2.0.0 armed="));
    CHECK(host.Handle(1, "ping", 0) == "ok");
    CHECK(host.Handle(1, "fly", 0) == "err unknown-command");
    CHECK(host.Handle(1, "clip \"", 0) == "err bad-syntax");
}

TEST(SdkHost_GreetsEachProcessOnce) {
    SdkHost host = GreetedHost(1, 42);
    CHECK(host.TakeGreetings() == std::vector<std::string>{"My Game"});
    host.Disconnected(1);
    host.Connected(2, 42);
    (void)host.Handle(2, "hello name=\"My Game\" v=1", 0);
    CHECK(host.TakeGreetings().empty());
}

TEST(SdkHost_AutoArmsTheBufferAndSetsItsLength) {
    SdkHost host = GreetedHost();
    CHECK(!host.armed_by_apps());
    CHECK(host.Handle(1, "clip", 0) == "err not-armed");

    CHECK(host.Handle(1, "auto on seconds=60", 0) == "ok armed=1 seconds=60");
    CHECK(host.armed_by_apps());
    CHECK_EQ(host.wanted_seconds(), 60u);
    CHECK_EQ(host.buffer_seconds(), 60u + SdkHost::kMaxPostSeconds);
    CHECK_EQ(host.audio_pid(), 42u);

    CHECK(host.Handle(1, "auto seconds=5000", 0) == "ok armed=1 seconds=600");
    CHECK(host.Handle(1, "auto off", 0) == "ok armed=0 seconds=10");
    CHECK(!host.armed_by_apps());
}

TEST(SdkHost_DisconnectReleasesTheBuffer) {
    SdkHost host = GreetedHost();
    (void)host.Handle(1, "auto on", 0);
    CHECK(host.armed_by_apps());
    host.Disconnected(1);
    CHECK(!host.armed_by_apps());
    CHECK_EQ(host.audio_pid(), 0u);
}

TEST(SdkHost_ClipUsesTheGamesLengthByDefault) {
    SdkHost host = GreetedHost();
    (void)host.Handle(1, "auto on seconds=30", 0);
    CHECK(host.Handle(1, "clip post=3 tag=kill", 100 * kSecond) == "ok queued");
    CHECK(host.Handle(1, "clip tag=bad!", 100 * kSecond) == "err bad-argument");

    CHECK(host.TakeDueClips(102 * kSecond).empty());
    const auto due = host.TakeDueClips(103 * kSecond);
    CHECK_EQ(due.size(), std::size_t{1});
    CHECK_EQ(due[0].seconds, 33u);
    CHECK_EQ(due[0].pid, 42u);
    CHECK(due[0].app == "My Game");
}

TEST(SdkHost_UserReplayAloneAllowsClips) {
    SdkHost host = GreetedHost();
    host.SetUserReplay(true, 20);
    CHECK(host.Handle(1, "clip", 0) == "ok queued");
    CHECK_EQ(host.TakeDueClips(0)[0].seconds, 20u);
}

TEST(SdkHost_AnnouncesStateChangesToGreetedClients) {
    SdkHost host = GreetedHost();
    host.SetUserReplay(true, 45);
    const auto outgoing = host.TakeOutgoing();
    CHECK_EQ(outgoing.size(), std::size_t{1});
    CHECK(outgoing[0].client == 1);
    CHECK(outgoing[0].line == "event state armed=1 seconds=45");
    host.SetUserReplay(true, 45);
    CHECK(host.TakeOutgoing().empty());
}

TEST(SdkHost_TellsGamesWhenTheMenuOpens) {
    SdkHost host = GreetedHost();
    host.SetOverlayOpen(true);
    host.SetOverlayOpen(true);
    host.SetOverlayOpen(false);
    const auto outgoing = host.TakeOutgoing();
    CHECK_EQ(outgoing.size(), std::size_t{2});
    CHECK(outgoing[0].line == "event overlay opened");
    CHECK(outgoing[1].line == "event overlay closed");
}

TEST(SdkHost_LateGameLearnsTheMenuIsAlreadyOpen) {
    SdkHost host("2.0.0");
    host.SetOverlayOpen(true);
    host.Connected(1, 42);
    (void)host.Handle(1, "hello name=Game v=1", 0);
    const auto outgoing = host.TakeOutgoing();
    CHECK_EQ(outgoing.size(), std::size_t{1});
    CHECK(outgoing[0].line == "event overlay opened");
}

TEST(SdkHost_PlayerCanSwitchAppClipsOff) {
    SdkHost host("2.0.0");
    host.SetAllowed(false);
    host.Connected(1, 42);
    (void)host.Handle(1, "hello name=Game v=1", 0);
    CHECK(host.TakeGreetings().empty());
    (void)host.Handle(1, "auto on seconds=60", 0);
    CHECK(!host.armed_by_apps());
    CHECK_EQ(host.wanted_seconds(), 0u);
    CHECK_EQ(host.audio_pid(), 0u);
    CHECK(host.Handle(1, "clip", 0) == "err disabled");

    host.SetAllowed(true);
    CHECK(host.armed_by_apps());
    CHECK(host.Handle(1, "clip", 0) == "ok queued");
}

TEST(SdkHost_EventsQuotePaths) {
    CHECK(SdkHost::SavedEvent(R"(C:\Clips\a b.mp4)", 24, {"kill", "ace"}) ==
          R"(event saved path="C:\\Clips\\a b.mp4" seconds=24 tag=kill+ace)");
    CHECK(SdkHost::FailedEvent("buffer-empty", {}) == "event failed reason=buffer-empty");
}

TEST(ControlServer_RoundTripOverAPipe) {
    const std::wstring name =
        L"\\\\.\\pipe\\reframe-plus-plus-test-" + std::to_wstring(::GetCurrentProcessId());
    ControlServer server;
    const Status started = server.Start(name);
    CHECK(started.ok());
    if (!started.ok()) return;

    HANDLE client = ::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  OPEN_EXISTING, 0, nullptr);
    CHECK(client != INVALID_HANDLE_VALUE);
    if (client == INVALID_HANDLE_VALUE) return;

    DWORD written = 0;
    const char request[] = "ping\nhello name=x v=1\n";
    CHECK(::WriteFile(client, request, sizeof(request) - 1, &written, nullptr) != FALSE);

    std::vector<ControlServer::Incoming> received;
    for (int i = 0; i < 200 && received.size() < 3; ++i) {
        for (auto& incoming : server.Take()) received.push_back(std::move(incoming));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK_EQ(received.size(), std::size_t{3});
    if (received.size() == 3) {
        CHECK(received[0].kind == ControlServer::Incoming::Kind::Connected);
        CHECK_EQ(received[0].pid, static_cast<std::uint32_t>(::GetCurrentProcessId()));
        CHECK(received[1].text == "ping");
        CHECK(received[2].text == "hello name=x v=1");

        server.Send(received[0].client, "ok");
        char reply[16]{};
        DWORD read = 0;
        CHECK(::ReadFile(client, reply, sizeof(reply), &read, nullptr) != FALSE);
        CHECK(std::string(reply, read) == "ok\n");
    }

    ::CloseHandle(client);
    bool closed = false;
    for (int i = 0; i < 200 && !closed; ++i) {
        for (const auto& incoming : server.Take())
            closed = closed || incoming.kind == ControlServer::Incoming::Kind::Closed;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(closed);
    server.Stop();
}
