#include "rf/replay/ReplayBuffer.h"

#include <chrono>
#include <thread>

#include "test_framework.h"

using namespace rf;

namespace {

PacketPtr MakeVideo(Ticks100ns pts, bool keyframe, std::size_t bytes = 1000) {
    auto p = std::make_shared<Packet>();
    p->kind = MediaKind::Video;
    p->pts = pts;
    p->dts = pts;
    p->keyframe = keyframe;
    p->duration = kOneSecond100ns / 60;
    p->data.assign(bytes, 0);
    return p;
}

std::filesystem::path TempDir(const char* name) {
    return std::filesystem::temp_directory_path() / "reframe-tests" / name;
}

void Settle(const ReplayBuffer& buffer) {
    for (int i = 0; i < 200 && buffer.GetStats().queued > 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

void FillSeconds(ReplayBuffer& buffer, int seconds, std::size_t bytes_per_frame = 1000) {
    const int frames = seconds * 60;
    for (int i = 0; i < frames; ++i)
        buffer.Push(MakeVideo(static_cast<Ticks100ns>(i) * kOneSecond100ns / 60, i % 120 == 0,
                              bytes_per_frame));
    Settle(buffer);
}

}

TEST(ReplayBuffer_TrimsToWindow) {
    ReplayBuffer buffer;
    buffer.Configure(5 * kOneSecond100ns, 1024ull * 1024 * 1024, TempDir("trim"));
    FillSeconds(buffer, 30);

    const auto stats = buffer.GetStats();

    CHECK(stats.duration <= 7 * kOneSecond100ns);
    CHECK(stats.duration >= 3 * kOneSecond100ns);
    CHECK(stats.dropped_gops > 0);
}

TEST(ReplayBuffer_AlwaysStartsOnKeyframe) {
    ReplayBuffer buffer;
    buffer.Configure(5 * kOneSecond100ns, 1024ull * 1024 * 1024, TempDir("keyframe"));
    FillSeconds(buffer, 30);

    const auto packets = buffer.Snapshot(5 * kOneSecond100ns);
    CHECK(!packets.empty());
    CHECK(packets.front()->keyframe);

    CHECK_EQ(packets.front()->pts, 0ll);
}

TEST(ReplayBuffer_RespectsMemoryCeiling) {
    ReplayBuffer buffer;

    buffer.Configure(600 * kOneSecond100ns, 4 * 1024 * 1024, TempDir("ceiling"));
    FillSeconds(buffer, 60, 10'000);

    const auto stats = buffer.GetStats();
    CHECK(stats.bytes <= 4 * 1024 * 1024 + 10'000 * 120);
    CHECK(stats.dropped_gops > 0);
}

TEST(ReplayBuffer_SnapshotIsMonotonic) {
    ReplayBuffer buffer;
    buffer.Configure(10 * kOneSecond100ns, 1024ull * 1024 * 1024, TempDir("monotonic"));
    FillSeconds(buffer, 20);

    const auto packets = buffer.Snapshot(10 * kOneSecond100ns);
    for (std::size_t i = 1; i < packets.size(); ++i)
        CHECK(packets[i]->dts >= packets[i - 1]->dts);
}

TEST(ReplayBuffer_NextSnapshotStartsWhereTheLastEnded) {
    ReplayBuffer buffer;
    buffer.Configure(30 * kOneSecond100ns, 1024ull * 1024 * 1024, TempDir("continue"));
    FillSeconds(buffer, 20);

    Ticks100ns covered_to = 0;
    const auto first = buffer.Snapshot(30 * kOneSecond100ns, &covered_to);
    CHECK(!first.empty());
    buffer.DropThrough(covered_to);

    CHECK_EQ(buffer.GetStats().video_packets, std::size_t{0});

    const int frames = 10 * 60;
    for (int i = 0; i < frames; ++i)
        buffer.Push(MakeVideo(covered_to + static_cast<Ticks100ns>(i + 1) * kOneSecond100ns / 60,
                              i % 120 == 0));
    Settle(buffer);

    const auto second = buffer.Snapshot(30 * kOneSecond100ns);
    CHECK(!second.empty());
    CHECK(second.front()->keyframe);

    CHECK(buffer.GetStats().duration <= 10 * kOneSecond100ns);
}

TEST(ReplayBuffer_SegmentsRotateWithoutWaitingForKeyframe) {
    const auto dir = TempDir("rotate");
    ReplayBuffer buffer;
    buffer.Configure(600 * kOneSecond100ns, 1024ull * 1024 * 1024, dir);

    constexpr std::size_t kMegabyte = 1024 * 1024;
    for (int i = 0; i < 70; ++i)
        buffer.Push(MakeVideo(static_cast<Ticks100ns>(i) * kOneSecond100ns / 60, i == 0, kMegabyte));
    Settle(buffer);

    std::size_t segment_files = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
        if (entry.path().filename().string().starts_with("replay-")) ++segment_files;
    CHECK(segment_files >= 3);
}

TEST(ReplayBuffer_SnapshotStreamsEveryPacketIntoSink) {
    ReplayBuffer buffer;
    buffer.Configure(10 * kOneSecond100ns, 1024ull * 1024 * 1024, TempDir("stream"));
    FillSeconds(buffer, 8);

    std::size_t streamed = 0;
    const auto count = [&streamed](PacketPtr) {
        ++streamed;
        return Status::Ok();
    };
    CHECK(buffer.Snapshot(10 * kOneSecond100ns, count).ok());
    CHECK_EQ(streamed, buffer.Snapshot(10 * kOneSecond100ns).size());

    const auto refuse = [](PacketPtr) { return Status::Fail("disk full"); };
    CHECK(!buffer.Snapshot(10 * kOneSecond100ns, refuse).ok());
}

TEST(ReplayBuffer_EmptySnapshotIsSafe) {
    ReplayBuffer buffer;
    buffer.Configure(5 * kOneSecond100ns, 1024 * 1024, TempDir("empty"));
    CHECK(buffer.Snapshot(5 * kOneSecond100ns).empty());
    CHECK_EQ(buffer.GetStats().video_packets, std::size_t{0});
}
