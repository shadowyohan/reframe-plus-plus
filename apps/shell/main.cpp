#include <windows.h>

#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj_core.h>

#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>

#include <share.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <format>
#include <functional>
#include <mutex>
#include <thread>
#include <tuple>
#include <utility>

#include "resource.h"

#include "rf/core/Log.h"
#include "rf/core/Paths.h"
#include "rf/core/Strings.h"
#include "rf/core/Time.h"
#include "rf/gallery/Gallery.h"
#include "rf/engine/Recorder.h"
#include "rf/ui/Hud.h"
#include "rf/ui/Overlay.h"
#include "rf/ui/Screens.h"

namespace {

constexpr int kHotkeyOverlay = 1;
constexpr int kHotkeyRecord = 2;
constexpr int kHotkeyReplay = 3;

constexpr int kHotkeyEscape = 4;

constexpr UINT WM_RF_TRAY = WM_APP + 1;
constexpr UINT kMenuOpen = 100;
constexpr UINT kMenuQuit = 101;
constexpr UINT kMenuFolder = 102;

std::filesystem::path FindAssetDir() {
    wchar_t exe[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::filesystem::path dir = std::filesystem::path(exe).parent_path();

    for (int up = 0; up < 5; ++up) {
        std::error_code ec;
        if (std::filesystem::exists(dir / "assets" / "icons", ec)) return dir / "assets";
        dir = dir.parent_path();
    }
    return std::filesystem::path(exe).parent_path() / "assets";
}

bool PickFolder(HWND owner, std::filesystem::path& path) {
    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&dialog))))
        return false;

    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST);
    if (FAILED(dialog->Show(owner))) return false;

    Microsoft::WRL::ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) return false;

    PWSTR raw = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw))) return false;
    path = raw;
    ::CoTaskMemFree(raw);
    return true;
}

void ShellOpen(const std::filesystem::path& path) {
    ::ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void ShellReveal(const std::filesystem::path& path) {
    const std::wstring args = L"/select,\"" + path.wstring() + L"\"";
    ::ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

LONG CALLBACK CrashReporter(EXCEPTION_POINTERS* info) {
    const auto code = info->ExceptionRecord->ExceptionCode;

    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_INT_DIVIDE_BY_ZERO)
        return EXCEPTION_CONTINUE_SEARCH;

    void* address = info->ExceptionRecord->ExceptionAddress;
    wchar_t module_path[MAX_PATH] = L"<unknown>";
    HMODULE module = nullptr;
    if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             static_cast<LPCWSTR>(address), &module))
        ::GetModuleFileNameW(module, module_path, MAX_PATH);

    RF_ERROR("CRASH 0x{:08X} at {} in {}", static_cast<unsigned>(code), address,
             rf::ToUtf8(module_path));
    return EXCEPTION_CONTINUE_SEARCH;
}

struct MicDevice {
    std::string id;
    std::string name;
};

std::vector<MicDevice> EnumerateMics() {
    std::vector<MicDevice> out;
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&enumerator))))
        return out;

    Microsoft::WRL::ComPtr<IMMDeviceCollection> devices;
    if (FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &devices))) return out;

    UINT count = 0;
    devices->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        Microsoft::WRL::ComPtr<IMMDevice> device;
        if (FAILED(devices->Item(i, &device))) continue;

        MicDevice mic;
        PWSTR id = nullptr;
        if (SUCCEEDED(device->GetId(&id)) && id) {
            mic.id = rf::ToUtf8(id);
            ::CoTaskMemFree(id);
        }
        Microsoft::WRL::ComPtr<IPropertyStore> props;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT value;
            ::PropVariantInit(&value);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &value)) &&
                value.vt == VT_LPWSTR)
                mic.name = rf::ToUtf8(value.pwszVal);
            ::PropVariantClear(&value);
        }
        if (!mic.id.empty()) out.push_back(std::move(mic));
    }
    return out;
}

void DiskUsage(const std::filesystem::path& dir, std::uint64_t& used, std::uint64_t& total) {
    ULARGE_INTEGER free_bytes{}, total_bytes{}, total_free{};
    if (::GetDiskFreeSpaceExW(dir.c_str(), &free_bytes, &total_bytes, &total_free))
        total = total_bytes.QuadPart;

    std::uint64_t recordings = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const auto size = entry.file_size(ec);
        if (!ec) recordings += size;
    }
    used = recordings;
}

struct App {
    rf::Settings settings;
    rf::Recorder recorder;
    rf::Gallery gallery;
    rf::ui::Overlay overlay;
    rf::ui::Menu menu;
    rf::ui::Hud hud;
    rf::ui::AppModel model;

    rf::VideoPlayer player;

    NOTIFYICONDATAW tray{};
    HWND tray_hwnd = nullptr;
    rf::Ticks100ns recording_started = 0;
    rf::Ticks100ns settings_touched = 0;
    bool settings_dirty = false;

    std::int32_t applied_gpu_luid_low = 0;
    std::int32_t applied_gpu_luid_high = 0;

    bool replay_target = false;
    rf::Ticks100ns replay_touched = 0;
    bool replay_pending = false;

