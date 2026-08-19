#pragma once
#include <atomic>
#include <mutex>
#include <thread>

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
    struct Api;

    void OutputLoop();
    void DestroySession();

    D3DDevicePtr device_;
    EncoderConfig config_{};
    PacketCallback on_packet_;
    ColorConverter converter_;
    CodecPrivate codec_private_;
    EncoderStats stats_{};

    std::unique_ptr<Api> api_;

    static constexpr int kInputTextures = 8;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> inputs_[kInputTextures];
    int next_input_ = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> last_input_;

    std::thread output_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> force_idr_{false};
    std::mutex submit_mutex_;
    Ticks100ns epoch_ = 0;
    bool open_ = false;
};

}
