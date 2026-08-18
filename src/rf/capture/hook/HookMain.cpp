
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>

#include <atomic>
#include <cstdio>

#include <MinHook.h>

#include "HookGl.h"
#include "HookProtocol.h"
#include "OverlayQuad_ps.h"
#include "OverlayQuad_vs.h"

using namespace rf::hook;

namespace {

HMODULE g_self = nullptr;
HANDLE g_section = nullptr;
SharedState* g_state = nullptr;
HANDLE g_frame_event = nullptr;
HANDLE g_ready_event = nullptr;
std::atomic<bool> g_shutting_down{false};

CRITICAL_SECTION g_lock;

struct Capture {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11Texture2D* shared[kSlots] = {};
    std::uint32_t next_slot = 0;
    IUnknown* swapchain = nullptr;
    UINT width = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool live = false;
} g_cap;

LARGE_INTEGER g_qpc_freq{};

void Log(const char* fmt, ...);

struct OverlayDraw {
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11BlendState* blend = nullptr;
    ID3D11SamplerState* sampler = nullptr;
    ID3D11RasterizerState* raster = nullptr;
    ID3D11DepthStencilState* depth = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11Texture2D* rtv_source = nullptr;
    std::uint32_t serial = 0;
} g_overlay;

void ReleaseOverlayTexture() {
    if (g_overlay.srv) g_overlay.srv->Release();
    if (g_overlay.texture) g_overlay.texture->Release();
    g_overlay.srv = nullptr;
    g_overlay.texture = nullptr;
    g_overlay.serial = 0;
}

void ReleaseOverlay() {
    ReleaseOverlayTexture();
    if (g_overlay.rtv) g_overlay.rtv->Release();
    if (g_overlay.depth) g_overlay.depth->Release();
    if (g_overlay.raster) g_overlay.raster->Release();
    if (g_overlay.sampler) g_overlay.sampler->Release();
    if (g_overlay.blend) g_overlay.blend->Release();
    if (g_overlay.ps) g_overlay.ps->Release();
    if (g_overlay.vs) g_overlay.vs->Release();
    g_overlay = OverlayDraw{};
}

bool EnsureOverlayPipeline(ID3D11Device* device) {
    if (g_overlay.vs) return true;

    if (FAILED(device->CreateVertexShader(kOverlayQuad_vs, sizeof(kOverlayQuad_vs), nullptr,
                                          &g_overlay.vs)) ||
        FAILED(device->CreatePixelShader(kOverlayQuad_ps, sizeof(kOverlayQuad_ps), nullptr,
                                         &g_overlay.ps))) {
        Log("failed to create the overlay shaders");
        return false;
    }

    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device->CreateBlendState(&blend, &g_overlay.blend);

    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    device->CreateSamplerState(&sampler, &g_overlay.sampler);

    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    device->CreateRasterizerState(&raster, &g_overlay.raster);

    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = FALSE;
    depth.StencilEnable = FALSE;
    device->CreateDepthStencilState(&depth, &g_overlay.depth);

    return g_overlay.blend && g_overlay.sampler && g_overlay.raster && g_overlay.depth;
}

bool EnsureOverlayTexture(ID3D11Device* device) {
    if (g_overlay.texture && g_overlay.serial == g_state->overlay_serial) return true;

    ReleaseOverlayTexture();
    const HANDLE handle =
        reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(g_state->overlay_handle));
    if (!handle) return false;

    if (FAILED(device->OpenSharedResource(handle, __uuidof(ID3D11Texture2D),
                                          (void**)&g_overlay.texture))) {
        Log("failed to open the overlay texture");
        return false;
    }
    device->CreateShaderResourceView(g_overlay.texture, nullptr, &g_overlay.srv);
    if (!g_overlay.srv) {
        ReleaseOverlayTexture();
        return false;
    }

    g_overlay.serial = g_state->overlay_serial;
    Log("overlay texture bound (%ux%u)", g_state->overlay_width, g_state->overlay_height);
    return true;
}

