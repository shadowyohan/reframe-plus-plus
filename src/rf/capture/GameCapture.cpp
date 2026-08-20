#include "rf/capture/GameCapture.h"

#include "rf/core/Lang.h"

#include <psapi.h>

#include <algorithm>
#include <tlhelp32.h>

#include <filesystem>
#include <format>

#include "rf/core/Log.h"
#include "rf/core/Strings.h"

using Microsoft::WRL::ComPtr;

namespace rf {
namespace {

Status RemoteLoadLibrary(HANDLE process, const std::wstring& dll_path) {
    const SIZE_T bytes = (dll_path.size() + 1) * sizeof(wchar_t);
    void* remote = ::VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) return Status::Fail(std::format("VirtualAllocEx failed: {}", ::GetLastError()));

    Status result = Status::Ok();
    if (!::WriteProcessMemory(process, remote, dll_path.c_str(), bytes, nullptr)) {
        result = Status::Fail(std::format("WriteProcessMemory failed: {}", ::GetLastError()));
    } else {
        auto* loader = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            ::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
        HANDLE thread = ::CreateRemoteThread(process, nullptr, 0, loader, remote, 0, nullptr);
        if (!thread) {
            result = Status::Fail(std::format("CreateRemoteThread failed: {}", ::GetLastError()));
        } else {

            if (::WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0) {
                DWORD module = 0;
                ::GetExitCodeThread(thread, &module);
                if (module == 0) result = Status::Fail("the game refused to load the hook");
            }
            ::CloseHandle(thread);
        }
    }

    ::VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    return result;
}

bool IsProcess64Bit(HANDLE process, bool& out) {
    BOOL wow64 = FALSE;
    if (!::IsWow64Process(process, &wow64)) return false;

    out = !wow64;
    return true;
}

std::string ProcessName(std::uint32_t pid) {
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t path[MAX_PATH]{};
    DWORD len = MAX_PATH;
    std::string name;
    if (::QueryFullProcessImageNameW(h, 0, path, &len)) {
        const wchar_t* leaf = wcsrchr(path, L'\\');
        name = ToUtf8(leaf ? leaf + 1 : path);
    }
    ::CloseHandle(h);
    return name;
}

void EnableDebugPrivilege() {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &token)) return;
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (::LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &tp.Privileges[0].Luid))
        ::AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    ::CloseHandle(token);
}

}

std::uint32_t GameProcessForWindow(void* hwnd) {
    HWND wnd = static_cast<HWND>(hwnd);
    if (!wnd || !::IsWindow(wnd)) return 0;

    DWORD pid = 0;
    ::GetWindowThreadProcessId(wnd, &pid);
    if (!pid || pid == ::GetCurrentProcessId()) return 0;

    if (wnd == ::GetDesktopWindow() || wnd == ::GetShellWindow()) return 0;
    DWORD shell_pid = 0;
    if (HWND shell = ::GetShellWindow()) ::GetWindowThreadProcessId(shell, &shell_pid);
    if (pid == shell_pid) return 0;

    return pid;
}

Status HookDllPath(std::wstring& out_path) {
    wchar_t exe[MAX_PATH]{};
    const DWORD len = ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
        return Status::Fail(std::format("GetModuleFileName failed: {}", ::GetLastError()));

    const auto path = std::filesystem::path(exe).parent_path() / L"reframe-hook64.dll";

    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return Status::Fail(
            TrFormat("reframe-hook64.dll не найдена рядом с программой ({})", path.string()));

    out_path = path.wstring();
    return Status::Ok();
}

bool AlreadyInjected(HANDLE process) {
    HMODULE modules[1024];
    DWORD needed = 0;
    if (!::EnumProcessModulesEx(process, modules, sizeof(modules), &needed, LIST_MODULES_64BIT))
        return false;

    const DWORD count = std::min<DWORD>(needed / sizeof(HMODULE), std::size(modules));
    for (DWORD i = 0; i < count; ++i) {
        wchar_t name[MAX_PATH]{};
        if (!::GetModuleBaseNameW(process, modules[i], name, MAX_PATH)) continue;
        if (_wcsicmp(name, L"reframe-hook64.dll") == 0) return true;
    }
    return false;
}