    rf::Ticks100ns gallery_polled = 0;

    std::atomic<void*> refused_monitor{nullptr};

    std::thread engine;
    std::mutex task_mutex;
    std::condition_variable task_cv;
    std::deque<std::function<void()>> tasks;
    std::atomic<bool> engine_running{false};
    std::atomic<bool> needs_rebind{false};
    std::atomic<bool> pending_rebind{false};

    std::atomic<const char*> stage{"idle"};
    std::atomic<std::uint64_t> ticks{0};
    std::thread watchdog;

    bool quit = false;
};

App* g_app = nullptr;

void PostEngine(std::function<void()> task) {
    App& app = *g_app;
    {
        std::scoped_lock lock(app.task_mutex);

        if (app.tasks.size() > 8) app.tasks.clear();
        app.tasks.push_back(std::move(task));
    }
    app.task_cv.notify_one();
}

void WatchdogLoop() {
    App& app = *g_app;
    ::SetThreadDescription(::GetCurrentThread(), L"rf-watchdog");

    const auto path = rf::paths::LogDir() / L"watchdog.log";
    std::FILE* file = _wfsopen(path.c_str(), L"w", _SH_DENYNO);
    if (!file) return;
    std::setvbuf(file, nullptr, _IONBF, 0);

    auto stamp = [&](const char* what, std::uint64_t tick, int seconds) {
        SYSTEMTIME now{};
        ::GetLocalTime(&now);
        std::fprintf(file, "[%02d:%02d:%02d.%03d] %-9s tick=%llu stage=%s stalled=%ds\n",
                     now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, what,
                     static_cast<unsigned long long>(tick),
                     app.stage.load(std::memory_order_relaxed), seconds);
    };

    std::uint64_t last_tick = 0;
    int stalled_for = 0;
    bool reported = false;
    int since_heartbeat = 0;

    while (app.engine_running.load(std::memory_order_relaxed)) {
        ::Sleep(1000);
        const std::uint64_t tick = app.ticks.load(std::memory_order_relaxed);

        if (tick != last_tick) {
            if (reported) stamp("RECOVERED", tick, stalled_for);
            last_tick = tick;
            stalled_for = 0;
            reported = false;

            if (++since_heartbeat >= 5) {
                since_heartbeat = 0;
                stamp("alive", tick, 0);
            }
            continue;
        }

        if (++stalled_for >= 2 && !reported) {
            reported = true;
            stamp("STALLED", tick, stalled_for);
        }
    }
    stamp("shutdown", app.ticks.load(std::memory_order_relaxed), 0);
    std::fclose(file);
}

void EngineLoop() {
    App& app = *g_app;
    ::SetThreadDescription(::GetCurrentThread(), L"rf-engine");
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    while (app.engine_running.load(std::memory_order_relaxed)) {
        std::function<void()> task;
        {
            std::unique_lock lock(app.task_mutex);
            app.task_cv.wait(lock, [&] { return !app.tasks.empty() || !app.engine_running; });
            if (!app.engine_running && app.tasks.empty()) break;
            task = std::move(app.tasks.front());
            app.tasks.pop_front();
        }
        if (task) task();
    }
    ::CoUninitialize();
}

void SettingsTouched() {
    g_app->settings_dirty = true;
    g_app->settings_touched = rf::Now100ns();
}

HHOOK g_key_hook = nullptr;

HHOOK g_mouse_hook = nullptr;
std::atomic<int> g_mouse_x{0};
std::atomic<int> g_mouse_y{0};

POINT g_mouse_origin{};
std::atomic<bool> g_mouse_down{false};
std::atomic<int> g_mouse_wheel{0};

LRESULT CALLBACK MouseCaptureProc(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION) {
        const auto* ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(lparam);
        switch (wparam) {
            case WM_MOUSEMOVE: {

                const int dx = ms->pt.x - g_mouse_origin.x;
                const int dy = ms->pt.y - g_mouse_origin.y;
                const int w = ::GetSystemMetrics(SM_CXSCREEN);
                const int h = ::GetSystemMetrics(SM_CYSCREEN);
                g_mouse_x.store(std::clamp(g_mouse_x.load(std::memory_order_relaxed) + dx, 0, w - 1),
                                std::memory_order_relaxed);
                g_mouse_y.store(std::clamp(g_mouse_y.load(std::memory_order_relaxed) + dy, 0, h - 1),
                                std::memory_order_relaxed);
                return 1;
            }
            case WM_LBUTTONDOWN:
                g_mouse_down.store(true, std::memory_order_relaxed);
                return 1;
            case WM_LBUTTONUP:
                g_mouse_down.store(false, std::memory_order_relaxed);
                return 1;
            case WM_MOUSEWHEEL:
                g_mouse_wheel.fetch_add(GET_WHEEL_DELTA_WPARAM(ms->mouseData) / WHEEL_DELTA,
                                        std::memory_order_relaxed);
                return 1;
            case WM_RBUTTONDOWN:
            case WM_RBUTTONUP:
            case WM_MBUTTONDOWN:
            case WM_MBUTTONUP:

                return 1;
            default:
                break;
        }
    }
    return ::CallNextHookEx(g_mouse_hook, code, wparam, lparam);
}

