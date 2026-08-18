#pragma once
#include <d3d11_4.h>

#include <atomic>
#include <filesystem>
#include <mutex>

#include <wrl/client.h>

#include "rf/core/Status.h"
#include "rf/gpu/D3DDevice.h"

struct IMFMediaEngine;
struct IMFDXGIDeviceManager;

namespace rf {

class VideoPlayer {
public:
    VideoPlayer();
    ~VideoPlayer();

    VideoPlayer(const VideoPlayer&) = delete;
    VideoPlayer& operator=(const VideoPlayer&) = delete;

    Status Init(const D3DDevicePtr& device);
    void Shutdown();

    Status Open(const std::filesystem::path& file);
    void Close();

    void Play();
    void Pause();
    void TogglePlay();
    void Seek(double seconds);
    void SetVolume(float volume);

    bool Update();

    [[nodiscard]] ID3D11ShaderResourceView* frame() const { return srv_.Get(); }
    [[nodiscard]] std::uint32_t width() const { return width_; }
    [[nodiscard]] std::uint32_t height() const { return height_; }

    [[nodiscard]] bool open() const { return open_; }
    [[nodiscard]] bool ready() const { return ready_.load(std::memory_order_acquire); }
    [[nodiscard]] bool playing() const;
    [[nodiscard]] double position() const;
    [[nodiscard]] double duration() const;
    [[nodiscard]] float volume() const { return volume_; }
    [[nodiscard]] const std::filesystem::path& file() const { return file_; }
    [[nodiscard]] const std::string& error() const { return error_; }

private:
    friend class MediaEngineNotify;

    void SelectEveryAudioStream();
    void OnEngineEvent(std::uint32_t event);
    Status EnsureTexture();

    D3DDevicePtr device_;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> dxgi_manager_;
    Microsoft::WRL::ComPtr<IMFMediaEngine> engine_;
    Microsoft::WRL::ComPtr<IUnknown> notify_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;

    std::filesystem::path file_;
    std::string error_;
    bool open_ = false;
    bool mf_started_ = false;
    float volume_ = 1.0f;

    std::atomic<bool> ready_{false};
    std::atomic<bool> failed_{false};
    std::atomic<bool> ended_{false};

    std::atomic<bool> size_known_{false};
};

}
