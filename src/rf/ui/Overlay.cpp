#include "rf/ui/Overlay.h"

#include <algorithm>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include "rf/capture/hook/HookProtocol.h"
#include "rf/core/Log.h"
#include "rf/core/Time.h"

#pragma comment(lib, "dcomp.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);

using Microsoft::WRL::ComPtr;

namespace rf::ui {
namespace {

bool SystemCursorVisible() {
    CURSORINFO info{sizeof(info)};
    return ::GetCursorInfo(&info) && (info.flags & CURSOR_SHOWING) != 0 && info.hCursor;
}

constexpr const wchar_t* kClassName = L"ReframeOverlay";

constexpr int kNotFullscreenMargin = 1;

SIZE OverlaySize() {
    return {::GetSystemMetrics(SM_CXSCREEN),
            ::GetSystemMetrics(SM_CYSCREEN) - kNotFullscreenMargin};
}
}

Overlay::~Overlay() { Destroy(); }

LRESULT CALLBACK Overlay::WndProcThunk(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<Overlay*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    return self ? self->WndProc(hwnd, msg, wparam, lparam)
                : ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT Overlay::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (imgui_ready_ && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) return 1;

    switch (msg) {
        case WM_SIZE:
            if (wparam != SIZE_MINIMIZED) Resize(LOWORD(lparam), HIWORD(lparam));
            return 0;

        case WM_DPICHANGED:
            dpi_scale_ = static_cast<float>(HIWORD(wparam)) / 96.0f;
            RF_INFO("overlay DPI changed to {:.2f}x", dpi_scale_);
            return 0;

        case WM_NCHITTEST:

            if (!interactive_ && !clickable_) return HTTRANSPARENT;
            return ::DefWindowProcW(hwnd, msg, wparam, lparam);

        case WM_HOTKEY:
            if (on_hotkey) on_hotkey(static_cast<int>(wparam));
            return 0;

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            if (!interactive_) return ::DefWindowProcW(hwnd, msg, wparam, lparam);

            if (on_key) {
                std::uint32_t mods = 0;
                if (::GetKeyState(VK_CONTROL) < 0) mods |= MOD_CONTROL;
                if (::GetKeyState(VK_MENU) < 0) mods |= MOD_ALT;
                if (::GetKeyState(VK_SHIFT) < 0) mods |= MOD_SHIFT;
                if (on_key(static_cast<std::uint32_t>(wparam), mods)) return 0;
            }

            if (wparam == VK_ESCAPE && on_escape) {
                on_escape();
                return 0;
            }
            return ::DefWindowProcW(hwnd, msg, wparam, lparam);
        }

        case WM_DISPLAYCHANGE:

        {
            const SIZE size = OverlaySize();
            Resize(static_cast<UINT>(size.cx), static_cast<UINT>(size.cy));
            ::SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, size.cx, size.cy, SWP_NOACTIVATE);
            return 0;
        }

        case WM_CLOSE:
            RF_WARN("overlay received WM_CLOSE");
            quit_ = true;
            return 0;

        case WM_DESTROY:
            RF_WARN("overlay window destroyed");
            ::PostQuitMessage(0);
            return 0;

        case WM_ENDSESSION:
            RF_WARN("overlay received WM_ENDSESSION");
            return 0;

