#pragma once
#include <windows.h>

#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_6.h>

#include <functional>
#include <memory>

#include <wrl/client.h>

#include "rf/core/Status.h"
#include "rf/gpu/D3DDevice.h"
#include "rf/ui/Assets.h"
#include "rf/ui/Widgets.h"

namespace rf::ui {

class Overlay {
public:
    ~Overlay();

    Status Create(const D3DDevicePtr& device, const std::filesystem::path& asset_dir);
    void Destroy();

    bool PumpMessages();

    UiContext* BeginFrame();
    void EndFrame();

    void SetUiScale(float scale);

    void SetInteractive(bool interactive);

    void BringToTop();
    [[nodiscard]] bool interactive() const { return interactive_; }

    void SetClickable(bool clickable);

    void SetVisible(bool visible);

    void SetShape(const std::vector<ImVec4>& rects);

    Status SetMirrorToSharedSurface(bool enabled);

    void SetExternalMouse(bool active, ImVec2 pos, bool down, float wheel);
    [[nodiscard]] std::uint32_t width() const { return width_; }
    [[nodiscard]] std::uint32_t height() const { return height_; }
    [[nodiscard]] std::uint32_t shared_surface_handle() const { return shared_handle_; }
    [[nodiscard]] std::uint32_t shared_surface_serial() const { return shared_serial_; }

    Status RebindDevice(const D3DDevicePtr& device);

    [[nodiscard]] bool device_lost() const { return device_lost_; }

    [[nodiscard]] HWND hwnd() const { return hwnd_; }
    [[nodiscard]] float ui_scale() const { return ui_scale_; }
    [[nodiscard]] ImVec2 design_size() const {
        return ImVec2(static_cast<float>(width_) / ui_scale_, static_cast<float>(height_) / ui_scale_);
    }
    [[nodiscard]] ImVec2 size() const { return ImVec2(static_cast<float>(width_),
                                                      static_cast<float>(height_)); }
    [[nodiscard]] float dpi_scale() const { return dpi_scale_; }
    [[nodiscard]] const IconSet& icons() const { return icons_; }
    [[nodiscard]] const FontSet& fonts() const { return fonts_; }
    TextureCache& textures() { return textures_; }

    std::function<void(int)> on_hotkey;

    std::function<void()> on_escape;

    std::function<bool(std::uint32_t vk, std::uint32_t mods)> on_key;

private:
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

    Status CreateSwapChain();
    void ReleaseRenderTarget();
    Status CreateRenderTarget();
    void Resize(UINT width, UINT height);

    D3DDevicePtr device_;
    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;

    Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_chain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> shared_texture_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> shared_rtv_;
    std::uint32_t shared_handle_ = 0;
    std::uint32_t shared_serial_ = 0;
    bool mirror_enabled_ = false;

    float ui_scale_ = 1.0f;
    int input_settle_ = 0;

    std::uint32_t active_widget_ = 0;
    bool external_mouse_ = false;
    ImVec2 external_pos_{};
    bool external_down_ = false;
    bool external_down_prev_ = false;
    float external_wheel_ = 0.0f;
    Microsoft::WRL::ComPtr<IDCompositionDevice> dcomp_;
    Microsoft::WRL::ComPtr<IDCompositionTarget> dcomp_target_;
    Microsoft::WRL::ComPtr<IDCompositionVisual> dcomp_visual_;

    IconSet icons_;
    FontSet fonts_;
    TextureCache textures_;
    AnimStore anim_;
    UiContext ctx_{};

    void ApplyInputStyle();

    UINT width_ = 0, height_ = 0;
    float dpi_scale_ = 1.0f;
    bool interactive_ = false;
    bool clickable_ = false;
    bool visible_ = true;
    bool device_lost_ = false;
    std::vector<ImVec4> shape_;
    std::filesystem::path asset_dir_;
    bool quit_ = false;
    bool imgui_ready_ = false;
    std::int64_t last_tick_ = 0;
};

}
