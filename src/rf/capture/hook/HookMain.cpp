// reframe-hook: the DLL reframe++ injects into a game to capture its swapchain.
//
// Copyright (c) 2026 shadowyohan. See LICENSE at the repository root.
//
// Independent implementation. Detouring Present, copying the backbuffer into a
// shared texture and handing it over through named shared memory is the standard
// way to capture a Direct3D application, and no third-party code is reproduced
// here: the detours use MinHook against real function pointers rather than
// offsets discovered by a helper process, the texture is synchronised with a
// keyed mutex, and the IPC layout in HookProtocol.h was designed for this
// project.
//
// What runs here runs inside somebody else's process, on their render thread,
// so the rules are strict: no CRT locale work, no C++ exceptions escaping, no
// blocking waits in Present, and every failure ends in "stop capturing" rather
// than anything that could take the game down with us.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>

#include <atomic>
#include <cstdio>

#include <MinHook.h>

#include "HookProtocol.h"

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
    ID3D11Texture2D* shared = nullptr;
    IDXGIKeyedMutex* mutex = nullptr;
    IUnknown* swapchain = nullptr;
    UINT width = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool live = false;
} g_cap;

LARGE_INTEGER g_qpc_freq{};

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
    if (g_cap.mutex) g_cap.mutex->Release();
    if (g_cap.shared) g_cap.shared->Release();
    if (g_cap.context) g_cap.context->Release();
    if (g_cap.device) g_cap.device->Release();
    g_cap = Capture{};

    if (g_state) {
        g_state->api = static_cast<std::uint32_t>(GraphicsApi::None);
        g_state->shared_handle = 0;
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
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

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
    IDXGIKeyedMutex* mutex = nullptr;
    tex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&mutex);

    if (!handle || !mutex) {
        if (mutex) mutex->Release();
        tex->Release();
        Log("shared handle / keyed mutex unavailable");
        return false;
    }

    g_cap.shared = tex;
    g_cap.mutex = mutex;
    g_cap.width = desc.Width;
    g_cap.height = desc.Height;
    g_cap.format = desc.Format;

    g_state->width = desc.Width;
    g_state->height = desc.Height;
    g_state->format = static_cast<std::uint32_t>(desc.Format);
    g_state->shared_handle = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(handle));
    g_state->texture_serial++;
    g_state->api = static_cast<std::uint32_t>(GraphicsApi::D3D11);

    Log("shared texture ready: %ux%u fmt %u handle 0x%x", desc.Width, desc.Height, desc.Format,
        g_state->shared_handle);
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
    if (!g_state || g_state->stop || !g_state->capture) return;

    ID3D11Texture2D* back = PrepareLocked(swap);
    if (!back) return;

    if (g_cap.mutex->AcquireSync(kKeyHook, 0) == WAIT_OBJECT_0) {
        g_cap.context->CopyResource(g_cap.shared, back);

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        g_state->qpc = now.QuadPart;
        g_state->frame_index++;

        g_cap.mutex->ReleaseSync(kKeyHost);
        SetEvent(g_frame_event);
    }
    back->Release();
}

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT,
                                               const DXGI_PRESENT_PARAMETERS*);
using ResizeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

PresentFn g_present_orig = nullptr;
Present1Fn g_present1_orig = nullptr;
ResizeFn g_resize_orig = nullptr;

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

HRESULT STDMETHODCALLTYPE ResizeBuffersHook(IDXGISwapChain* swap, UINT count, UINT w, UINT h,
                                            DXGI_FORMAT fmt, UINT flags) {

    EnterCriticalSection(&g_lock);
    ReleaseCapture();
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
    if (!g_section) return false;

    g_state = static_cast<SharedState*>(
        MapViewOfFile(g_section, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState)));
    if (!g_state) return false;

    if (g_state->magic != kMagic || g_state->version != kVersion) {
        Log("handshake mismatch (magic 0x%08x version %u)", g_state->magic, g_state->version);
        return false;
    }

    g_frame_event = OpenEventW(EVENT_MODIFY_STATE, FALSE, names.frame);
    g_ready_event = OpenEventW(EVENT_MODIFY_STATE, FALSE, names.ready);
    return g_frame_event && g_ready_event;
}

void Teardown() {
    g_shutting_down.store(true, std::memory_order_relaxed);
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();

    EnterCriticalSection(&g_lock);
    ReleaseCapture();
    LeaveCriticalSection(&g_lock);

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

    if (!OpenIpc() || !InstallHooks()) {
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