void SetMouseCaptureHook(bool wanted) {
    if (wanted == (g_mouse_hook != nullptr)) return;
    if (wanted) {

        POINT p{};
        if (::GetCursorPos(&p)) {
            g_mouse_origin = p;
            g_mouse_x.store(p.x, std::memory_order_relaxed);
            g_mouse_y.store(p.y, std::memory_order_relaxed);
        }
        g_mouse_down.store(false, std::memory_order_relaxed);
        g_mouse_hook = ::SetWindowsHookExW(WH_MOUSE_LL, MouseCaptureProc, nullptr, 0);
        if (!g_mouse_hook) RF_WARN("could not install the mouse hook for the in-game menu");
    } else {
        ::UnhookWindowsHookEx(g_mouse_hook);
        g_mouse_hook = nullptr;
    }
}

bool IsModifierKey(DWORD vk) {
    return vk == VK_CONTROL || vk == VK_MENU || vk == VK_SHIFT || vk == VK_LCONTROL ||
           vk == VK_RCONTROL || vk == VK_LMENU || vk == VK_RMENU || vk == VK_LSHIFT ||
           vk == VK_RSHIFT || vk == VK_LWIN || vk == VK_RWIN;
}

LRESULT CALLBACK KeyCaptureProc(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION && (wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN) && g_app) {
        const auto* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam);

        if (IsModifierKey(kb->vkCode)) return ::CallNextHookEx(g_key_hook, code, wparam, lparam);

        std::uint32_t mods = 0;
        if (::GetAsyncKeyState(VK_CONTROL) < 0) mods |= MOD_CONTROL;
        if (::GetAsyncKeyState(VK_MENU) < 0) mods |= MOD_ALT;
        if (::GetAsyncKeyState(VK_SHIFT) < 0) mods |= MOD_SHIFT;

        g_app->model.pending_vk = kb->vkCode;
        g_app->model.pending_mods = mods;
        return 1;
    }
    return ::CallNextHookEx(g_key_hook, code, wparam, lparam);
}

void SetKeyCaptureHook(bool wanted) {
    if (wanted == (g_key_hook != nullptr)) return;
    if (wanted) {
        g_key_hook = ::SetWindowsHookExW(WH_KEYBOARD_LL, KeyCaptureProc, nullptr, 0);
        if (!g_key_hook) RF_WARN("could not install the keyboard hook for rebinding");
    } else {
        ::UnhookWindowsHookEx(g_key_hook);
        g_key_hook = nullptr;
    }
}

void RestartRecorder() {
    PostEngine([] {
        App& app = *g_app;
        const bool was_armed = app.recorder.state() != rf::Recorder::State::Idle;
        if (app.recorder.state() == rf::Recorder::State::Recording) app.recorder.StopRecording();
        app.recorder.Shutdown();

        if (auto s = app.recorder.Init(app.settings); !s.ok()) {
            RF_ERROR("restarting the recorder failed: {}", s.str());
            app.hud.Push(rf::ui::Hud::Kind::Error, "Не удалось переключить видеокарту");
            return;
        }
        if (was_armed && app.settings.replay_enabled) {
            if (auto s = app.recorder.ArmReplay(); !s.ok())
                RF_ERROR("re-arm after the GPU change: {}", s.str());
        }
        RF_INFO("recorder restarted on the selected GPU");
    });
}

void FlushSettings() {
    App& app = *g_app;
    if (!app.settings_dirty) return;
    if (rf::Now100ns() - app.settings_touched < rf::MsTo100ns(700)) return;

    app.settings_dirty = false;
    app.settings.Save(rf::paths::SettingsFile());
    app.gallery.SetDirectory(app.settings.output_dir);

    if (app.settings.gpu_luid_low != app.applied_gpu_luid_low ||
        app.settings.gpu_luid_high != app.applied_gpu_luid_high) {
        app.applied_gpu_luid_low = app.settings.gpu_luid_low;
        app.applied_gpu_luid_high = app.settings.gpu_luid_high;
        RF_INFO("capture GPU changed - restarting the recorder");
        RestartRecorder();
        return;
    }

    const rf::Settings snapshot = app.settings;
    PostEngine([snapshot] {
        if (auto s = g_app->recorder.ApplySettings(snapshot); !s.ok()) {
            RF_ERROR("applying settings: {}", s.str());
            g_app->hud.Push(rf::ui::Hud::Kind::Error, "Не удалось применить настройки");
        }
    });
}

void RefreshMicDevices() {
    App& app = *g_app;
    app.model.mic_devices.clear();
    for (const auto& mic : EnumerateMics())
        app.model.mic_devices.push_back({mic.id, mic.name});
    app.model.mic_selected_id = app.settings.mic_device;
}