Status InjectHook(std::uint32_t pid, const std::wstring& dll_path) {
    EnableDebugPrivilege();

    const DWORD access = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                         PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION;
    HANDLE process = ::OpenProcess(access, FALSE, pid);
    if (!process) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_ACCESS_DENIED)
            return Status::Fail(
                "нет доступа к процессу - запустите reframe++ от имени администратора");
        return Status::Fail(std::format("OpenProcess failed: {}", err));
    }

    bool is64 = true;
    if (IsProcess64Bit(process, is64) && !is64) {
        ::CloseHandle(process);
        return Status::Fail("32-битные процессы пока не поддерживаются игровым захватом");
    }

    if (AlreadyInjected(process)) {
        ::CloseHandle(process);
        RF_DEBUG("hook already present in pid {} - not loading it twice", pid);
        return Status::Ok();
    }

    Status s = RemoteLoadLibrary(process, dll_path);
    ::CloseHandle(process);
    return s;
}

GameCapture::GameCapture(D3DDevicePtr device) : device_(std::move(device)) {}

GameCapture::~GameCapture() { Stop(); }

Status GameCapture::Attach(const CaptureTarget& target) {
    HWND wnd = target.hwnd ? static_cast<HWND>(target.hwnd) : ::GetForegroundWindow();
    pid_ = GameProcessForWindow(wnd);
    if (!pid_) return Status::Fail("нет подходящего окна на переднем плане");

    game_name_ = ProcessName(pid_);

    std::wstring dll;
    if (Status s = HookDllPath(dll); !s) return s;

    hook::Names names(pid_);

    section_ = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                    sizeof(hook::SharedState), names.section);
    if (!section_) return Status::Fail(std::format("CreateFileMapping failed: {}", ::GetLastError()));

    state_ = static_cast<hook::SharedState*>(
        ::MapViewOfFile(section_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(hook::SharedState)));
    if (!state_) {
        ReleaseIpc();
        return Status::Fail(std::format("MapViewOfFile failed: {}", ::GetLastError()));
    }

    *state_ = hook::SharedState{};
    state_->magic = hook::kMagic;
    state_->version = hook::kVersion;
    state_->capture = 1;
    state_->host_pid = ::GetCurrentProcessId();

    if (target.fps > 0) {
        LARGE_INTEGER freq{};
        ::QueryPerformanceFrequency(&freq);
        state_->capture_period_qpc =
            static_cast<std::uint64_t>(freq.QuadPart) * 9 / (target.fps * 10);
    }

    frame_event_ = ::CreateEventW(nullptr, FALSE, FALSE, names.frame);
    ready_event_ = ::CreateEventW(nullptr, TRUE, FALSE, names.ready);
    if (!frame_event_ || !ready_event_) {
        ReleaseIpc();
        return Status::Fail(std::format("CreateEvent failed: {}", ::GetLastError()));
    }

    if (Status s = InjectHook(pid_, dll); !s) {
        ReleaseIpc();
        return s;
    }

    if (::WaitForSingleObject(ready_event_, 5000) != WAIT_OBJECT_0) {
        ReleaseIpc();
        return Status::Fail("приложение не ответило на внедрение (античит?)");
    }

    attached_ = true;
    return Status::Ok();
}

Status GameCapture::Start(const CaptureTarget& target, const FrameCallback& on_frame) {
    Stop();
    RF_TRY(Attach(target));

    const Ticks100ns deadline = Now100ns() + kOneSecond100ns * 3 / 2;
    while (state_->width == 0 || state_->height == 0) {
        if (Now100ns() > deadline) {
            ReleaseIpc();
            return Status::Fail("приложение не отдало ни одного кадра через swapchain");
        }
        ::WaitForSingleObject(frame_event_, 100);
    }
    width_ = state_->width;
    height_ = state_->height;

    RF_INFO("game capture hooked \"{}\" (pid {}, {}x{})", game_name_, pid_, width_, height_);

    on_frame_ = on_frame;
    stats_ = CaptureStats{};
    last_frame_index_ = 0;
    running_.store(true, std::memory_order_release);
    reader_ = std::thread([this] { ReaderLoop(); });
    return Status::Ok();
}

