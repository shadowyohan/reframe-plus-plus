#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

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
    int PickDepth() const;
    void DestroySession();
    void OutputLoop();
    Status EncodeTexture(ID3D11Texture2D* input, Ticks100ns timestamp);

    D3DDevicePtr device_;
    EncoderConfig config_{};
    PacketCallback on_packet_;
    ColorConverter converter_;
    CodecPrivate codec_private_;
    EncoderStats stats_{};

    std::unique_ptr<Api> api_;

    std::thread output_thread_;
    std::atomic<bool> running_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<int> free_slots_;
    std::deque<int> inflight_;

    static constexpr int kMaxDepth = 8;
    static constexpr int kInputTextures = kMaxDepth + 1;
    int depth_ = kMaxDepth;
    int input_count_ = kInputTextures;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> inputs_[kInputTextures];
    int next_input_ = 0;
    ID3D11Texture2D* last_input_ = nullptr;

    bool direct_rgb_ = false;

    std::atomic<int> fail_streak_{0};
    std::atomic<bool> fatal_{false};
    std::atomic<bool> force_idr_{false};
    Ticks100ns epoch_ = 0;
    bool open_ = false;
};

}
