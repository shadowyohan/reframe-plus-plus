#pragma once
#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace rf::ui {

struct SpringParams {
    float k = 0.0f;
    float c = 0.0f;

    static constexpr SpringParams FromFigma(float a, float b, float window_seconds = 1.0f) {
        const float as = a / window_seconds;
        const float bs = b / window_seconds;
        return SpringParams{as * as + bs * bs, 2.0f * as};
    }
};

namespace spring {

inline constexpr SpringParams kQuick = SpringParams::FromFigma(7.4663f, 9.9792f, 0.5f);

inline constexpr SpringParams kMenu = SpringParams::FromFigma(7.5910f, 7.7443f, 0.4f);

inline constexpr SpringParams kExit = SpringParams::FromFigma(7.6657f, 6.7605f, 0.25f);

inline constexpr SpringParams kPill = SpringParams::FromFigma(7.3635f, 12.7540f, 0.5f);

inline constexpr SpringParams kBouncy = SpringParams::FromFigma(7.3635f, 12.7540f, 0.35f);

inline constexpr SpringParams kSoft = SpringParams::FromFigma(11.1803f, 0.1581f, 0.5f);

inline constexpr SpringParams kSavedThumb = SpringParams::FromFigma(7.4674f, 9.9565f, 0.79f);

inline constexpr SpringParams kArmedSpin = SpringParams::FromFigma(7.3635f, 12.7540f, 0.9f);
}

class Spring {
public:
    Spring() = default;
    explicit Spring(float initial) : value_(initial), target_(initial) {}

    void Reset(float v) {
        value_ = target_ = v;
        velocity_ = 0.0f;
    }

    void SetTarget(float t) { target_ = t; }

    float Update(float dt, const SpringParams& p) {

        constexpr float kMaxStep = 1.0f / 120.0f;
        int steps = static_cast<int>(dt / kMaxStep) + 1;
        if (steps > 16) steps = 16;
        const float h = dt / static_cast<float>(steps);

        for (int i = 0; i < steps; ++i) {
            const float accel = p.k * (target_ - value_) - p.c * velocity_;
            velocity_ += accel * h;
            value_ += velocity_ * h;
        }
        return value_;
    }

    [[nodiscard]] float value() const { return value_; }
    [[nodiscard]] float target() const { return target_; }
    [[nodiscard]] bool settled() const {
        return std::fabs(target_ - value_) < 0.001f && std::fabs(velocity_) < 0.01f;
    }

private:
    float value_ = 0.0f;
    float target_ = 0.0f;
    float velocity_ = 0.0f;
};

class AnimStore {
public:
    Spring& Get(std::uint32_t id, float initial = 0.0f) {
        auto [it, inserted] = springs_.try_emplace(id, Entry{Spring(initial), frame_});
        it->second.last_frame = frame_;
        return it->second.spring;
    }

    void BeginFrame() { ++frame_; }

    void Sweep() {
        for (auto it = springs_.begin(); it != springs_.end();)
            it = (frame_ - it->second.last_frame > 240) ? springs_.erase(it) : std::next(it);
    }

private:
    struct Entry {
        Spring spring;
        std::uint64_t last_frame = 0;
    };
    std::unordered_map<std::uint32_t, Entry> springs_;
    std::uint64_t frame_ = 0;
};

constexpr std::uint32_t HashId(const char* text, std::uint32_t seed = 0) {
    std::uint32_t hash = 2166136261u ^ seed;
    while (*text) {
        hash ^= static_cast<std::uint32_t>(static_cast<unsigned char>(*text++));
        hash *= 16777619u;
    }
    return hash;
}

inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }

}
