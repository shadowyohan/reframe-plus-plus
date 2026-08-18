#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "rf/core/Time.h"

namespace rf {

enum class Codec { H264, HEVC, AV1 };
enum class RateControl { CBR, VBR, ConstQp };

enum class ColorSpace { Rec709, Rec2020Pq };

const char* ToString(Codec c);
const char* ToString(ColorSpace cs);

enum class MediaKind : std::uint8_t { Video, Audio };

struct Packet {
    MediaKind kind = MediaKind::Video;
    std::vector<std::uint8_t> data;
    Ticks100ns pts = 0;
    Ticks100ns dts = 0;
    Ticks100ns duration = 0;
    bool keyframe = false;
    std::uint32_t track = 0;

    [[nodiscard]] std::size_t size() const noexcept { return data.size(); }
};

using PacketPtr = std::shared_ptr<const Packet>;

struct CodecPrivate {
    std::vector<std::uint8_t> data;
};

struct VideoFormat {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t fps_num = 60;
    std::uint32_t fps_den = 1;
    Codec codec = Codec::H264;
    ColorSpace color = ColorSpace::Rec709;
};

struct AudioFormat {
    std::uint32_t sample_rate = 48'000;
    std::uint32_t channels = 2;
    std::uint32_t bits = 32;
};

}