void FollowCursorAcrossMonitors() {
    App& app = *g_app;
    if (!app.settings.monitor_follow_cursor || app.settings.capture_focused_window_only) return;
    if (app.recorder.state() == rf::Recorder::State::Idle) return;

    POINT cursor{};
    if (!::GetCursorPos(&cursor)) return;

    HMONITOR under = ::MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    static HMONITOR watched = nullptr;
    static rf::Ticks100ns since = 0;

    if (under != watched) {
        watched = under;
        since = rf::Now100ns();
        return;
    }
    if (under == app.recorder.capture_monitor()) return;

    const rf::Ticks100ns dwell = rf::MsTo100ns(app.settings.monitor_switch_delay_ms);
    if (rf::Now100ns() - since < dwell) return;

    static HMONITOR refused = nullptr;
    if (under == refused) return;

    since = rf::Now100ns();
    PostEngine([under] {
        if (auto s = g_app->recorder.SetCaptureMonitor(under); !s.ok()) {
            RF_WARN("could not follow the cursor to the other display: {} - not trying it again",
                    s.str());
            g_app->refused_monitor.store(under, std::memory_order_relaxed);
        }
    });
    if (void* denied = app.refused_monitor.exchange(nullptr, std::memory_order_relaxed))
        refused = static_cast<HMONITOR>(denied);
}

void RefreshMonitors() {
    App& app = *g_app;
    app.model.monitors.clear();
    const auto monitors = rf::EnumerateMonitors();
    for (std::size_t i = 0; i < monitors.size(); ++i) {
        const rf::MonitorInfo& m = monitors[i];

        app.model.monitors.push_back(
            {std::format("{}. {} - {}x{}{}", i + 1, rf::ToUtf8(m.description), m.width, m.height,
                         m.primary ? " (основной)" : ""),
             rf::ToUtf8(m.device_name)});
    }
}

void PickMicDevice(const std::string& id) {
    App& app = *g_app;
    app.settings.mic_device = id;
    app.model.mic_selected_id = id;
    if (id.empty()) {
        app.model.mic_device_label = "Системный по умолчанию";
    } else {
        for (const auto& mic : app.model.mic_devices)
            if (mic.id == id) app.model.mic_device_label = mic.name;
    }
    SettingsTouched();
}

void RefreshMicLabel() {
    App& app = *g_app;
    if (app.settings.mic_device.empty()) {
        app.model.mic_device_label = "Системный по умолчанию";
        return;
    }
    for (const auto& mic : EnumerateMics()) {
        if (mic.id == app.settings.mic_device) {
            app.model.mic_device_label = mic.name;
            return;
        }
    }
    app.model.mic_device_label = "Системный по умолчанию";
}

void ToggleRecording() {
    PostEngine([] {
        App& app = *g_app;
        if (app.recorder.state() == rf::Recorder::State::Recording) {
            app.recorder.StopRecording();
            app.hud.Push(rf::ui::Hud::Kind::RecordingStopped, "Запись остановлена");
            app.gallery.Refresh();
        } else if (auto s = app.recorder.StartRecording({}); s.ok()) {
            app.recording_started = rf::Now100ns();

            const std::string app_name = app.recorder.target_app();
            app.hud.Push(rf::ui::Hud::Kind::RecordingStarted,
                         app_name.empty() ? "Запись запущена"
                                          : std::format("Запись {} запущена", app_name),
                         {}, app_name);
        } else {
            RF_ERROR("start recording: {}", s.str());
            app.hud.Push(rf::ui::Hud::Kind::Error, "Не удалось начать запись");
        }
    });
}

void SaveReplay() {
    PostEngine([] {
    App& app = *g_app;
    std::filesystem::path saved;
    if (auto s = app.recorder.SaveReplay(&saved); s.ok()) {

        const std::string app_name = app.recorder.target_app();
        app.hud.Push(rf::ui::Hud::Kind::ReplaySaved,
                     app_name.empty()
                         ? "Мгновенный повтор сохранен"
                         : std::format("Мгновенный повтор из {} сохранен", app_name),
                     rf::ToUtf8(saved.filename().wstring()), app_name);
        app.gallery.Refresh();
    } else {
        RF_WARN("save replay: {}", s.str());
        app.hud.Push(rf::ui::Hud::Kind::Error, "Повтор недоступен");
    }
    });
}

void SetReplayArmed(bool armed) {
    App& app = *g_app;
    app.replay_target = armed;
    app.replay_touched = rf::Now100ns();
    app.replay_pending = true;
    app.settings.replay_enabled = armed;
    app.model.replay_armed = armed;
}

void ApplyReplayState() {
    App& app = *g_app;
    if (!app.replay_pending) return;
    if (rf::Now100ns() - app.replay_touched < rf::MsTo100ns(450)) return;

    const bool armed = app.replay_target;
    const bool already =
        armed ? app.recorder.state() != rf::Recorder::State::Idle : app.recorder.state() == rf::Recorder::State::Idle;
    app.replay_pending = false;
    if (already) return;

    app.settings.Save(rf::paths::SettingsFile());
    PostEngine([armed] {
    App& app = *g_app;

    if (armed) {
        if (auto s = app.recorder.ArmReplay(); !s.ok()) {
            RF_ERROR("arm replay: {}", s.str());
            app.hud.Push(rf::ui::Hud::Kind::Error, "Откаты недоступны");
            app.settings.replay_enabled = false;
            app.model.replay_armed = false;
            return;
        }
        app.hud.Push(rf::ui::Hud::Kind::ReplayArmed, "Мгновенный повтор включен");
    } else {

        if (app.recorder.state() == rf::Recorder::State::Recording) return;
        app.recorder.DisarmReplay();
        app.hud.Push(rf::ui::Hud::Kind::ReplayArmed, "Мгновенный повтор выключен");
    }
    });
}

