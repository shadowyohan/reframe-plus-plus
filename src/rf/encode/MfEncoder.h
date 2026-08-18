#pragma once
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>

#include <atomic>
#include <mutex>
#include <thread>

#include "rf/encode/IVideoEncoder.h"
#include "rf/gpu/ColorConverter.h"

namespace rf {

class MfEncoder final : public IVideoEncoder {
public:
    explicit MfEncoder(D3DDevicePtr device);
    ~MfEncoder() override;

    Status Open(const EncoderConfig& config, const PacketCallback& on_packet) override;
    void Close() override;
    Status Submit(const CapturedFrame& frame) override;
    Status RequestKeyframe() override;
    Status Flush() override;

    CodecPrivate codec_private() const override { return codec_private_; }
    EncoderStats stats() const override { return stats_; }
    EncoderBackend backend() const override { return EncoderBackend::MediaFoundation; }
    std::string name() const override { return name_; }

private:
    Status SelectTransform();
    Status ConfigureTypes();
    Status BindD3DManager();
    Status StartEventLoop();
    void EventLoop();
    Status FeedSample(IMFSample* sample);
    Status DrainOutput();

    Status SubmitSurface(ID3D11Texture2D* nv12, Ticks100ns timestamp);

    D3DDevicePtr device_;
    EncoderConfig config_{};
    PacketCallback on_packet_;
    ColorConverter converter_;

    Microsoft::WRL::ComPtr<IMFTransform> transform_;
    Microsoft::WRL::ComPtr<IMFMediaEventGenerator> events_;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> dxgi_manager_;
    UINT dxgi_reset_token_ = 0;
    DWORD input_stream_ = 0;
    DWORD output_stream_ = 0;

    std::thread event_thread_;
    std::atomic<bool> running_{false};
    std::atomic<int> need_input_{0};
    std::atomic<bool> force_keyframe_{false};

    std::atomic<bool> draining_{false};

    std::mutex pending_mutex_;
    std::vector<Microsoft::WRL::ComPtr<IMFSample>> pending_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> last_nv12_;
    CodecPrivate codec_private_;
    EncoderStats stats_{};
    std::string name_ = "Media Foundation";
    Ticks100ns first_pts_ = -1;
    bool mf_started_ = false;
};

}
