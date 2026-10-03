#pragma once
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

#include "rf/core/Media.h"

namespace rf {

class ForegroundTracker {
public:
    static constexpr double kDominantShare = 0.6;

    void Record(Ticks100ns now, std::string app);
    [[nodiscard]] std::string Dominant(Ticks100ns from, Ticks100ns to) const;
    void Clear();

private:
    struct Run {
        Ticks100ns start = 0;
        std::string app;
    };

    mutable std::mutex mutex_;
    std::deque<Run> runs_;
};

class ForegroundAppNamer {
public:
    [[nodiscard]] std::string AppOn(void* monitor);

private:
    std::unordered_map<unsigned long, std::string> names_by_pid_;
};

}