void RecoverFromDeviceLoss() {
    App& app = *g_app;
    if (app.needs_rebind) return;
    app.needs_rebind = true;

    PostEngine([] {
        App& app = *g_app;
        RF_WARN("rebuilding the GPU pipeline after a device loss");

        const bool was_armed = app.recorder.state() != rf::Recorder::State::Idle;
        if (app.recorder.state() == rf::Recorder::State::Recording)
            app.recorder.StopRecording();
        app.recorder.Shutdown();

        if (auto s = app.recorder.Init(app.settings); !s.ok()) {
            RF_ERROR("device recovery failed: {}", s.str());
            app.quit = true;
            return;
        }

        app.needs_rebind = false;
        app.pending_rebind = true;

        if (was_armed && app.settings.replay_enabled) {
            if (auto s = app.recorder.ArmReplay(); !s.ok())
                RF_ERROR("re-arm after recovery: {}", s.str());
        }
        app.hud.Push(rf::ui::Hud::Kind::Error, "Драйвер перезапустился — запись восстановлена");
        RF_INFO("GPU pipeline rebuilt");
    });
}

void OnHotkey(int id) {
    RF_INFO("hotkey {} fired (menu {} -> toggling)", id, g_app->menu.open() ? "open" : "closed");
    switch (id) {
        case kHotkeyOverlay: g_app->menu.ToggleOpen(); break;
        case kHotkeyRecord:  ToggleRecording(); break;
        case kHotkeyReplay:  SaveReplay(); break;
        case kHotkeyEscape:
            if (g_app->menu.player_open())
                g_app->menu.ClosePlayer(g_app->model);
            else if (g_app->menu.open())
                g_app->menu.Close();
            break;
        default: break;
    }
}

LRESULT CALLBACK TrayProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_RF_TRAY:
            if (LOWORD(lparam) == WM_LBUTTONUP || LOWORD(lparam) == WM_LBUTTONDBLCLK)
                g_app->menu.Open();
            if (LOWORD(lparam) == WM_RBUTTONUP) {
                POINT pt{};
                ::GetCursorPos(&pt);
                HMENU menu = ::CreatePopupMenu();
                ::AppendMenuW(menu, MF_STRING, kMenuOpen, L"Открыть reframe++\tAlt+Z");
                ::AppendMenuW(menu, MF_STRING, kMenuFolder, L"Папка с записями");
                ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                ::AppendMenuW(menu, MF_STRING, kMenuQuit, L"Выход");
                ::SetForegroundWindow(hwnd);
                ::TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
                ::DestroyMenu(menu);
            }
            return 0;

        case WM_COMMAND:
            switch (LOWORD(wparam)) {
                case kMenuOpen:   g_app->menu.Open(); break;
                case kMenuFolder: ShellOpen(g_app->settings.output_dir); break;
                case kMenuQuit:   g_app->quit = true; break;
                default: break;
            }
            return 0;

        default:
            return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

