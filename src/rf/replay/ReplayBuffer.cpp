#include "rf/replay/ReplayBuffer.h"

#include <windows.h>

#include <share.h>

#include <algorithm>
#include <format>
#include <unordered_map>

#include "rf/core/Log.h"

namespace rf {
namespace {

constexpr std::uint64_t kSegmentBytes = 32ull * 1024 * 1024;

constexpr std::size_t kMaxQueued = 2048;

constexpr auto kQueueWait = std::chrono::milliseconds(250);

}

ReplayBuffer::~ReplayBuffer() {
    StopWriter();
    CloseAll();
}

void ReplayBuffer::Configure(Ticks100ns window, std::size_t max_bytes,
                             const std::filesystem::path& temp_dir, bool in_memory) {
    StopWriter();

    std::scoped_lock lock(mutex_);
    window_ = window;
    max_bytes_ = max_bytes;

    if (in_memory_ != in_memory) {
        CloseAll();
        in_memory_ = in_memory;
        RF_INFO("replay buffer keeps clips {}", in_memory ? "in memory" : "on disk");
    }

    if (dir_ != temp_dir) {
        CloseAll();
        dir_ = temp_dir;

        std::error_code ec;
        std::filesystem::create_directories(dir_, ec);

        for (const auto& entry : std::filesystem::directory_iterator(dir_, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            const auto name = entry.path().filename().string();
            if (name.starts_with("replay-") && entry.path().extension() == ".bin")
                std::filesystem::remove(entry.path(), ec);
        }
        RF_INFO("replay spool: {}", dir_.string());
    }
    TrimLocked();
    StartWriter();
}

void ReplayBuffer::StartWriter() {
    if (writing_ || (dir_.empty() && !in_memory_)) return;
    writing_ = true;
    writer_ = std::thread([this] { WriterLoop(); });
}

void ReplayBuffer::StopWriter() {
    if (!writing_) return;
    writing_ = false;
    queue_cv_.notify_all();
    space_cv_.notify_all();
    if (writer_.joinable()) writer_.join();

    std::scoped_lock lock(queue_mutex_);
    queue_.clear();
}

void ReplayBuffer::Push(PacketPtr packet) {
    if (!packet || !writing_) return;

    {
        std::unique_lock lock(queue_mutex_);
        if (!space_cv_.wait_for(lock, kQueueWait,
                                [this] { return queue_.size() < kMaxQueued || !writing_; })) {
            RF_WARN("replay spool cannot keep up with the encoder - dropping a packet");
            return;
        }
        if (!writing_) return;
        queue_.push_back(std::move(packet));
    }
    queue_cv_.notify_one();
}

void ReplayBuffer::WriterLoop() {
    ::SetThreadDescription(::GetCurrentThread(), L"rf-replay-spool");

    while (writing_.load(std::memory_order_relaxed)) {
        PacketPtr packet;
        {
            std::unique_lock lock(queue_mutex_);
            queue_cv_.wait(lock, [this] { return !queue_.empty() || !writing_; });
            if (!writing_ && queue_.empty()) break;
            packet = std::move(queue_.front());
            queue_.pop_front();
        }
        space_cv_.notify_one();
        if (!packet) continue;

        std::scoped_lock lock(mutex_);
        WriteLocked(*packet);
        TrimLocked();
    }
}

void ReplayBuffer::WriteLocked(const Packet& packet) {
    if (packet.data.empty()) return;

    const bool need_segment =
        segments_.empty() || (segments_.back().size >= kSegmentBytes &&
                              packet.kind == MediaKind::Video && packet.keyframe);

    if (need_segment) {
        Segment segment;
        segment.id = next_segment_++;

        if (in_memory_) {
            segment.memory.reserve(kSegmentBytes);
            segments_.push_back(std::move(segment));
        } else {
        segment.path = dir_ / std::format("replay-{:06}.bin", segment.id);

        segment.write_file = _wfsopen(segment.path.c_str(), L"wb", _SH_DENYNO);
        if (!segment.write_file) {
            RF_ERROR("cannot open replay segment {}", segment.path.string());
            return;
        }
        std::setvbuf(segment.write_file, nullptr, _IOFBF, 1 << 20);

        segment.read_file = _wfsopen(segment.path.c_str(), L"rb", _SH_DENYNO);
        if (!segment.read_file) {
            RF_ERROR("cannot open replay segment {} for reading", segment.path.string());
            std::fclose(segment.write_file);
            return;
        }
        segments_.push_back(std::move(segment));
        }
    }

    Segment& segment = segments_.back();
    const std::uint64_t offset = segment.size;

    if (in_memory_) {
        segment.memory.insert(segment.memory.end(), packet.data.begin(), packet.data.end());
    } else if (std::fwrite(packet.data.data(), 1, packet.data.size(), segment.write_file) !=
               packet.data.size()) {
        char reason[128] = {};
        strerror_s(reason, errno);
        RF_ERROR("replay spool write failed: {} (errno {})", reason, errno);

        CloseSegment(segment);
        for (const Entry& entry : video_)
            if (entry.segment == segment.id) bytes_ -= entry.size;
        for (const Entry& entry : audio_)
            if (entry.segment == segment.id) bytes_ -= entry.size;
        const std::uint32_t dead = segment.id;
        std::erase_if(video_, [dead](const Entry& e) { return e.segment == dead; });
        std::erase_if(audio_, [dead](const Entry& e) { return e.segment == dead; });
        segments_.pop_back();
        return;
    }
    segment.size += packet.data.size();
    ++segment.refs;

    Entry entry;
    entry.pts = packet.pts;
    entry.dts = packet.dts;
    entry.duration = packet.duration;
    entry.offset = offset;
    entry.size = static_cast<std::uint32_t>(packet.data.size());
    entry.segment = segment.id;
    entry.track = packet.track;
    entry.kind = packet.kind;
    entry.keyframe = packet.keyframe;

    bytes_ += entry.size;
    (packet.kind == MediaKind::Video ? video_ : audio_).push_back(entry);
}

void ReplayBuffer::CloseSegment(Segment& segment) {
    segment.memory.clear();
    segment.memory.shrink_to_fit();
    if (segment.write_file) std::fclose(segment.write_file);
    if (segment.read_file) std::fclose(segment.read_file);
    segment.write_file = nullptr;
    segment.read_file = nullptr;

    if (segment.path.empty()) return;

    std::error_code ec;
    std::filesystem::remove(segment.path, ec);
}

void ReplayBuffer::ReleaseSegment(std::uint32_t id) {
    for (auto it = segments_.begin(); it != segments_.end(); ++it) {
        if (it->id != id) continue;
        if (--it->refs > 0) return;

        if (it->id == segments_.back().id) return;
        CloseSegment(*it);
        segments_.erase(it);
        return;
    }
}

void ReplayBuffer::TrimLocked() {
    if (video_.empty()) return;

    const Ticks100ns newest = video_.back().pts;

    auto over_budget = [&] {
        return (newest - video_.front().pts) > window_ || bytes_ > max_bytes_;
    };

    while (video_.size() > 1 && over_budget()) {
        bytes_ -= video_.front().size;
        ReleaseSegment(video_.front().segment);
        video_.pop_front();
        while (video_.size() > 1 && !video_.front().keyframe) {
            bytes_ -= video_.front().size;
            ReleaseSegment(video_.front().segment);
            video_.pop_front();
        }
        ++dropped_gops_;
    }

    const Ticks100ns floor = video_.front().pts;
    while (!audio_.empty() && audio_.front().pts < floor) {
        bytes_ -= audio_.front().size;
        ReleaseSegment(audio_.front().segment);
        audio_.pop_front();
    }
}

void ReplayBuffer::CloseAll() {
    for (Segment& segment : segments_) CloseSegment(segment);
    segments_.clear();
    video_.clear();
    audio_.clear();
    bytes_ = 0;
}

void ReplayBuffer::Clear() {
    std::scoped_lock lock(mutex_);
    CloseAll();
}

std::vector<PacketPtr> ReplayBuffer::Snapshot(Ticks100ns window) const {
    std::scoped_lock lock(mutex_);
    std::vector<PacketPtr> out;
    if (video_.empty()) return out;

    for (const Segment& segment : segments_)
        if (segment.write_file) std::fflush(segment.write_file);


    const Ticks100ns newest = video_.back().pts;
    const Ticks100ns wanted = newest - std::min(window, window_);

    auto start = video_.begin();
    for (auto it = video_.begin(); it != video_.end(); ++it) {
        if (it->keyframe && it->pts <= wanted) start = it;
        if (it->pts > wanted) break;
    }

    const Ticks100ns base = start->pts;

    std::vector<const Entry*> wanted_entries;
    wanted_entries.reserve(static_cast<std::size_t>(std::distance(start, video_.end())) +
                           audio_.size());
    for (auto it = start; it != video_.end(); ++it) wanted_entries.push_back(&*it);
    for (const Entry& entry : audio_)
        if (entry.pts >= base) wanted_entries.push_back(&entry);

    std::stable_sort(wanted_entries.begin(), wanted_entries.end(),
                     [](const Entry* a, const Entry* b) { return a->dts < b->dts; });

    std::unordered_map<std::uint32_t, const Segment*> readers;
    for (const Segment& segment : segments_) readers[segment.id] = &segment;

    out.reserve(wanted_entries.size());
    for (const Entry* entry : wanted_entries) {
        const auto reader = readers.find(entry->segment);
        if (reader == readers.end() || !reader->second) continue;
        const Segment& source = *reader->second;
        if (!in_memory_ && !source.read_file) continue;

        auto packet = std::make_shared<Packet>();
        packet->kind = entry->kind;
        packet->track = entry->track;
        packet->keyframe = entry->keyframe;
        packet->pts = entry->pts - base;
        packet->dts = entry->dts - base;
        packet->duration = entry->duration;
        packet->data.resize(entry->size);

        if (in_memory_) {
            if (entry->offset + entry->size > source.memory.size()) {
                RF_WARN("replay buffer lost a packet - clip will be short");
                break;
            }
            std::memcpy(packet->data.data(), source.memory.data() + entry->offset, entry->size);
        } else if (_fseeki64(source.read_file, static_cast<std::int64_t>(entry->offset),
                             SEEK_SET) != 0 ||
                   std::fread(packet->data.data(), 1, entry->size, source.read_file) !=
                       entry->size) {
            RF_WARN("replay spool read failed - clip will be short");
            break;
        }
        out.push_back(std::move(packet));
    }

    RF_INFO("replay snapshot: {} packets, {:.1f}s, {:.0f} MB read back", out.size(),
            Ticks100nsToMs(newest - base) / 1000.0, bytes_ / 1048576.0);
    return out;
}

ReplayBuffer::Stats ReplayBuffer::GetStats() const {
    std::scoped_lock lock(mutex_);
    Stats s;
    s.bytes = bytes_;
    s.video_packets = video_.size();
    s.audio_packets = audio_.size();
    s.dropped_gops = dropped_gops_;
    if (!video_.empty()) s.duration = video_.back().pts - video_.front().pts;
    {
        std::scoped_lock queue_lock(queue_mutex_);
        s.queued = queue_.size();
    }
    return s;
}

}