        default:
            return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

Status Overlay::RebindDevice(const D3DDevicePtr& device) {
    RF_INFO("overlay rebinding to a fresh D3D11 device");

    if (imgui_ready_) ImGui_ImplDX11_Shutdown();
    icons_.Release();
    textures_.Release();
    ReleaseRenderTarget();
    dcomp_visual_.Reset();
    dcomp_target_.Reset();
    dcomp_.Reset();
    swap_chain_.Reset();

    device_ = device;
    device_lost_ = false;

    RF_TRY(CreateSwapChain());
    RF_TRY(CreateRenderTarget());

    if (auto s = icons_.Load(device_->device(), asset_dir_ / "icons", dpi_scale_); !s.ok())
        RF_WARN("icons: {}", s.str());
    textures_.Init(device_->device());

    ImGui_ImplDX11_Init(device_->device(), device_->context());
    return Status::Ok();
}

Status Overlay::Create(const D3DDevicePtr& device, const std::filesystem::path& asset_dir) {
    device_ = device;
    asset_dir_ = asset_dir;
    instance_ = ::GetModuleHandleW(nullptr);

    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProcThunk;
    wc.hInstance = instance_;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    ::RegisterClassExW(&wc);

    const SIZE size = OverlaySize();
    width_ = static_cast<UINT>(size.cx);
    height_ = static_cast<UINT>(size.cy);

    const DWORD ex_style =
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP | WS_EX_NOACTIVATE;

    hwnd_ = ::CreateWindowExW(ex_style, kClassName, L"Reframe Overlay", WS_POPUP, 0, 0,
                              static_cast<int>(width_), static_cast<int>(height_), nullptr, nullptr,
                              instance_, this);
    if (!hwnd_) return Status::Fail(HRESULT_FROM_WIN32(::GetLastError()), "CreateWindowEx");
    ::SetPropW(hwnd_, L"NonRudeHWND", reinterpret_cast<HANDLE>(TRUE));

    dpi_scale_ = static_cast<float>(::GetDpiForWindow(hwnd_)) / 96.0f;

    RF_TRY(CreateSwapChain());
    RF_TRY(CreateRenderTarget());

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    fonts_.Load(io, asset_dir / "fonts", dpi_scale_);
    if (auto s = icons_.Load(device_->device(), asset_dir / "icons", dpi_scale_); !s.ok())
        RF_WARN("icons: {}", s.str());
    textures_.Init(device_->device());

    ImGui_ImplWin32_Init(hwnd_);
    ImGui_ImplDX11_Init(device_->device(), device_->context());
    imgui_ready_ = true;

    ::ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    last_tick_ = QpcNow();

    RF_INFO("overlay up: {}x{} at {:.2f}x DPI", width_, height_, dpi_scale_);
    return Status::Ok();
}

Status Overlay::CreateSwapChain() {
    ComPtr<IDXGIDevice> dxgi_device;
    RF_HR(device_->device()->QueryInterface(IID_PPV_ARGS(&dxgi_device)));

    ComPtr<IDXGIAdapter> adapter;
    RF_HR(dxgi_device->GetAdapter(&adapter));
    ComPtr<IDXGIFactory2> factory;
    RF_HR(adapter->GetParent(IID_PPV_ARGS(&factory)));

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width_;
    desc.Height = height_;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;

    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

    RF_HR(factory->CreateSwapChainForComposition(device_->device(), &desc, nullptr, &swap_chain_));

    RF_HR(::DCompositionCreateDevice(dxgi_device.Get(), IID_PPV_ARGS(&dcomp_)));
    RF_HR(dcomp_->CreateTargetForHwnd(hwnd_, TRUE, &dcomp_target_));
    RF_HR(dcomp_->CreateVisual(&dcomp_visual_));
    RF_HR(dcomp_visual_->SetContent(swap_chain_.Get()));
    RF_HR(dcomp_target_->SetRoot(dcomp_visual_.Get()));
    RF_HR(dcomp_->Commit());
    return Status::Ok();
}

Status Overlay::CreateRenderTarget() {
    ComPtr<ID3D11Texture2D> back_buffer;
    RF_HR(swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer)));
    RF_HR(device_->device()->CreateRenderTargetView(back_buffer.Get(), nullptr, &rtv_));
    return Status::Ok();
}

void Overlay::ReleaseRenderTarget() { rtv_.Reset(); }

void Overlay::Resize(UINT width, UINT height) {
    if (!swap_chain_ || width == 0 || height == 0) return;
    if (width == width_ && height == height_) return;

    width_ = width;
    height_ = height;
    ReleaseRenderTarget();
    swap_chain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (auto s = CreateRenderTarget(); !s.ok()) RF_ERROR("overlay resize: {}", s.str());
}

void Overlay::ApplyInputStyle() {

    LONG_PTR style = ::GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    style |= static_cast<LONG_PTR>(WS_EX_NOACTIVATE);
    ::SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, style);
    ::SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_NOACTIVATE);
}

void Overlay::SetShape(const std::vector<ImVec4>& rects) {
    if (rects == shape_) return;
    shape_ = rects;

    if (rects.empty()) {
        ::SetWindowRgn(hwnd_, nullptr, TRUE);
        return;
    }

    HRGN combined = ::CreateRectRgn(0, 0, 0, 0);
    for (const ImVec4& r : rects) {

        HRGN piece = ::CreateRectRgn(static_cast<int>(r.x * ui_scale_) - 2,
                                     static_cast<int>(r.y * ui_scale_) - 2,
                                     static_cast<int>(r.z * ui_scale_) + 2,
                                     static_cast<int>(r.w * ui_scale_) + 2);
        ::CombineRgn(combined, combined, piece, RGN_OR);
        ::DeleteObject(piece);
    }
    ::SetWindowRgn(hwnd_, combined, TRUE);
}