void DrawOverlayLocked(ID3D11Texture2D* backbuffer) {
    if (!g_state->overlay_visible || !g_state->overlay_handle) return;

    ID3D11Device* device = g_cap.device;
    ID3D11DeviceContext* ctx = g_cap.context;
    if (!device || !ctx || !backbuffer) return;
    if (!EnsureOverlayPipeline(device) || !EnsureOverlayTexture(device)) return;

    if (g_overlay.rtv_source != backbuffer) {
        if (g_overlay.rtv) g_overlay.rtv->Release();
        g_overlay.rtv = nullptr;
        if (FAILED(device->CreateRenderTargetView(backbuffer, nullptr, &g_overlay.rtv))) return;
        g_overlay.rtv_source = backbuffer;
    }

    D3D11_TEXTURE2D_DESC back_desc{};
    backbuffer->GetDesc(&back_desc);

    ID3D11RenderTargetView* saved_rtv = nullptr;
    ID3D11DepthStencilView* saved_dsv = nullptr;
    ctx->OMGetRenderTargets(1, &saved_rtv, &saved_dsv);
    ID3D11BlendState* saved_blend = nullptr;
    FLOAT saved_factor[4]{};
    UINT saved_mask = 0;
    ctx->OMGetBlendState(&saved_blend, saved_factor, &saved_mask);
    ID3D11DepthStencilState* saved_depth = nullptr;
    UINT saved_stencil = 0;
    ctx->OMGetDepthStencilState(&saved_depth, &saved_stencil);
    ID3D11RasterizerState* saved_raster = nullptr;
    ctx->RSGetState(&saved_raster);
    UINT viewport_count = 1;
    D3D11_VIEWPORT saved_viewport{};
    ctx->RSGetViewports(&viewport_count, &saved_viewport);
    ID3D11VertexShader* saved_vs = nullptr;
    ID3D11PixelShader* saved_ps = nullptr;
    ctx->VSGetShader(&saved_vs, nullptr, nullptr);
    ctx->PSGetShader(&saved_ps, nullptr, nullptr);
    ID3D11ShaderResourceView* saved_srv = nullptr;
    ctx->PSGetShaderResources(0, 1, &saved_srv);
    ID3D11SamplerState* saved_sampler = nullptr;
    ctx->PSGetSamplers(0, 1, &saved_sampler);
    ID3D11InputLayout* saved_layout = nullptr;
    ctx->IAGetInputLayout(&saved_layout);
    D3D11_PRIMITIVE_TOPOLOGY saved_topology{};
    ctx->IAGetPrimitiveTopology(&saved_topology);

    const D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(back_desc.Width),
                                  static_cast<float>(back_desc.Height), 0.0f, 1.0f};
    const FLOAT blend_factor[4] = {0, 0, 0, 0};
    ctx->OMSetRenderTargets(1, &g_overlay.rtv, nullptr);
    ctx->OMSetBlendState(g_overlay.blend, blend_factor, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(g_overlay.depth, 0);
    ctx->RSSetState(g_overlay.raster);
    ctx->RSSetViewports(1, &viewport);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_overlay.vs, nullptr, 0);
    ctx->PSSetShader(g_overlay.ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &g_overlay.srv);
    ctx->PSSetSamplers(0, 1, &g_overlay.sampler);
    ctx->Draw(3, 0);

    ctx->OMSetRenderTargets(1, &saved_rtv, saved_dsv);
    ctx->OMSetBlendState(saved_blend, saved_factor, saved_mask);
    ctx->OMSetDepthStencilState(saved_depth, saved_stencil);
    ctx->RSSetState(saved_raster);
    if (viewport_count) ctx->RSSetViewports(1, &saved_viewport);
    ctx->VSSetShader(saved_vs, nullptr, 0);
    ctx->PSSetShader(saved_ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &saved_srv);
    ctx->PSSetSamplers(0, 1, &saved_sampler);
    ctx->IASetInputLayout(saved_layout);
    ctx->IASetPrimitiveTopology(saved_topology);

    if (saved_rtv) saved_rtv->Release();
    if (saved_dsv) saved_dsv->Release();
    if (saved_blend) saved_blend->Release();
    if (saved_depth) saved_depth->Release();
    if (saved_raster) saved_raster->Release();
    if (saved_vs) saved_vs->Release();
    if (saved_ps) saved_ps->Release();
    if (saved_srv) saved_srv->Release();
    if (saved_sampler) saved_sampler->Release();
    if (saved_layout) saved_layout->Release();

}

