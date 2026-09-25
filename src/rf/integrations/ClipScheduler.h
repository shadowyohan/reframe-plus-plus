#pragma once
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rf/core/Time.h"

namespace rf::integrations {

class ClipScheduler {
public:
    static constexpr std::size_t kMaxClipsPerMinute = 20;

    enum class Result { Queued, RateLimited };

    struct Due {
        std::uint32_t seconds = 0;
        std::vector<std::string> tags;
    };

    Result Add(std::uint32_t pre_seconds, std::uint32_t post_seconds, std::string_view tag,
               Ticks100ns now);

    [[nodiscard]] std::optional<Due> TakeDue(Ticks100ns now, std::uint32_t max_seconds);

    [[nodiscard]] bool pending() const { return pending_.has_value(); }

private:
    struct Pending {
        Ticks100ns start = 0;
        Ticks100ns deadline = 0;
        std::vector<std::string> tags;
    };

    std::optional<Pending> pending_;
    std::deque<Ticks100ns> saved_;
};

}