void Overlay::SetUiScale(float scale) {
    const float wanted = std::clamp(scale, 0.5f, 2.0f);
    if (wanted == ui_scale_) return;
    ui_scale_ = wanted;

    shape_.clear();
    ReloadAssets();
}

void Overlay::ReloadAssets() {
    if (!imgui_ready_ || !device_) return;

    const float raster = dpi_scale_ * ui_scale_;

    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    fonts_.Load(io, asset_dir_ / "fonts", raster);
    ImGui_ImplDX11_InvalidateDeviceObjects();

    if (auto s = icons_.Load(device_->device(), asset_dir_ / "icons", raster); !s.ok())
        RF_WARN("icons: {}", s.str());

    RF_INFO("interface redrawn at {:.0f}% ({:.2f}x pixels)", ui_scale_ * 100.0f, raster);
}

void Overlay::SetVisible(bool visible) {
    if (visible_ == visible) return;
    visible_ = visible;
    ::ShowWindow(hwnd_, visible ? SW_SHOWNOACTIVATE : SW_HIDE);

    if (visible) input_settle_ = 2;
}

void Overlay::SetClickable(bool clickable) {
    if (clickable_ == clickable) return;
    clickable_ = clickable;
    if (!interactive_) ApplyInputStyle();
}

void Overlay::BringToTop() {
    if (!hwnd_) return;
    ::SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER |
                       SWP_ASYNCWINDOWPOS);
}

void Overlay::SetInteractive(bool interactive) {
    if (interactive_ == interactive) return;
    interactive_ = interactive;
    ApplyInputStyle();
    if (interactive) BringToTop();

    RF_DEBUG("overlay interactive = {}", interactive);
}

bool Overlay::PumpMessages() {
    MSG msg;
    while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
        if (msg.message == WM_QUIT) quit_ = true;
    }
    return !quit_;
}

UiContext* Overlay::BeginFrame() {
    if (!rtv_ || width_ == 0 || height_ == 0 || device_lost_) return nullptr;

    if (const HRESULT reason = device_->DeviceRemovedReason(); reason != S_OK) {
        RF_ERROR("D3D device lost: {} (0x{:08X})", D3DDevice::DescribeRemovedReason(reason),
                 static_cast<unsigned>(reason));
        device_lost_ = true;
        return nullptr;
    }

    const std::int64_t now = QpcNow();
    const float dt = static_cast<float>(static_cast<double>(now - last_tick_) /
                                        static_cast<double>(QpcFrequency()));
    last_tick_ = now;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();

    ImGui::GetIO().DisplaySize =
        ImVec2(static_cast<float>(width_) / ui_scale_, static_cast<float>(height_) / ui_scale_);

    if (interactive_ || clickable_) {
        POINT cursor{};
        if (::GetCursorPos(&cursor) && ::ScreenToClient(hwnd_, &cursor))
            ImGui::GetIO().AddMousePosEvent(static_cast<float>(cursor.x) / ui_scale_,
                                            static_cast<float>(cursor.y) / ui_scale_);
    }

    ImGui::NewFrame();

    anim_.BeginFrame();

    ImGuiIO& io = ImGui::GetIO();
    ctx_ = UiContext{};
    ctx_.dl = ImGui::GetBackgroundDrawList();
    ctx_.anim = &anim_;
    ctx_.icons = &icons_;
    ctx_.fonts = &fonts_;
    ctx_.dt = (dt > 0.0f && dt < 0.5f) ? dt : 1.0f / 60.0f;
    ctx_.dpi = dpi_scale_;
    ctx_.font_scale = 1.0f / ui_scale_;
    if (external_mouse_) {

        ctx_.mouse = ImVec2(external_pos_.x / ui_scale_, external_pos_.y / ui_scale_);
        ctx_.mouse_down = external_down_;
        ctx_.mouse_pressed = external_down_ && !external_down_prev_;
        ctx_.mouse_released = !external_down_ && external_down_prev_;
        ctx_.wheel = external_wheel_;
        external_down_prev_ = external_down_;
        external_wheel_ = 0.0f;
    } else {
        ctx_.mouse = io.MousePos;
        ctx_.mouse_down = io.MouseDown[0];
        ctx_.mouse_pressed = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        ctx_.mouse_released = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
        ctx_.wheel = io.MouseWheel;
        external_down_prev_ = false;
    }

    ctx_.interactive = interactive_ || clickable_ || external_mouse_;
    if (input_settle_ > 0) {
        --input_settle_;
        ctx_.mouse_down = false;
        ctx_.mouse_pressed = false;
        ctx_.mouse_released = false;
        ctx_.wheel = 0.0f;
    }

    if (!ctx_.mouse_down && !ctx_.mouse_released) active_widget_ = 0;
    ctx_.active_id = active_widget_;
    ctx_.alpha = 1.0f;
    return &ctx_;
}

