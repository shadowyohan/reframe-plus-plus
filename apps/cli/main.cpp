#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rf/core/Log.h"
#include "rf/core/Paths.h"
#include "rf/core/Strings.h"
#include "rf/encode/IVideoEncoder.h"
#include "rf/engine/Recorder.h"

namespace {

LONG CALLBACK CrashReporter(EXCEPTION_POINTERS* info) {
    const auto code = info->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_BREAKPOINT || code == DBG_PRINTEXCEPTION_C ||
        code == DBG_PRINTEXCEPTION_WIDE_C)
        return EXCEPTION_CONTINUE_SEARCH;

    void* address = info->ExceptionRecord->ExceptionAddress;
    wchar_t module_path[MAX_PATH] = L"<unknown>";
    HMODULE module = nullptr;
    if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             static_cast<LPCWSTR>(address), &module))
        ::GetModuleFileNameW(module, module_path, MAX_PATH);

    RF_ERROR("EXCEPTION 0x{:08X} at {} in {}", static_cast<unsigned>(code), address,
             rf::ToUtf8(module_path));
    return EXCEPTION_CONTINUE_SEARCH;
}

void Print(const std::string& text) {
    std::fputs((text + "\n").c_str(), stdout);

    std::fflush(stdout);
}

int Diagnose() {
    Print("Reframe++ diagnostics");
    Print("---------------------");

    const auto adapters = rf::EnumerateAdapters();
    if (adapters.empty()) {
        Print("no DXGI adapters found - is a display driver installed?");
        return 1;
    }

    for (const auto& a : adapters) {
        Print(std::format("GPU {}: {} [{}]", a.index, rf::ToUtf8(a.description),
                          rf::ToString(a.vendor)));
        Print(std::format("  VRAM        : {} MiB", a.dedicated_vram / (1024 * 1024)));
        Print(std::format("  LUID        : {} {}", a.luid_low, a.luid_high));
        Print(std::format("  UMD version : {}", a.driver.str()));
        if (!a.driver_branding.empty())
            Print(std::format("  Driver      : {}", rf::ToUtf8(a.driver_branding)));
    }

    Print("");
    Print(std::format("Windows.Graphics.Capture : {}",
                      rf::IsWgcSupported() ? "supported" : "NOT supported (using duplication)"));

    rf::D3DDevicePtr device;
    if (auto s = rf::D3DDevice::CreateForOutput(nullptr, device); !s.ok()) {
        Print(std::format("D3D11 device creation failed: {}", s.str()));
        return 1;
    }

    Print("");
    Print("Hardware encoders:");
    for (const auto& cap : rf::ProbeEncoders(device))
        Print(std::format("  [{}] {} - {}", rf::ToString(cap.backend), rf::ToString(cap.codec),
                          cap.name));

    Print("");
    Print(std::format("Driver quirks applied: {}", rf::DetectQuirks(device->info()).describe()));
    Print(std::format("Logs: {}", rf::paths::LogDir().string()));
    return 0;
}

void PrintStatus(const rf::Recorder::Status_& s) {

    Print(std::format(
        "  capture {} ({} skipped, {} recov) | encode {} frames, {:.1f} MB, {:.2f} ms/frame, "
        "{} dropped | ring {:.1f}s / {:.0f} MB",
        s.capture.frames_captured, s.capture.frames_repeated, s.capture.recoveries,
        s.encode.frames_encoded,
        s.encode.bytes_out / 1048576.0, s.encode.avg_encode_ms, s.encode.frames_dropped,
        rf::Ticks100nsToMs(s.replay.duration) / 1000.0, s.replay.bytes / 1048576.0));
}