void GameCapture::Stop() {
    attached_ = false;

    running_.store(false, std::memory_order_release);
    if (state_) state_->stop = 1;
    if (frame_event_) ::SetEvent(frame_event_);
    if (reader_.joinable()) reader_.join();

    for (auto& slot : shared_) slot.Reset();
    staging_.Reset();
    bound_serial_ = 0;
    width_ = height_ = 0;
    ReleaseIpc();
}

void GameCapture::SetOverlay(std::uint32_t handle, std::uint32_t serial, std::uint32_t width,
                             std::uint32_t height, bool visible) {
    if (!state_) return;
    state_->overlay_handle = handle;
    state_->overlay_serial = serial;
    state_->overlay_width = width;
    state_->overlay_height = height;
    state_->overlay_visible = visible ? 1u : 0u;
}

void GameCapture::ReleaseIpc() {
    if (state_) ::UnmapViewOfFile(state_);
    if (section_) ::CloseHandle(section_);
    if (frame_event_) ::CloseHandle(frame_event_);
    if (ready_event_) ::CloseHandle(ready_event_);
    state_ = nullptr;
    section_ = frame_event_ = ready_event_ = nullptr;
}

bool GameCapture::RebindSharedTexture() {
    for (auto& slot : shared_) slot.Reset();
    staging_.Reset();

    for (std::uint32_t i = 0; i < hook::kSlots; ++i) {
        const HANDLE handle =
            reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(state_->shared_handle[i]));
        if (!handle) return false;

        if (HRESULT hr = device_->device()->OpenSharedResource(handle, IID_PPV_ARGS(&shared_[i]));
            FAILED(hr)) {
            RF_WARN("game capture: OpenSharedResource failed: 0x{:08x}", static_cast<unsigned>(hr));
            return false;
        }
    }

    D3D11_TEXTURE2D_DESC desc{};
    shared_[0]->GetDesc(&desc);

    D3D11_TEXTURE2D_DESC own = desc;
    own.MiscFlags = 0;
    own.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    own.Usage = D3D11_USAGE_DEFAULT;
    own.CPUAccessFlags = 0;
    if (FAILED(device_->device()->CreateTexture2D(&own, nullptr, &staging_))) return false;

    width_ = desc.Width;
    height_ = desc.Height;
    bound_serial_ = state_->texture_serial;
    RF_INFO("game capture: bound {}x{} from \"{}\"", width_, height_, game_name_);
    return true;
}

void GameCapture::ReaderLoop() {
    MmcssScope mmcss(L"Capture");

    while (running_.load(std::memory_order_acquire)) {

        const DWORD wait = ::WaitForSingleObject(frame_event_, 500);
        if (!running_.load(std::memory_order_acquire)) break;

        if (wait == WAIT_TIMEOUT) {

            HANDLE p = ::OpenProcess(SYNCHRONIZE, FALSE, pid_);
            if (!p) {
                RF_WARN("game capture: \"{}\" exited", game_name_);
                break;
            }
            const bool dead = ::WaitForSingleObject(p, 0) == WAIT_OBJECT_0;
            ::CloseHandle(p);
            if (dead) {
                RF_WARN("game capture: \"{}\" exited", game_name_);
                break;
            }
            continue;
        }
        if (wait != WAIT_OBJECT_0) break;

        if (state_->texture_serial != bound_serial_ || !shared_) {
            if (!RebindSharedTexture()) continue;
        }

        const std::uint64_t index = state_->frame_index;
        if (index == last_frame_index_) continue;
        const std::int64_t qpc = state_->qpc;
        const std::uint32_t slot = state_->write_slot % hook::kSlots;

        {

            D3DDevice::ContextLock lock(*device_);
            device_->context()->CopyResource(staging_.Get(), shared_[slot].Get());
        }

        last_frame_index_ = index;
        stats_.frames_captured++;

        D3D11_TEXTURE2D_DESC desc{};
        staging_->GetDesc(&desc);

        CapturedFrame frame{};
        frame.texture = staging_.Get();
        frame.width = desc.Width;
        frame.height = desc.Height;
        frame.format = desc.Format;
        frame.timestamp = QpcTo100ns(qpc);
        frame.content_changed = true;
        frame.frame_index = index;
        if (on_frame_) on_frame_(frame);
    }

    running_.store(false, std::memory_order_release);
}

}
