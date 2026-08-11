#pragma once
#include "rf/encode/IVideoEncoder.h"
#include "rf/gpu/ColorConverter.h"

namespace rf {

class AmfEncoder final : public IVideoEncoder {
public:
    explicit AmfEncoder(D3DDevicePtr device);
    ~AmfEncoder() override;

    Status Open(const EncoderConfig& config, const PacketCallback& on_packet) override;
    void Close() override;
    Status Submit(const CapturedFrame& frame) override;
    Status RequestKeyframe() override;
    Status Flush() override;

    CodecPrivate codec_private() const override { return codec_private_; }
    EncoderStats stats() const override { return stats_; }
    EncoderBackend backend() const override { return EncoderBackend::Amf; }
    std::string name() const override { return "AMF (direct)"; }

    static bool Available();

private:
    D3DDevicePtr device_;
    EncoderConfig config_{};
    PacketCallback on_packet_;
    ColorConverter converter_;
    CodecPrivate codec_private_;
    EncoderStats stats_{};
};

}
