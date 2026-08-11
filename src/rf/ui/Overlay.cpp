#include "rf/ui/Overlay.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include "rf/core/Log.h"
#include "rf/core/Time.h"

#pragma comment(lib, "dcomp.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);

using Microsoft::WRL::ComPtr;

namespace rf::ui {
namespace {
constexpr const wchar_t* kClassName = L"ReframeOverlay";
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

            Resize(static_cast<UINT>(::GetSystemMetrics(SM_CXSCREEN)),
                   static_cast<UINT>(::GetSystemMetrics(SM_CYSCREEN)));
            ::SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, ::GetSystemMetrics(SM_CXSCREEN),
                           ::GetSystemMetrics(SM_CYSCREEN), SWP_NOACTIVATE);
            return 0;

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

    width_ = static_cast<UINT>(::GetSystemMetrics(SM_CXSCREEN));
    height_ = static_cast<UINT>(::GetSystemMetrics(SM_CYSCREEN));

    const DWORD ex_style =
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP | WS_EX_NOACTIVATE;

    hwnd_ = ::CreateWindowExW(ex_style, kClassName, L"Reframe Overlay", WS_POPUP, 0, 0,
                              static_cast<int>(width_), static_cast<int>(height_), nullptr, nullptr,
                              instance_, this);
    if (!hwnd_) return Status::Fail(HRESULT_FROM_WIN32(::GetLastError()), "CreateWindowEx");

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
    if (interactive_) {
        style &= ~static_cast<LONG_PTR>(WS_EX_NOACTIVATE);
    } else {
        style |= static_cast<LONG_PTR>(WS_EX_NOACTIVATE);
    }
    ::SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, style);
    ::SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED |
                       (interactive_ ? 0 : SWP_NOACTIVATE));
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

        HRGN piece = ::CreateRectRgn(static_cast<int>(r.x) - 2, static_cast<int>(r.y) - 2,
                                     static_cast<int>(r.z) + 2, static_cast<int>(r.w) + 2);
        ::CombineRgn(combined, combined, piece, RGN_OR);
        ::DeleteObject(piece);
    }
    ::SetWindowRgn(hwnd_, combined, TRUE);
}

void Overlay::SetVisible(bool visible) {
    if (visible_ == visible) return;
    visible_ = visible;
    ::ShowWindow(hwnd_, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
}

void Overlay::SetClickable(bool clickable) {
    if (clickable_ == clickable) return;
    clickable_ = clickable;
    if (!interactive_) ApplyInputStyle();
}

void Overlay::SetInteractive(bool interactive) {
    if (interactive_ == interactive) return;
    interactive_ = interactive;
    ApplyInputStyle();

    if (interactive) {

        const DWORD self = ::GetCurrentThreadId();
        const HWND foreground_window = ::GetForegroundWindow();
        DWORD foreground = ::GetWindowThreadProcessId(foreground_window, nullptr);

        if (foreground && foreground != self) {
            DWORD_PTR result = 0;
            if (!::SendMessageTimeoutW(foreground_window, WM_NULL, 0, 0,
                                       SMTO_ABORTIFHUNG | SMTO_BLOCK, 200, &result)) {
                RF_WARN("foreground window is not responding - not attaching input to it");
                foreground = 0;
            }
        }

        if (foreground && foreground != self) ::AttachThreadInput(self, foreground, TRUE);
        ::SetForegroundWindow(hwnd_);
        ::SetActiveWindow(hwnd_);
        ::SetFocus(hwnd_);
        if (foreground && foreground != self) ::AttachThreadInput(self, foreground, FALSE);
    }
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

    if (interactive_ || clickable_) {
        POINT cursor{};
        if (::GetCursorPos(&cursor) && ::ScreenToClient(hwnd_, &cursor))
            ImGui::GetIO().AddMousePosEvent(static_cast<float>(cursor.x),
                                            static_cast<float>(cursor.y));
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
    ctx_.mouse = io.MousePos;
    ctx_.mouse_down = io.MouseDown[0];
    ctx_.mouse_pressed = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    ctx_.mouse_released = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    ctx_.wheel = io.MouseWheel;
    ctx_.interactive = interactive_ || clickable_;
    ctx_.alpha = 1.0f;
    return &ctx_;
}

void Overlay::EndFrame() {
    ImGui::Render();

    constexpr float kClear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    ID3D11RenderTargetView* rtv = rtv_.Get();
    {
        D3DDevice::ContextLock lock(*device_);
        device_->context()->OMSetRenderTargets(1, &rtv, nullptr);
        device_->context()->ClearRenderTargetView(rtv, kClear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

    const HRESULT presented = swap_chain_->Present(0, 0);
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