void Log(const char* fmt, ...) {
    wchar_t dir[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
    if (!n || n >= MAX_PATH) return;

    wchar_t path[MAX_PATH];
    if (_snwprintf_s(path, _TRUNCATE, L"%s\\Reframe\\logs\\hook.log", dir) < 0) return;

    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"at") != 0 || !f) return;

    char line[512];
    va_list args;
    va_start(args, fmt);
    int len = _vsnprintf_s(line, _TRUNCATE, fmt, args);
    va_end(args);
    if (len > 0) fprintf(f, "[%lu] %s\n", GetCurrentProcessId(), line);
    fclose(f);
}

void ReleaseCapture() {
    for (ID3D11Texture2D* tex : g_cap.shared)
        if (tex) tex->Release();
    if (g_cap.context) g_cap.context->Release();
    if (g_cap.device) g_cap.device->Release();
    g_cap = Capture{};

    if (g_state) {
        g_state->api = static_cast<std::uint32_t>(GraphicsApi::None);
        for (std::uint32_t& handle : g_state->shared_handle) handle = 0;
    }
}

bool CreateShared(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& back) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = back.Width;
    desc.Height = back.Height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = back.Format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    for (std::uint32_t i = 0; i < kSlots; ++i) {
        ID3D11Texture2D* tex = nullptr;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &tex))) {
            Log("CreateTexture2D failed (%ux%u fmt %u)", desc.Width, desc.Height, desc.Format);
            return false;
        }

        IDXGIResource* res = nullptr;
        HANDLE handle = nullptr;
        if (SUCCEEDED(tex->QueryInterface(__uuidof(IDXGIResource), (void**)&res))) {
            res->GetSharedHandle(&handle);
            res->Release();
        }
        if (!handle) {
            tex->Release();
            Log("shared handle unavailable");
            return false;
        }

        g_cap.shared[i] = tex;
        g_state->shared_handle[i] =
            static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(handle));
    }

    g_cap.width = desc.Width;
    g_cap.height = desc.Height;
    g_cap.format = desc.Format;
    g_cap.next_slot = 0;

    g_state->width = desc.Width;
    g_state->height = desc.Height;
    g_state->format = static_cast<std::uint32_t>(desc.Format);
    g_state->write_slot = 0;
    g_state->texture_serial++;
    g_state->api = static_cast<std::uint32_t>(GraphicsApi::D3D11);

    Log("shared textures ready: %ux%u fmt %u handles 0x%x / 0x%x", desc.Width, desc.Height,
        desc.Format, g_state->shared_handle[0], g_state->shared_handle[1]);
    return true;
}

ID3D11Texture2D* PrepareLocked(IDXGISwapChain* swap) {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) || !back) return nullptr;

    D3D11_TEXTURE2D_DESC desc{};
    back->GetDesc(&desc);

    if (desc.SampleDesc.Count > 1) {
        back->Release();
        return nullptr;
    }

    if (g_cap.live && g_cap.swapchain == swap && g_cap.width == desc.Width &&
        g_cap.height == desc.Height && g_cap.format == desc.Format) {
        return back;
    }

    ReleaseCapture();

    ID3D11Device* device = nullptr;
    back->GetDevice(&device);
    if (!device) {
        back->Release();
        return nullptr;
    }
    device->GetImmediateContext(&g_cap.context);
    g_cap.device = device;
    g_cap.swapchain = swap;

    if (!CreateShared(device, desc)) {
        ReleaseCapture();
        back->Release();
        return nullptr;
    }
    g_cap.live = true;
    return back;
}

void PublishLocked(IDXGISwapChain* swap) {

    if (!g_state || g_state->stop) return;
    if (!g_state->capture && !g_state->overlay_visible) return;

    static std::int64_t last_publish_qpc = 0;
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const std::uint64_t period = g_state->capture_period_qpc;
    const bool due =
        period == 0 || now.QuadPart - last_publish_qpc >= static_cast<std::int64_t>(period);

    if (!due && !g_state->overlay_visible) return;

    ID3D11Texture2D* back = PrepareLocked(swap);
    if (!back) return;

    if (due && g_state->capture) {

        const std::uint32_t slot = g_cap.next_slot;
        g_cap.next_slot = (g_cap.next_slot + 1) % kSlots;

        g_cap.context->CopyResource(g_cap.shared[slot], back);

        last_publish_qpc = now.QuadPart;
        g_state->qpc = now.QuadPart;
        g_state->write_slot = slot;
        g_state->frame_index++;

        SetEvent(g_frame_event);
    }

    DrawOverlayLocked(back);
    back->Release();
}

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT,
                                               const DXGI_PRESENT_PARAMETERS*);
using ResizeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using SwapBuffersFn = BOOL(WINAPI*)(HDC);

PresentFn g_present_orig = nullptr;
Present1Fn g_present1_orig = nullptr;
ResizeFn g_resize_orig = nullptr;
SwapBuffersFn g_swap_buffers_orig = nullptr;
SwapBuffersFn g_gdi_swap_orig = nullptr;

thread_local bool t_inside = false;

void CaptureFrame(IDXGISwapChain* swap) {
    if (t_inside || g_shutting_down.load(std::memory_order_relaxed)) return;
    t_inside = true;
    EnterCriticalSection(&g_lock);
    PublishLocked(swap);
    LeaveCriticalSection(&g_lock);
    t_inside = false;
}

HRESULT STDMETHODCALLTYPE PresentHook(IDXGISwapChain* swap, UINT interval, UINT flags) {

    if (!(flags & DXGI_PRESENT_TEST)) CaptureFrame(swap);
    return g_present_orig(swap, interval, flags);
}

HRESULT STDMETHODCALLTYPE Present1Hook(IDXGISwapChain1* swap, UINT interval, UINT flags,
                                       const DXGI_PRESENT_PARAMETERS* params) {
    if (!(flags & DXGI_PRESENT_TEST)) CaptureFrame(swap);
    return g_present1_orig(swap, interval, flags, params);
}

void DrawGlOverlay(HDC hdc) {
    static bool seen = false;
    if (!seen) {
        seen = true;
        Log("first GL swap seen");
    }
    if (t_inside || g_shutting_down.load(std::memory_order_relaxed) || !g_state || g_state->stop ||
        !g_state->overlay_visible)
        return;
    t_inside = true;
    gl::DrawOverlay(hdc, g_state->overlay_handle, g_state->overlay_serial, g_state->overlay_width,
                    g_state->overlay_height);
    t_inside = false;
}

BOOL WINAPI SwapBuffersHook(HDC hdc) {
    DrawGlOverlay(hdc);
    return g_swap_buffers_orig(hdc);
}

BOOL WINAPI GdiSwapBuffersHook(HDC hdc) {
    DrawGlOverlay(hdc);
    return g_gdi_swap_orig(hdc);
}

HRESULT STDMETHODCALLTYPE ResizeBuffersHook(IDXGISwapChain* swap, UINT count, UINT w, UINT h,
                                            DXGI_FORMAT fmt, UINT flags) {

    EnterCriticalSection(&g_lock);
    ReleaseCapture();
    ReleaseOverlay();
    LeaveCriticalSection(&g_lock);
    return g_resize_orig(swap, count, w, h, fmt, flags);
}

bool InstallHooks() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = g_self;
    wc.lpszClassName = L"ReframeHookProbe";
    ATOM atom = RegisterClassExW(&wc);
    if (!atom) return false;

    HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                               g_self, nullptr);
    if (!wnd) {
        UnregisterClassW(wc.lpszClassName, g_self);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount = 2;
    scd.BufferDesc.Width = 1;
    scd.BufferDesc.Height = 1;
    scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = wnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    IDXGISwapChain* swap = nullptr;
    ID3D11Device* device = nullptr;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels,
                                               ARRAYSIZE(levels), D3D11_SDK_VERSION, &scd, &swap,
                                               &device, nullptr, nullptr);
    if (FAILED(hr)) {
        Log("probe device failed: 0x%08lx", hr);
        DestroyWindow(wnd);
        UnregisterClassW(wc.lpszClassName, g_self);
        return false;
    }

    void** vtable = *reinterpret_cast<void***>(swap);
    void* present = vtable[8];
    void* resize = vtable[13];
    void* present1 = nullptr;

    IDXGISwapChain1* swap1 = nullptr;
    if (SUCCEEDED(swap->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&swap1))) {
        present1 = (*reinterpret_cast<void***>(swap1))[22];
        swap1->Release();
    }

    swap->Release();
    device->Release();
    DestroyWindow(wnd);
    UnregisterClassW(wc.lpszClassName, g_self);

    if (MH_Initialize() != MH_OK) return false;

    bool ok = MH_CreateHook(present, (void*)&PresentHook, (void**)&g_present_orig) == MH_OK;
    ok = MH_CreateHook(resize, (void*)&ResizeBuffersHook, (void**)&g_resize_orig) == MH_OK && ok;
    if (present1)
        ok = MH_CreateHook(present1, (void*)&Present1Hook, (void**)&g_present1_orig) == MH_OK && ok;

    if (HMODULE gl_module = GetModuleHandleW(L"opengl32.dll")) {
        if (void* swap = (void*)GetProcAddress(gl_module, "wglSwapBuffers")) {
            if (MH_CreateHook(swap, (void*)&SwapBuffersHook, (void**)&g_swap_buffers_orig) ==
                MH_OK) {
                Log("wglSwapBuffers detoured at %p", swap);
            }
        }
        if (HMODULE gdi = GetModuleHandleW(L"gdi32.dll")) {
            if (void* swap = (void*)GetProcAddress(gdi, "SwapBuffers")) {
                if (MH_CreateHook(swap, (void*)&GdiSwapBuffersHook, (void**)&g_gdi_swap_orig) ==
                    MH_OK) {
                    Log("gdi32!SwapBuffers detoured at %p", swap);
                }
            }
        }
    } else {
        Log("no opengl32 in this process - Direct3D only");
    }

    if (!ok || MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        Log("failed to install detours");
        MH_Uninitialize();
        return false;
    }

    Log("detours installed (Present %p, Present1 %p, ResizeBuffers %p)", present, present1, resize);
    return true;
}

