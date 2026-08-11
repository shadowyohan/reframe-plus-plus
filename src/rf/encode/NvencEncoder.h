#pragma once
#include <atomic>
#include <mutex>

#include "rf/encode/IVideoEncoder.h"
#include "rf/gpu/ColorConverter.h"

namespace rf {

class NvencEncoder final : public IVideoEncoder {
public:
    explicit NvencEncoder(D3DDevicePtr device);
    ~NvencEncoder() override;

    Status Open(const EncoderConfig& config, const PacketCallback& on_packet) override;
    void Close() override;
    Status Submit(const CapturedFrame& frame) override;
    Status RequestKeyframe() override;
    Status Flush() override;

    CodecPrivate codec_private() const override { return codec_private_; }
    EncoderStats stats() const override { return stats_; }
    EncoderBackend backend() const override { return EncoderBackend::Nvenc; }
    std::string name() const override { return "NVENC (direct)"; }

    static bool Available();

private:
    struct Api;

    Status InitSession();
    void DestroySession();

    D3DDevicePtr device_;
    EncoderConfig config_{};
    PacketCallback on_packet_;
    ColorConverter converter_;
    CodecPrivate codec_private_;
    EncoderStats stats_{};

    Status DrainSlot(int slot);

    std::unique_ptr<Api> api_;

    std::mutex encode_mutex_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> input_[2];
    int parity_ = 0;
    bool pending_ = false;
    std::atomic<bool> force_idr_{false};
    Ticks100ns epoch_ = 0;
    bool open_ = false;
    bool have_input_ = false;
};

}
