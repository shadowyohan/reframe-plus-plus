#include "rf/capture/GameWatcher.h"

#include <windows.h>

#include <dwmapi.h>

#include <algorithm>
#include <array>

#include "rf/core/Log.h"
#include "rf/core/Strings.h"

namespace rf {
namespace {

constexpr DWORD kPollMs = 300;

constexpr std::int64_t kSwitchDelayMs = 2000;

constexpr std::array kNeverHook = {
    L"explorer.exe", L"applicationframehost.exe", L"shellexperiencehost.exe",
    L"startmenuexperiencehost.exe", L"searchhost.exe", L"searchapp.exe", L"textinputhost.exe",
    L"lockapp.exe", L"dwm.exe", L"sihost.exe", L"reframe.exe",
};

std::wstring LowerLeaf(const std::wstring& path) {
    const std::size_t slash = path.find_last_of(L'\\');
    std::wstring leaf = slash == std::wstring::npos ? path : path.substr(slash + 1);
    std::transform(leaf.begin(), leaf.end(), leaf.begin(), ::towlower);
    return leaf;
}

bool IsCloaked(HWND wnd) {

    BOOL cloaked = FALSE;
    return SUCCEEDED(::DwmGetWindowAttribute(wnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
           cloaked;
}

bool UnderSystemRoot(const std::wstring& path) {
    wchar_t root[MAX_PATH]{};
    if (!::GetWindowsDirectoryW(root, MAX_PATH)) return false;
    const std::size_t n = wcslen(root);
    return path.size() > n && _wcsnicmp(path.c_str(), root, n) == 0;
}

std::int64_t NowMs() { return static_cast<std::int64_t>(::GetTickCount64()); }

}

const char* ToString(GameVerdict v) {
    switch (v) {
        case GameVerdict::Game:          return "game";
        case GameVerdict::NoWindow:      return "no usable window";
        case GameVerdict::OwnProcess:    return "reframe++ itself";
        case GameVerdict::Shell:         return "the desktop or the shell";
        case GameVerdict::NotVisible:    return "hidden, minimised or cloaked";
        case GameVerdict::TooSmall:      return "window too small";
        case GameVerdict::SystemProcess: return "a Windows component";
        case GameVerdict::KnownNonGame:  return "the shell or reframe++ itself";
        case GameVerdict::NotAppWindow:  return "not a top-level application window";
    }
    return "?";
}

GameVerdict ClassifyWindow(void* hwnd_raw, GameWindow& out, bool strict) {
    HWND wnd = static_cast<HWND>(hwnd_raw);
    if (!wnd || !::IsWindow(wnd)) return GameVerdict::NoWindow;
    if (wnd == ::GetDesktopWindow() || wnd == ::GetShellWindow()) return GameVerdict::Shell;

    if (strict) {

        if (::GetWindow(wnd, GW_OWNER) != nullptr) return GameVerdict::NotAppWindow;
        if (::GetWindowLongW(wnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return GameVerdict::NotAppWindow;
        if (::GetWindowTextLengthW(wnd) == 0) return GameVerdict::NotAppWindow;
    }

    DWORD pid = 0;
    ::GetWindowThreadProcessId(wnd, &pid);
    if (!pid) return GameVerdict::NoWindow;
    if (pid == ::GetCurrentProcessId()) return GameVerdict::OwnProcess;

    DWORD shell_pid = 0;
    if (HWND shell = ::GetShellWindow()) ::GetWindowThreadProcessId(shell, &shell_pid);
    if (pid == shell_pid) return GameVerdict::Shell;

    if (!::IsWindowVisible(wnd) || ::IsIconic(wnd) || IsCloaked(wnd)) return GameVerdict::NotVisible;

    RECT client{};
    if (!::GetClientRect(wnd, &client)) return GameVerdict::NoWindow;
    const LONG w = client.right - client.left;
    const LONG h = client.bottom - client.top;

    if (w < 320 || h < 240) return GameVerdict::TooSmall;

    HANDLE process =
        ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process) {

        return GameVerdict::SystemProcess;
    }

    wchar_t path[MAX_PATH]{};
    DWORD len = MAX_PATH;
    const bool have_path = ::QueryFullProcessImageNameW(process, 0, path, &len) != 0;
    const std::wstring full = have_path ? std::wstring(path, len) : std::wstring();
    const std::wstring leaf = LowerLeaf(full);

    GameVerdict verdict = GameVerdict::Game;
    if (!have_path || UnderSystemRoot(full)) {
        verdict = GameVerdict::SystemProcess;
    } else if (std::find(kNeverHook.begin(), kNeverHook.end(), leaf) != kNeverHook.end()) {
        verdict = GameVerdict::KnownNonGame;
    }
    ::CloseHandle(process);

    if (verdict != GameVerdict::Game) return verdict;

    out.hwnd = wnd;
    out.pid = pid;
    out.exe = ToUtf8(leaf);
    out.width = static_cast<std::uint32_t>(w);
    out.height = static_cast<std::uint32_t>(h);
    return GameVerdict::Game;
}

bool FindRecordableBelow(GameWindow& out) {
    struct Scan {
        GameWindow* out;
        bool found;
    } scan{&out, false};

    ::EnumWindows(
        [](HWND wnd, LPARAM param) -> BOOL {
            auto* s = reinterpret_cast<Scan*>(param);
            if (ClassifyWindow(wnd, *s->out, true) != GameVerdict::Game) return TRUE;
            s->found = true;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&scan));

    return scan.found;
}

GameWatcher::~GameWatcher() { Stop(); }

void GameWatcher::Start(std::function<void(const GameWindow&)> on_change) {
    if (running_.load(std::memory_order_acquire)) return;
    on_change_ = std::move(on_change);
    wake_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { Loop(); });
}

void GameWatcher::Stop() {
    if (running_.exchange(false, std::memory_order_acq_rel)) {
        if (wake_) ::SetEvent(wake_);
        if (thread_.joinable()) thread_.join();
    }
    if (wake_) {
        ::CloseHandle(wake_);
        wake_ = nullptr;
    }
}

GameWindow GameWatcher::current() const {
    std::scoped_lock lock(mutex_);
    return current_;
}

void GameWatcher::Loop() {
    ::SetThreadDescription(::GetCurrentThread(), L"rf-game-watch");

    GameVerdict last_verdict = GameVerdict::Game;
    HWND last_seen = nullptr;

    GameWindow pending;
    std::int64_t pending_since = 0;

    while (running_.load(std::memory_order_acquire)) {
        GameWindow found;
        HWND fg = ::GetForegroundWindow();
        GameVerdict verdict = ClassifyWindow(fg, found);

        if (verdict != GameVerdict::Game && (fg != last_seen || verdict != last_verdict)) {
            RF_DEBUG("game watcher: foreground rejected - {}", ToString(verdict));
        }

        if (verdict != GameVerdict::Game && FindRecordableBelow(found)) {
            if (fg != last_seen)
                RF_DEBUG("game watcher: falling through to \"{}\" behind the foreground",
                         found.exe);
            verdict = GameVerdict::Game;
        }
        last_seen = fg;
        last_verdict = verdict;

        GameWindow next;
        {
            std::scoped_lock lock(mutex_);
            next = current_;
        }

        if (verdict == GameVerdict::Game) {
            if (found.SameAs(next)) {

                pending = GameWindow{};
            } else if (!found.SameAs(pending)) {
                pending = found;
                pending_since = NowMs();
            } else if (NowMs() - pending_since >= kSwitchDelayMs || !next.valid()) {

                next = found;
                pending = GameWindow{};
            }
        } else if (next.valid()) {
            pending = GameWindow{};

            HANDLE p = ::OpenProcess(SYNCHRONIZE, FALSE, next.pid);
            const bool alive =
                p && ::WaitForSingleObject(p, 0) == WAIT_TIMEOUT && ::IsWindow(static_cast<HWND>(next.hwnd));
            if (p) ::CloseHandle(p);
            if (!alive) next = GameWindow{};
        }

        bool changed = false;
        {
            std::scoped_lock lock(mutex_);
            changed = !next.SameAs(current_);
            current_ = next;
        }

        if (changed) {
            if (next.valid())
                RF_INFO("game watcher: now \"{}\" (pid {}, {}x{})", next.exe, next.pid, next.width,
                        next.height);
            else
                RF_INFO("game watcher: no game");
            if (on_change_) on_change_(next);
        }

        if (wake_) ::WaitForSingleObject(wake_, kPollMs);
    }
}

}
