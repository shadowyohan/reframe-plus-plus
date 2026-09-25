#include "rf/integrations/ClipScheduler.h"

#include <algorithm>

namespace rf::integrations {

ClipScheduler::Result ClipScheduler::Add(std::uint32_t pre_seconds, std::uint32_t post_seconds,
                                         std::string_view tag, Ticks100ns now) {
    const Ticks100ns start = now - static_cast<Ticks100ns>(pre_seconds) * kOneSecond100ns;
    const Ticks100ns deadline = now + static_cast<Ticks100ns>(post_seconds) * kOneSecond100ns;

    if (!pending_) {
        while (!saved_.empty() && now - saved_.front() >= 60 * kOneSecond100ns) saved_.pop_front();
        if (saved_.size() >= kMaxClipsPerMinute) return Result::RateLimited;
        pending_ = Pending{start, deadline, {}};
    }

    pending_->start = std::min(pending_->start, start);
    pending_->deadline = std::max(pending_->deadline, deadline);
    if (!tag.empty() && std::find(pending_->tags.begin(), pending_->tags.end(), tag) ==
                            pending_->tags.end())
        pending_->tags.emplace_back(tag);
    return Result::Queued;
}

std::optional<ClipScheduler::Due> ClipScheduler::TakeDue(Ticks100ns now,
                                                         std::uint32_t max_seconds) {
    if (!pending_) return std::nullopt;

    const Ticks100ns longest = static_cast<Ticks100ns>(max_seconds) * kOneSecond100ns;
    if (now < pending_->deadline && now - pending_->start < longest) return std::nullopt;

    const Ticks100ns span = std::min(now - pending_->start, longest);
    Due due;
    due.seconds = static_cast<std::uint32_t>((span + kOneSecond100ns - 1) / kOneSecond100ns);
    due.tags = std::move(pending_->tags);
    pending_.reset();
    saved_.push_back(now);
    return due;
}

}