HWND CreateTrayWindow(HINSTANCE instance) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = TrayProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"ReframeTray";
    ::RegisterClassExW(&wc);
    return ::CreateWindowExW(0, wc.lpszClassName, L"Reframe", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                             instance, nullptr);
}

}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    rf::log::Init(rf::log::Level::Info);
    ::AddVectoredExceptionHandler(1, CrashReporter);

    HANDLE single = ::CreateMutexW(nullptr, TRUE, L"Local\\ReframePlusPlusSingleton");
    if (single && ::GetLastError() == ERROR_ALREADY_EXISTS) {
        RF_WARN("another instance is already running");
        return 0;
    }

    App app;
    g_app = &app;
    app.settings = rf::Settings::Load(rf::paths::SettingsFile());

    std::error_code ec;
    std::filesystem::create_directories(app.settings.output_dir, ec);

    if (auto s = app.recorder.Init(app.settings); !s.ok()) {
        RF_ERROR("engine init failed: {}", s.str());
        ::MessageBoxW(nullptr, rf::ToWide(s.str()).c_str(), L"reframe++", MB_ICONERROR);
        return 1;
    }

    app.applied_gpu_luid_low = app.settings.gpu_luid_low;
    app.applied_gpu_luid_high = app.settings.gpu_luid_high;

    app.gallery.Start(app.settings.output_dir);

    rf::D3DDevicePtr ui_device;
    if (auto s = rf::D3DDevice::CreateForOutput(nullptr, ui_device); !s.ok()) {
        RF_ERROR("overlay device: {}", s.str());
        ::MessageBoxW(nullptr, rf::ToWide(s.str()).c_str(), L"reframe++", MB_ICONERROR);
        return 1;
    }

    if (auto s = app.overlay.Create(ui_device, FindAssetDir()); !s.ok()) {
        RF_ERROR("overlay failed: {}", s.str());
        ::MessageBoxW(nullptr, rf::ToWide(s.str()).c_str(), L"reframe++", MB_ICONERROR);
        return 1;
    }
    if (auto s = app.player.Init(ui_device); s.ok()) {
        app.model.player = &app.player;
    } else {

        RF_WARN("in-app player unavailable ({}) - falling back to the system player", s.str());
    }

    app.overlay.on_hotkey = OnHotkey;
    app.overlay.on_escape = [] {

        if (g_app->menu.player_open())
            g_app->menu.ClosePlayer(g_app->model);
        else if (g_app->menu.open())
            g_app->menu.Close();
    };
    app.hud.on_stop = [] {
        if (g_app->recorder.state() == rf::Recorder::State::Recording) ToggleRecording();
    };

    app.tray_hwnd = CreateTrayWindow(instance);
    app.tray.cbSize = sizeof(app.tray);
    app.tray.hWnd = app.tray_hwnd;
    app.tray.uID = 1;
    app.tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    app.tray.uCallbackMessage = WM_RF_TRAY;

    app.tray.hIcon = ::LoadIconW(::GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON));
    if (!app.tray.hIcon) app.tray.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(app.tray.szTip, L"reframe++");
    ::Shell_NotifyIconW(NIM_ADD, &app.tray);

    auto register_hotkey = [&](int id, std::uint32_t mods, std::uint32_t vk,
                               std::initializer_list<std::pair<std::uint32_t, std::uint32_t>> alts,
                               const char* name) -> std::pair<std::uint32_t, std::uint32_t> {
        if (::RegisterHotKey(app.overlay.hwnd(), id, mods | MOD_NOREPEAT, vk)) return {mods, vk};
        for (const auto& [alt_mods, alt_vk] : alts) {
            if (::RegisterHotKey(app.overlay.hwnd(), id, alt_mods | MOD_NOREPEAT, alt_vk)) {
                RF_WARN("hotkey '{}' was taken - using the fallback combination instead", name);
                return {alt_mods, alt_vk};
            }
        }
        RF_ERROR("no hotkey could be registered for '{}'", name);
        return {0, 0};
    };

    std::tie(app.settings.hotkey_overlay_mods, app.settings.hotkey_overlay_vk) = register_hotkey(
        kHotkeyOverlay, app.settings.hotkey_overlay_mods, app.settings.hotkey_overlay_vk,
        {{MOD_CONTROL | MOD_ALT, 'Z'}, {MOD_CONTROL | MOD_SHIFT, 'Z'}}, "overlay");
    std::tie(app.settings.hotkey_toggle_record_mods, app.settings.hotkey_toggle_record_vk) =
        register_hotkey(kHotkeyRecord, app.settings.hotkey_toggle_record_mods,
                        app.settings.hotkey_toggle_record_vk,
                        {{MOD_CONTROL | MOD_ALT, VK_F9}, {MOD_CONTROL | MOD_SHIFT, VK_F9}},
                        "record");
    std::tie(app.settings.hotkey_save_replay_mods, app.settings.hotkey_save_replay_vk) =
        register_hotkey(kHotkeyReplay, app.settings.hotkey_save_replay_mods,
                        app.settings.hotkey_save_replay_vk,
                        {{MOD_CONTROL | MOD_ALT, VK_F10}, {MOD_CONTROL | MOD_SHIFT, VK_F10}},
                        "replay");

    app.model.on_hotkeys_changed = []() -> bool {
        App& a = *g_app;
        ::UnregisterHotKey(a.overlay.hwnd(), kHotkeyOverlay);
        ::UnregisterHotKey(a.overlay.hwnd(), kHotkeyRecord);
        ::UnregisterHotKey(a.overlay.hwnd(), kHotkeyReplay);

        const bool ok =
            ::RegisterHotKey(a.overlay.hwnd(), kHotkeyOverlay,
                             a.settings.hotkey_overlay_mods | MOD_NOREPEAT,
                             a.settings.hotkey_overlay_vk) &&
            ::RegisterHotKey(a.overlay.hwnd(), kHotkeyRecord,
                             a.settings.hotkey_toggle_record_mods | MOD_NOREPEAT,
                             a.settings.hotkey_toggle_record_vk) &&
            ::RegisterHotKey(a.overlay.hwnd(), kHotkeyReplay,
                             a.settings.hotkey_save_replay_mods | MOD_NOREPEAT,
                             a.settings.hotkey_save_replay_vk);

        a.model.record_hotkey = rf::ui::DescribeHotkey(a.settings.hotkey_toggle_record_mods,
                                                       a.settings.hotkey_toggle_record_vk);
        a.model.replay_hotkey = rf::ui::DescribeHotkey(a.settings.hotkey_save_replay_mods,
                                                       a.settings.hotkey_save_replay_vk);
        if (!ok) RF_WARN("a hotkey combination was refused by Windows");
        return ok;
    };

    app.overlay.on_key = [](std::uint32_t vk, std::uint32_t mods) -> bool {
        if (!g_app->menu.capturing_key()) return false;
        g_app->model.pending_vk = vk;
        g_app->model.pending_mods = mods;
        return true;
    };

    app.model.gpus.push_back({"Авто", 0, 0});
    for (const rf::AdapterInfo& info : rf::EnumerateAdapters()) {
        if (info.is_software) continue;
        app.model.gpus.push_back(
            {rf::ToUtf8(info.description), info.luid_low, info.luid_high});
    }

    RefreshMonitors();

    app.model.record_hotkey = rf::ui::DescribeHotkey(app.settings.hotkey_toggle_record_mods,
                                                     app.settings.hotkey_toggle_record_vk);
    app.model.replay_hotkey = rf::ui::DescribeHotkey(app.settings.hotkey_save_replay_mods,
                                                     app.settings.hotkey_save_replay_vk);

    app.model.settings = &app.settings;
    app.model.gallery = &app.gallery;
    app.model.on_toggle_record = ToggleRecording;
    app.model.on_pick_mic = PickMicDevice;
    RefreshMicLabel();
    RefreshMicDevices();
    app.model.on_save_replay = SaveReplay;
    app.model.on_set_replay = SetReplayArmed;
    app.model.on_settings_changed = SettingsTouched;
    app.model.on_open_file = [](const std::filesystem::path& p) { ShellOpen(p); };
    app.model.on_reveal_file = [](const std::filesystem::path& p) { ShellReveal(p); };
    app.model.on_delete_file = [](const std::filesystem::path& p) { g_app->gallery.Remove(p); };
    app.model.on_pick_folder = [](std::filesystem::path& p) {
        return PickFolder(g_app->overlay.hwnd(), p);
    };
    app.model.on_close = [] { g_app->menu.Close(); };

    app.engine_running = true;
    app.engine = std::thread(EngineLoop);

    if (app.settings.replay_enabled) SetReplayArmed(true);

    RF_INFO("reframe++ 1.1.0 build 512, compiled {} {}", __DATE__, __TIME__);
    RF_INFO("reframe++ ready - Alt+Z opens the overlay");

    app.watchdog = std::thread(WatchdogLoop);

    while (!app.quit && (app.stage = "pump", app.overlay.PumpMessages())) {
        app.ticks.fetch_add(1, std::memory_order_relaxed);

        MSG msg;
        while (::PeekMessageW(&msg, app.tray_hwnd, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }

        app.stage = "settings";
        FlushSettings();
        ApplyReplayState();

        const bool recording = app.recorder.state() == rf::Recorder::State::Recording;
        app.model.recording = recording;

        if (!app.replay_pending)
            app.model.replay_armed = app.settings.replay_enabled &&
                                     app.recorder.state() != rf::Recorder::State::Idle;
        app.model.recording_seconds =
            recording ? rf::Ticks100nsToMs(rf::Now100ns() - app.recording_started) / 1000.0 : 0.0;

        static rf::Ticks100ns last_poll = 0;
        const rf::Ticks100ns now_poll = rf::Now100ns();
        const bool poll_tick = now_poll - last_poll > rf::kOneSecond100ns;
        if (poll_tick) {
            last_poll = now_poll;
            app.stage = "disk-usage";
            DiskUsage(app.settings.output_dir, app.model.disk_used_bytes,
                      app.model.disk_total_bytes);

            DWM_TIMING_INFO timing{};
            timing.cbSize = sizeof(timing);
            if (SUCCEEDED(::DwmGetCompositionTimingInfo(nullptr, &timing)) &&
                timing.rateRefresh.uiDenominator && timing.rateRefresh.uiNumerator) {
                app.model.display_hz = static_cast<double>(timing.rateRefresh.uiNumerator) /
                                       timing.rateRefresh.uiDenominator;

                static double recent_hz[3] = {0.0, 0.0, 0.0};
                static int recent_index = 0;
                recent_hz[recent_index] = app.model.display_hz;
                recent_index = (recent_index + 1) % 3;
                const double peak_hz = std::max({recent_hz[0], recent_hz[1], recent_hz[2]});

                if (app.settings.ClampFpsToDisplay(peak_hz)) {
                    RF_INFO("frame rate lowered to {} - the display runs at {:.0f} Hz",
                            app.settings.ResolvedFps(), peak_hz);
                    app.settings_dirty = true;
                }
            }
        }

        FollowCursorAcrossMonitors();

        app.hud.SetRecording(recording && !app.menu.open(), app.model.recording_seconds,
                             app.recorder.target_app());

        if (now_poll - app.gallery_polled > rf::kOneSecond100ns / 5) {
            app.gallery_polled = now_poll;
            app.stage = "gallery-poll";
            app.model.gallery_items = app.gallery.Snapshot();

            for (const auto& item : app.model.gallery_items) {
                if (item.thumb_ready && !item.thumb_uploaded && !item.thumbnail.empty() &&
                    !app.overlay.textures().Contains(item.display_name)) {
                    app.overlay.textures().FromRgba(item.display_name, item.thumbnail.data(),
                                                    static_cast<int>(item.thumb_width),
                                                    static_cast<int>(item.thumb_height));
                    app.gallery.MarkUploaded(item.path);
                    break;
                }
            }
        }

        static bool was_menu_open = false;
        if (app.menu.open() && !was_menu_open) {
            RefreshMicDevices();
            RefreshMonitors();
        }
        was_menu_open = app.menu.open();

        app.stage = "window-style";

        const bool drawn_in_game = app.recorder.capture_is_hooked();
        app.overlay.SetInteractive(app.menu.open() && !drawn_in_game);

        static bool escape_registered = false;
        if (app.menu.open() != escape_registered) {
            escape_registered = app.menu.open();
            if (escape_registered)
                ::RegisterHotKey(app.overlay.hwnd(), kHotkeyEscape, MOD_NOREPEAT, VK_ESCAPE);
            else
                ::UnregisterHotKey(app.overlay.hwnd(), kHotkeyEscape);
        }

        SetKeyCaptureHook(app.menu.capturing_key());

        const bool in_game_menu = drawn_in_game && app.menu.open();
        SetMouseCaptureHook(in_game_menu);
        app.overlay.SetExternalMouse(
            in_game_menu,
            ImVec2(static_cast<float>(g_mouse_x.load(std::memory_order_relaxed)),
                   static_cast<float>(g_mouse_y.load(std::memory_order_relaxed))),
            g_mouse_down.load(std::memory_order_relaxed),
            static_cast<float>(g_mouse_wheel.exchange(0, std::memory_order_relaxed)));

        const bool want_mirror = app.recorder.capture_is_hooked();
        if (auto s = app.overlay.SetMirrorToSharedSurface(want_mirror); !s.ok())
            RF_WARN("overlay mirror: {}", s.str());
        if (want_mirror) {
            app.recorder.SetInGameOverlay(app.overlay.shared_surface_handle(),
                                          app.overlay.shared_surface_serial(),
                                          app.overlay.width(), app.overlay.height(),
                                          app.menu.visible() || app.hud.busy());
        }

        static rf::Ticks100ns last_raise = 0;
        if ((app.menu.visible() || app.hud.busy()) &&
            rf::Now100ns() - last_raise > rf::MsTo100ns(250)) {
            last_raise = rf::Now100ns();
            app.overlay.BringToTop();
        }

        bool over_pill = false;
        if (!app.menu.open() && recording) {
            POINT cursor{};

            const float scale = app.overlay.ui_scale();
            const ImVec4 r = app.hud.pill_rect();
            if (r.z > r.x && ::GetCursorPos(&cursor))
                over_pill = cursor.x >= r.x * scale - 4 && cursor.x <= r.z * scale + 4 &&
                            cursor.y >= r.y * scale - 4 && cursor.y <= r.w * scale + 4;
        }
        app.overlay.SetClickable(over_pill);

        const bool anything_visible = app.menu.visible() || app.hud.busy();
        app.stage = "visibility";
        app.overlay.SetVisible(anything_visible);
        if (!anything_visible) {
            app.stage = "idle";
            ::Sleep(20);
            continue;
        }

        app.overlay.SetUiScale(app.settings.ui_scale);

        app.stage = "render";
        if (rf::ui::UiContext* ctx = app.overlay.BeginFrame()) {
            const ImVec2 screen = app.overlay.design_size();
            app.menu.Draw(*ctx, screen, app.model, app.overlay.textures());
            app.hud.Draw(*ctx, screen, app.overlay.textures());
            app.stage = "present";
            app.overlay.EndFrame();

            app.stage = "shape";
            if (app.menu.visible() && !drawn_in_game)
                app.overlay.SetShape({});
            else
                app.overlay.SetShape(app.hud.hit_rects());
        }

        if (app.pending_rebind.exchange(false)) {
            if (auto s = app.overlay.RebindDevice(app.recorder.device()); !s.ok()) {
                RF_ERROR("overlay recovery failed: {}", s.str());
                app.quit = true;
            }
        }

        if (app.overlay.device_lost() || (app.recorder.device() && !app.recorder.device()->alive()))
            RecoverFromDeviceLoss();

        ::Sleep(4);
    }

    RF_INFO("shutting down (quit={}, pump alive={})", app.quit, app.overlay.hwnd() != nullptr);

    app.engine_running = false;
    app.task_cv.notify_all();
    if (app.engine.joinable()) app.engine.join();
    if (app.watchdog.joinable()) app.watchdog.join();

    app.settings.Save(rf::paths::SettingsFile());
    app.gallery.Stop();
    app.recorder.Shutdown();
    app.overlay.Destroy();
    ::Shell_NotifyIconW(NIM_DELETE, &app.tray);
    rf::log::Shutdown();
    ::CoUninitialize();
    if (single) ::CloseHandle(single);
    return 0;
}