bool OpenIpc() {
    Names names(GetCurrentProcessId());

    g_section = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, names.section);
    if (!g_section) {
        Log("no section for this process (%lu) - reframe++ is not expecting us",
            GetCurrentProcessId());
        return false;
    }

    g_state = static_cast<SharedState*>(
        MapViewOfFile(g_section, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState)));
    if (!g_state) {
        Log("MapViewOfFile failed (%lu)", GetLastError());
        return false;
    }

    if (g_state->magic != kMagic || g_state->version != kVersion) {
        Log("handshake mismatch (magic 0x%08x version %u)", g_state->magic, g_state->version);
        return false;
    }

    g_frame_event = OpenEventW(EVENT_MODIFY_STATE, FALSE, names.frame);
    g_ready_event = OpenEventW(EVENT_MODIFY_STATE, FALSE, names.ready);
    if (!g_frame_event || !g_ready_event) Log("could not open the events (%lu)", GetLastError());
    return g_frame_event && g_ready_event;
}

void Teardown() {
    g_shutting_down.store(true, std::memory_order_relaxed);
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();

    EnterCriticalSection(&g_lock);
    ReleaseCapture();
    ReleaseOverlay();
    LeaveCriticalSection(&g_lock);
    gl::Release();

    if (g_frame_event) CloseHandle(g_frame_event);
    if (g_ready_event) CloseHandle(g_ready_event);
    if (g_state) UnmapViewOfFile(g_state);
    if (g_section) CloseHandle(g_section);
    g_frame_event = g_ready_event = g_section = nullptr;
    g_state = nullptr;
}

DWORD WINAPI HookThread(LPVOID) {
    QueryPerformanceFrequency(&g_qpc_freq);
    InitializeCriticalSection(&g_lock);

    Log("injected into pid %lu", GetCurrentProcessId());
    if (!OpenIpc() || !InstallHooks()) {
        Log("giving up - unloading");
        Teardown();
        DeleteCriticalSection(&g_lock);
        FreeLibraryAndExitThread(g_self, 0);
    }

    SetEvent(g_ready_event);
    Log("hook live in \"%ls\"", []() -> const wchar_t* {
        static wchar_t exe[MAX_PATH] = L"?";
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        return exe;
    }());

    HANDLE host = g_state->host_pid ? OpenProcess(SYNCHRONIZE, FALSE, g_state->host_pid) : nullptr;
    while (!g_state->stop) {
        if (host && WaitForSingleObject(host, 250) == WAIT_OBJECT_0) {
            Log("host process exited - unhooking");
            break;
        }
        if (!host) Sleep(250);
    }
    if (host) CloseHandle(host);

    Teardown();
    DeleteCriticalSection(&g_lock);
    FreeLibraryAndExitThread(g_self, 0);
    return 0;
}

}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;

    g_self = module;
    DisableThreadLibraryCalls(module);

    HANDLE thread = CreateThread(nullptr, 0, HookThread, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
    return TRUE;
}