int ProbeCapture(int seconds, rf::CaptureBackend backend) {
    rf::D3DDevicePtr device;
    if (auto s = rf::D3DDevice::CreateForOutput(nullptr, device); !s.ok()) {
        Print(std::format("device: {}", s.str()));
        return 1;
    }

    rf::CaptureTarget probe_target;
    rf::VideoCapturePtr capture;
    if (auto s = rf::CreateVideoCapture(device, backend, probe_target, capture); !s.ok()) {
        Print(std::format("capture: {}", s.str()));
        return 1;
    }

    std::atomic<std::uint64_t> count{0};
    std::mutex mutex;
    std::vector<rf::Ticks100ns> stamps;
    stamps.reserve(4096);

    if (auto s = capture->Start(probe_target, [&](const rf::CapturedFrame& f) {
            count.fetch_add(1, std::memory_order_relaxed);
            std::scoped_lock lock(mutex);
            if (stamps.size() < 4096) stamps.push_back(f.timestamp);
        });
        !s.ok()) {
        Print(std::format("start: {}", s.str()));
        return 1;
    }

    Print("Move the mouse / play a video to make the screen update.");
    std::uint64_t previous = 0;
    for (int i = 0; i < seconds; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const std::uint64_t now = count.load();
        Print(std::format("  {} frames/s delivered by {}", now - previous,
                          rf::ToString(capture->backend())));
        previous = now;
    }
    capture->Stop();

    std::scoped_lock lock(mutex);
    if (stamps.size() > 2) {
        double sum = 0.0, min_d = 1e9, max_d = 0.0;
        for (std::size_t i = 1; i < stamps.size(); ++i) {
            const double d = rf::Ticks100nsToMs(stamps[i] - stamps[i - 1]);
            sum += d;
            min_d = (std::min)(min_d, d);
            max_d = (std::max)(max_d, d);
        }
        const double mean = sum / (stamps.size() - 1);
        Print(std::format("  inter-arrival: mean {:.2f} ms ({:.1f} fps), min {:.2f}, max {:.2f}",
                          mean, 1000.0 / mean, min_d, max_d));
    }
    return 0;
}

int RunFor(rf::Recorder& recorder, int seconds, bool recording) {
    if (recording) {
        if (auto s = recorder.StartRecording({}); !s.ok()) {
            Print(std::format("start failed: {}", s.str()));
            return 1;
        }
    } else if (auto s = recorder.ArmReplay(); !s.ok()) {
        Print(std::format("arm failed: {}", s.str()));
        return 1;
    }

    for (int i = 0; i < seconds; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        PrintStatus(recorder.GetStatus());
    }
    return 0;
}

}

int wmain(int argc, wchar_t** argv) {
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    rf::log::Init(rf::log::Level::Debug);
    ::AddVectoredExceptionHandler(1, CrashReporter);

    const std::string command = argc > 1 ? rf::ToUtf8(argv[1]) : "diagnose";
    const int seconds = argc > 2 ? _wtoi(argv[2]) : 10;

    int exit_code = 0;

    if (command == "diagnose") {
        exit_code = Diagnose();
    } else if (command == "capture") {
        auto backend = rf::CaptureBackend::Auto;
        if (argc > 3) {
            const std::string which = rf::ToUtf8(argv[3]);
            if (which == "dda") backend = rf::CaptureBackend::DesktopDuplication;
            if (which == "wgc") backend = rf::CaptureBackend::WindowsGraphicsCapture;
        }
        exit_code = ProbeCapture(seconds, backend);
    } else {
        auto settings = rf::Settings::Load(rf::paths::SettingsFile());

        rf::Recorder recorder;
        if (auto s = recorder.Init(settings); !s.ok()) {
            Print(std::format("init failed: {}", s.str()));
            exit_code = 1;
        } else if (command == "record") {
            exit_code = RunFor(recorder, seconds, true);
            recorder.StopRecording();
        } else if (command == "replay") {
            exit_code = RunFor(recorder, seconds, false);
            std::filesystem::path saved;
            if (auto save = recorder.SaveReplay(&saved); save.ok())
                Print(std::format("saved: {}", saved.string()));
            else
                Print(std::format("save failed: {}", save.str()));
        } else if (command == "bench") {
            settings.replay_enabled = false;
            exit_code = RunFor(recorder, seconds, false);
        } else {
            Print("usage: reframe-cli [diagnose|capture <s>|record <s>|replay <s>|bench <s>]");
            exit_code = 2;
        }
        recorder.Shutdown();
    }

    rf::log::Shutdown();
    ::CoUninitialize();
    return exit_code;
}
