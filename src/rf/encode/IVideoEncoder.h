#pragma once
#include <d3d11_4.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "rf/capture/IVideoCapture.h"
#include "rf/core/Media.h"
#include "rf/core/Status.h"
#include "rf/gpu/D3DDevice.h"
#include "rf/gpu/Quirks.h"

namespace rf {

enum class EncoderBackend {
    Auto,
    Nvenc,
    Amf,
    MediaFoundation
};

const char* ToString(EncoderBackend b);

struct EncoderConfig {
    VideoFormat format;
    RateControl rate_control = RateControl::CBR;
    std::uint32_t bitrate_kbps = 50'000;
    std::uint32_t max_bitrate_kbps = 0;
    std::uint32_t qp = 22;

    std::uint32_t keyframe_interval_ms = 2'000;

    std::uint32_t b_frames = 0;
    bool low_latency = true;
    bool async_encode = true;

    std::uint32_t async_depth = 12;

    std::uint32_t quality_vs_speed = 66;

    bool device_is_dedicated = false;

    std::uint32_t input_width = 0;
    std::uint32_t input_height = 0;

    Ticks100ns epoch = 0;

    QuirkSet quirks;
};

using PacketCallback = std::function<void(PacketPtr)>;

struct EncoderStats {
    std::uint64_t frames_submitted = 0;
    std::uint64_t frames_encoded = 0;
    std::uint64_t frames_dropped = 0;
    std::uint64_t bytes_out = 0;
    double avg_encode_ms = 0.0;
    double avg_queue_depth = 0.0;
};

class IVideoEncoder {
public:
    virtual ~IVideoEncoder() = default;

    virtual Status Open(const EncoderConfig& config, const PacketCallback& on_packet) = 0;
    virtual void Close() = 0;

    virtual Status Submit(const CapturedFrame& frame) = 0;

    virtual Status RequestKeyframe() = 0;

    virtual Status Flush() = 0;

    [[nodiscard]] virtual CodecPrivate codec_private() const = 0;
    [[nodiscard]] virtual EncoderStats stats() const = 0;
    [[nodiscard]] virtual EncoderBackend backend() const = 0;
    [[nodiscard]] virtual std::string name() const = 0;
};

using VideoEncoderPtr = std::unique_ptr<IVideoEncoder>;

struct EncoderCapability {
    EncoderBackend backend = EncoderBackend::MediaFoundation;
    Codec codec = Codec::H264;
    std::string name;
    bool hardware = false;
    bool supports_10bit = false;
    bool supports_bframes = false;
    std::uint32_t max_width = 0;
    std::uint32_t max_height = 0;
};

std::vector<EncoderCapability> ProbeEncoders(const D3DDevicePtr& device);

Status CreateVideoEncoder(const D3DDevicePtr& device, EncoderBackend backend,
                          VideoEncoderPtr& out);

}
