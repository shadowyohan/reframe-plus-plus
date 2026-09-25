#include "rf/audio/AppAudioTracks.h"

#include <windows.h>

#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <format>
#include <map>

#include <wrl/client.h>

#include "rf/core/Log.h"
#include "rf/core/Strings.h"

#pragma comment(lib, "version.lib")

using Microsoft::WRL::ComPtr;

namespace rf {
namespace {

constexpr auto kWatchInterval = std::chrono::milliseconds(500);

struct ProcessEntry {
    std::uint32_t parent = 0;
    std::wstring exe;
};

std::map<std::uint32_t, ProcessEntry> SnapshotProcesses() {
    std::map<std::uint32_t, ProcessEntry> processes;
    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return processes;
    PROCESSENTRY32W entry{sizeof(entry)};
    for (BOOL more = ::Process32FirstW(snapshot, &entry); more;
         more = ::Process32NextW(snapshot, &entry)) {
        std::wstring exe = entry.szExeFile;
        std::transform(exe.begin(), exe.end(), exe.begin(), ::towlower);
        processes[entry.th32ProcessID] = {entry.th32ParentProcessID, std::move(exe)};
    }
    ::CloseHandle(snapshot);
    return processes;
}

std::uint32_t RootOfSameExe(std::uint32_t pid,
                            const std::map<std::uint32_t, ProcessEntry>& processes) {
    for (int depth = 0; depth < 16; ++depth) {
        const auto self = processes.find(pid);
        if (self == processes.end()) break;
        const auto parent = processes.find(self->second.parent);
        if (parent == processes.end() || parent->first == pid ||
            parent->second.exe != self->second.exe)
            break;
        pid = parent->first;
    }
    return pid;
}

std::string FriendlyProcessName(std::uint32_t pid) {
    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return std::format("PID {}", pid);
    wchar_t path[MAX_PATH]{};
    DWORD length = MAX_PATH;
    const bool resolved = ::QueryFullProcessImageNameW(process, 0, path, &length);
    ::CloseHandle(process);
    if (!resolved) return std::format("PID {}", pid);

    const std::string fallback = ToUtf8(std::filesystem::path(path).stem().wstring());
    DWORD ignored = 0;
    const DWORD size = ::GetFileVersionInfoSizeW(path, &ignored);
    if (size == 0) return fallback;
    std::vector<std::uint8_t> info(size);
    if (!::GetFileVersionInfoW(path, 0, size, info.data())) return fallback;

    struct Translation {
        WORD language;
        WORD codepage;
    };
    Translation* translation = nullptr;
    UINT translation_bytes = 0;
    if (!::VerQueryValueW(info.data(), L"\\VarFileInfo\\Translation",
                          reinterpret_cast<void**>(&translation), &translation_bytes) ||
        translation_bytes < sizeof(Translation))
        return fallback;

    const std::wstring key = std::format(L"\\StringFileInfo\\{:04x}{:04x}\\FileDescription",
                                         translation->language, translation->codepage);
    wchar_t* description = nullptr;
    UINT description_chars = 0;
    if (!::VerQueryValueW(info.data(), key.c_str(), reinterpret_cast<void**>(&description),
                          &description_chars) ||
        description_chars <= 1)
        return fallback;
    return ToUtf8(description);
}

}

AppAudioTracks::~AppAudioTracks() { Stop(); }

Status AppAudioTracks::Start(std::uint32_t slots, std::uint32_t first_track,
                             std::uint32_t bitrate_bps, Ticks100ns epoch,
                             const std::function<void(PacketPtr)>& on_packet) {
    Stop();
    for (std::uint32_t i = 0; i < slots; ++i) {
        auto slot = std::make_unique<Slot>();
        RF_TRY(slot->track.Open(first_track + i, bitrate_bps, epoch, on_packet));
        slots_.push_back(std::move(slot));
    }

    watching_ = true;
    watcher_ = std::thread([this] { WatchLoop(); });
    RF_INFO("{} application audio tracks from track {}", slots, first_track + 1);
    return Status::Ok();
}

void AppAudioTracks::Stop() {
    if (watching_.exchange(false)) wake_.notify_all();
    if (watcher_.joinable()) watcher_.join();

    for (auto& slot : slots_) slot->track.Close();
    slots_.clear();
    std::scoped_lock lock(slots_mutex_);
    overflowed_.clear();
    overflow_queue_.clear();
}

void AppAudioTracks::Feed(std::uint32_t frames, Ticks100ns timestamp) {
    for (auto& slot : slots_) slot->track.Feed(frames, timestamp);
}

std::vector<std::string> AppAudioTracks::track_names() const {
    std::scoped_lock lock(slots_mutex_);
    std::vector<std::string> names;
    for (const auto& slot : slots_) names.push_back(slot->name);
    return names;
}

std::vector<bool> AppAudioTracks::AudibleSince(Ticks100ns pts) const {
    std::vector<bool> audible;
    for (const auto& slot : slots_) audible.push_back(slot->track.AudibleSince(pts));
    return audible;
}

std::vector<std::string> AppAudioTracks::TakeOverflowedApps() {
    std::scoped_lock lock(slots_mutex_);
    return std::exchange(overflow_queue_, {});
}

std::uint32_t AppAudioTracks::PickSlot(const std::vector<std::uint32_t>& assigned_roots,
                                       std::uint32_t root_pid, std::uint32_t slots) {
    for (std::uint32_t i = 0; i < slots && i < assigned_roots.size(); ++i)
        if (assigned_roots[i] == root_pid) return i;
    for (std::uint32_t i = 0; i < slots; ++i)
        if (i >= assigned_roots.size() || assigned_roots[i] == 0) return i;
    return slots;
}

std::vector<AudioApp> AppAudioTracks::ActiveAudioApps() {
    std::vector<AudioApp> apps;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&enumerator))))
        return apps;
    ComPtr<IMMDeviceCollection> endpoints;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &endpoints)))
        return apps;

    const auto processes = SnapshotProcesses();
    const std::uint32_t self = ::GetCurrentProcessId();
    std::set<std::uint32_t> seen;

    UINT endpoint_count = 0;
    endpoints->GetCount(&endpoint_count);
    for (UINT e = 0; e < endpoint_count; ++e) {
        ComPtr<IMMDevice> endpoint;
        ComPtr<IAudioSessionManager2> manager;
        ComPtr<IAudioSessionEnumerator> sessions;
        if (FAILED(endpoints->Item(e, &endpoint)) ||
            FAILED(endpoint->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                      &manager)) ||
            FAILED(manager->GetSessionEnumerator(&sessions)))
            continue;

        int session_count = 0;
        sessions->GetCount(&session_count);
        for (int i = 0; i < session_count; ++i) {
            ComPtr<IAudioSessionControl> control;
            ComPtr<IAudioSessionControl2> control2;
            AudioSessionState state = AudioSessionStateInactive;
            DWORD pid = 0;
            if (FAILED(sessions->GetSession(i, &control)) || FAILED(control.As(&control2)) ||
                control2->IsSystemSoundsSession() == S_OK || FAILED(control2->GetState(&state)) ||
                state != AudioSessionStateActive || FAILED(control2->GetProcessId(&pid)) ||
                pid == 0 || pid == self)
                continue;

            const std::uint32_t root = RootOfSameExe(pid, processes);
            if (root == self || !seen.insert(root).second) continue;
            apps.push_back({root, FriendlyProcessName(root)});
        }
    }
    return apps;
}

