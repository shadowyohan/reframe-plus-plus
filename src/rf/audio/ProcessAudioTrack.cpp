#include "rf/audio/ProcessAudioTrack.h"

#include <algorithm>
#include <cmath>

#include "rf/core/Log.h"

namespace rf {
namespace {

constexpr std::size_t kFifoCapSamples = 48'000 / 4 * 2;
constexpr float kAudibleFloor = 1.0e-4f;

}

ProcessAudioTrack::~ProcessAudioTrack() { Close(); }

Status ProcessAudioTrack::Open(std::uint32_t track, std::uint32_t bitrate_bps, Ticks100ns epoch,
                               const std::function<void(PacketPtr)>& on_packet) {
    track_ = track;
    epoch_ = epoch;
    last_audible_.store(kNeverAudible);
    return encoder_.Open(AudioFormat{}, bitrate_bps, epoch, on_packet, track);
}

Status ProcessAudioTrack::Capture(std::uint32_t pid) {
    if (capture_) capture_->Stop();
    capture_ = std::make_unique<WasapiCapture>();
    if (auto s = capture_->StartApplication(pid, [this](const AudioChunk& c) { OnAudio(c); });
        !s.ok()) {
        capture_.reset();
        return s;
    }
    return Status::Ok();
}

void ProcessAudioTrack::Close() {
    if (capture_) capture_->Stop();
    capture_.reset();
    encoder_.Close();
    std::scoped_lock lock(fifo_mutex_);
    fifo_.clear();
}

void ProcessAudioTrack::OnAudio(const AudioChunk& chunk) {
    if (!chunk.samples || chunk.channels != 2) return;
    const std::size_t count = static_cast<std::size_t>(chunk.frames) * 2;
    const bool audible = std::any_of(chunk.samples, chunk.samples + count,
                                     [](float v) { return std::abs(v) > kAudibleFloor; });
    if (audible) last_audible_.store(chunk.timestamp - epoch_);

    std::scoped_lock lock(fifo_mutex_);
    fifo_.insert(fifo_.end(), chunk.samples, chunk.samples + count);
    if (fifo_.size() > kFifoCapSamples)
        fifo_.erase(fifo_.begin(), fifo_.end() - kFifoCapSamples);
}

void ProcessAudioTrack::Feed(std::uint32_t frames, Ticks100ns timestamp) {
    const std::size_t samples = static_cast<std::size_t>(frames) * 2;
    scratch_.assign(samples, 0.0f);
    {
        std::scoped_lock lock(fifo_mutex_);
        const std::size_t take = std::min(fifo_.size(), samples);
        std::copy_n(fifo_.begin(), take, scratch_.begin());
        fifo_.erase(fifo_.begin(), fifo_.begin() + static_cast<std::ptrdiff_t>(take));
    }
    if (auto s = encoder_.Feed(scratch_.data(), frames, timestamp); !s.ok())
        RF_WARN("application track encode: {}", s.str());
}

}
