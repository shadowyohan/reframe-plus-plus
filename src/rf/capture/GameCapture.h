#pragma once
#include <atomic>
#include <string>
#include <thread>

#include "rf/capture/IVideoCapture.h"
#include "rf/capture/hook/HookProtocol.h"

namespace rf {

class GameCapture final : public IVideoCapture {
public:
    explicit GameCapture(D3DDevicePtr device);
    ~GameCapture() override;

    Status Start(const CaptureTarget& target, const FrameCallback& on_frame) override;

    Status Attach(const CaptureTarget& target);
    [[nodiscard]] bool attached() const { return attached_; }
    void Stop() override;

    [[nodiscard]] CaptureStats stats() const override { return stats_; }
    [[nodiscard]] CaptureBackend backend() const override { return CaptureBackend::GameHook; }
    [[nodiscard]] std::uint32_t width() const override { return width_; }
    [[nodiscard]] std::uint32_t height() const override { return height_; }

    [[nodiscard]] std::string game_name() const { return game_name_; }

    void SetOverlay(std::uint32_t handle, std::uint32_t serial, std::uint32_t width,
                    std::uint32_t height, bool visible);

private:
    void ReaderLoop();

    bool RebindSharedTexture();
    void ReleaseIpc();

    D3DDevicePtr device_;
    FrameCallback on_frame_;

    std::uint32_t pid_ = 0;
    bool attached_ = false;
    std::string game_name_;

    void* section_ = nullptr;
    void* frame_event_ = nullptr;
    void* ready_event_ = nullptr;
    hook::SharedState* state_ = nullptr;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> shared_[hook::kSlots];
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
    std::uint32_t bound_serial_ = 0;

    std::thread reader_;
    std::atomic<bool> running_{false};

    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    CaptureStats stats_{};
    std::uint64_t last_frame_index_ = 0;
};

Status InjectHook(std::uint32_t pid, const std::wstring& dll_path);

Status HookDllPath(std::wstring& out_path);

std::uint32_t GameProcessForWindow(void* hwnd);

}