void AppAudioTracks::WatchLoop() {
    ::SetThreadDescription(::GetCurrentThread(), L"rf-app-audio");
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (watching_) {
        for (const AudioApp& app : ActiveAudioApps()) Assign(app);
        std::unique_lock lock(wake_mutex_);
        wake_.wait_for(lock, kWatchInterval, [this] { return !watching_; });
    }
    ::CoUninitialize();
}

void AppAudioTracks::Assign(const AudioApp& app) {
    std::scoped_lock lock(slots_mutex_);
    std::vector<std::uint32_t> roots;
    for (const auto& slot : slots_) roots.push_back(slot->root_pid);

    const auto slot_count = static_cast<std::uint32_t>(slots_.size());
    const std::uint32_t index = PickSlot(roots, app.root_pid, slot_count);
    if (index == slot_count) {
        if (overflowed_.insert(app.root_pid).second) {
            RF_INFO("no free application track for {}", app.name);
            overflow_queue_.push_back(app.name);
        }
        return;
    }

    Slot& slot = *slots_[index];
    if (slot.root_pid == app.root_pid) return;
    slot.root_pid = app.root_pid;
    slot.name = app.name;

    if (auto s = slot.track.Capture(app.root_pid); !s.ok()) {
        RF_WARN("cannot capture {} on its own track: {}", app.name, s.str());
        return;
    }
    RF_INFO("audio of {} (PID {}) goes to application track {}", app.name, app.root_pid,
            index + 1);
}

}