Status Overlay::SetMirrorToSharedSurface(bool enabled) {
    if (enabled == mirror_enabled_) return Status::Ok();

    if (!enabled) {
        shared_rtv_.Reset();
        shared_texture_.Reset();
        shared_handle_ = 0;
        mirror_enabled_ = false;
        return Status::Ok();
    }

    if (width_ == 0 || height_ == 0) return Status::Fail("overlay has no size yet");

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width_;
    desc.Height = height_;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    RF_HR(device_->device()->CreateTexture2D(&desc, nullptr, &texture));
    RF_HR(device_->device()->CreateRenderTargetView(texture.Get(), nullptr, &shared_rtv_));

    Microsoft::WRL::ComPtr<IDXGIResource> resource;
    RF_HR(texture.As(&resource));
    HANDLE handle = nullptr;
    RF_HR(resource->GetSharedHandle(&handle));

    shared_texture_ = std::move(texture);
    shared_handle_ = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(handle));
    ++shared_serial_;
    mirror_enabled_ = true;
    RF_INFO("overlay mirror up: {}x{} handle 0x{:x}", width_, height_, shared_handle_);
    return Status::Ok();
}

void Overlay::SetExternalMouse(bool active, ImVec2 pos, bool down, float wheel) {
    external_mouse_ = active;
    external_pos_ = pos;
    external_down_ = down;
    external_wheel_ += wheel;
}

void Overlay::EndFrame() {
    active_widget_ = ctx_.active_id;

    if (external_mouse_ && !SystemCursorVisible()) {
        const ImVec2 p = ImVec2(external_pos_.x / ui_scale_, external_pos_.y / ui_scale_);
        const float s = external_down_ ? 0.9f : 1.0f;
        const ImVec2 tip(p.x, p.y);
        const ImVec2 tail(p.x + 11.0f * s, p.y + 15.0f * s);
        const ImVec2 side(p.x + 2.5f * s, p.y + 17.0f * s);

        ImDrawList* dl = ImGui::GetForegroundDrawList();

        dl->AddTriangleFilled(ImVec2(tip.x - 1.5f, tip.y - 1.5f),
                              ImVec2(tail.x + 1.5f, tail.y + 1.5f),
                              ImVec2(side.x - 1.5f, side.y + 1.5f), IM_COL32(0, 0, 0, 190));
        dl->AddTriangleFilled(tip, tail, side, IM_COL32(255, 255, 255, 255));
    }

    ImGui::Render();

    constexpr float kClear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    ID3D11RenderTargetView* rtv = rtv_.Get();
    {
        D3DDevice::ContextLock lock(*device_);
        device_->context()->OMSetRenderTargets(1, &rtv, nullptr);
        device_->context()->ClearRenderTargetView(rtv, kClear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

    if (mirror_enabled_ && shared_rtv_) {
        D3DDevice::ContextLock lock(*device_);
        ID3D11RenderTargetView* mirror = shared_rtv_.Get();
        device_->context()->OMSetRenderTargets(1, &mirror, nullptr);
        device_->context()->ClearRenderTargetView(mirror, kClear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        device_->context()->Flush();
    }

    const HRESULT presented = swap_chain_->Present(1, 0);
    if (presented == DXGI_ERROR_DEVICE_REMOVED || presented == DXGI_ERROR_DEVICE_RESET) {
        RF_ERROR("Present reported the device is gone (0x{:08X})",
                 static_cast<unsigned>(presented));
        device_lost_ = true;
    } else if (FAILED(presented)) {
        RF_WARN("Present failed 0x{:08X}", static_cast<unsigned>(presented));
    }
    anim_.Sweep();
}

void Overlay::Destroy() {
    if (imgui_ready_) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        imgui_ready_ = false;
    }
    icons_.Release();
    textures_.Release();
    ReleaseRenderTarget();
    dcomp_visual_.Reset();
    dcomp_target_.Reset();
    dcomp_.Reset();
    swap_chain_.Reset();

    if (hwnd_) {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    ::UnregisterClassW(kClassName, instance_);
    device_.reset();
}

}
