#include "rf/gpu/GpuInfo.h"

#include <windows.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <format>

#include <wrl/client.h>

#include "rf/core/Log.h"
#include "rf/core/Strings.h"
#include "rf/gpu/VendorApi.h"

#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace rf {
namespace {

GpuVendor VendorFromId(std::uint32_t id) {
    switch (id) {
        case 0x10DE: return GpuVendor::Nvidia;
        case 0x1002:
        case 0x1022: return GpuVendor::Amd;
        case 0x8086: return GpuVendor::Intel;
        case 0x1414: return GpuVendor::Microsoft;
        default:     return GpuVendor::Unknown;
    }
}

}

const char* ToString(GpuVendor v) {
    switch (v) {
        case GpuVendor::Nvidia:    return "NVIDIA";
        case GpuVendor::Amd:       return "AMD";
        case GpuVendor::Intel:     return "Intel";
        case GpuVendor::Microsoft: return "Microsoft";
        case GpuVendor::Unknown:   return "Unknown";
    }
    return "Unknown";
}

std::string DriverVersion::str() const {
    return std::format("{}.{}.{}.{}", product, major, minor, build);
}

std::uint64_t DriverVersion::packed() const {
    return (static_cast<std::uint64_t>(product) << 48) | (static_cast<std::uint64_t>(major) << 32) |
           (static_cast<std::uint64_t>(minor) << 16) | build;
}

std::vector<AdapterInfo> EnumerateAdapters() {
    std::vector<AdapterInfo> result;

    ComPtr<IDXGIFactory6> factory;
    if (FAILED(::CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        RF_ERROR("CreateDXGIFactory2 failed");
        return result;
    }

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0;
         factory->EnumAdapters1(i, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc))) continue;

        AdapterInfo info;
        info.index = i;

        info.description.assign(desc.Description,
                                ::wcsnlen(desc.Description, ARRAYSIZE(desc.Description)));
        info.vendor_id = desc.VendorId;
        info.device_id = desc.DeviceId;
        info.vendor = VendorFromId(desc.VendorId);
        info.dedicated_vram = desc.DedicatedVideoMemory;
        info.luid_low = static_cast<std::int32_t>(desc.AdapterLuid.LowPart);
        info.luid_high = desc.AdapterLuid.HighPart;
        info.is_software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;

        LARGE_INTEGER umd{};
        if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
            info.driver.product = static_cast<std::uint16_t>((umd.QuadPart >> 48) & 0xFFFF);
            info.driver.major = static_cast<std::uint16_t>((umd.QuadPart >> 32) & 0xFFFF);
            info.driver.minor = static_cast<std::uint16_t>((umd.QuadPart >> 16) & 0xFFFF);
            info.driver.build = static_cast<std::uint16_t>(umd.QuadPart & 0xFFFF);
        }

        vendor::EnrichAdapter(info);

        RF_INFO("adapter {}: {} [{}] vram={} MiB driver={} {}", i, ToUtf8(info.description),
                ToString(info.vendor), info.dedicated_vram / (1024 * 1024), info.driver.str(),
                ToUtf8(info.driver_branding));

        result.push_back(std::move(info));
    }
    return result;
}

bool FindAdapterForOutput(void* hwnd, AdapterInfo& out) {
    const auto adapters = EnumerateAdapters();
    if (adapters.empty()) return false;

    HMONITOR monitor = hwnd ? ::MonitorFromWindow(static_cast<HWND>(hwnd), MONITOR_DEFAULTTONEAREST)
                            : ::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);

    ComPtr<IDXGIFactory6> factory;
    if (SUCCEEDED(::CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0;
             factory->EnumAdapters1(i, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND;
             ++i) {
            ComPtr<IDXGIOutput> output;
            for (UINT o = 0; adapter->EnumOutputs(o, output.ReleaseAndGetAddressOf()) !=
                             DXGI_ERROR_NOT_FOUND;
                 ++o) {
                DXGI_OUTPUT_DESC od{};
                if (SUCCEEDED(output->GetDesc(&od)) && od.Monitor == monitor && i < adapters.size()) {
                    out = adapters[i];
                    return true;
                }
            }
        }
    }

    out = adapters.front();
    return true;
}

namespace {

BOOL CALLBACK CollectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM param) {
    auto& out = *reinterpret_cast<std::vector<MonitorInfo>*>(param);

    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!::GetMonitorInfoW(monitor, &info)) return TRUE;

    MonitorInfo entry;
    entry.device_name = info.szDevice;
    entry.handle = monitor;
    entry.x = info.rcMonitor.left;
    entry.y = info.rcMonitor.top;
    entry.width = static_cast<std::uint32_t>(info.rcMonitor.right - info.rcMonitor.left);
    entry.height = static_cast<std::uint32_t>(info.rcMonitor.bottom - info.rcMonitor.top);
    entry.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;

    DISPLAY_DEVICEW device{};
    device.cb = sizeof(device);
    if (::EnumDisplayDevicesW(info.szDevice, 0, &device, 0) && device.DeviceString[0])
        entry.description = device.DeviceString;
    else
        entry.description = info.szDevice;

    out.push_back(std::move(entry));
    return TRUE;
}

}

std::vector<MonitorInfo> EnumerateMonitors() {
    std::vector<MonitorInfo> out;
    ::EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, reinterpret_cast<LPARAM>(&out));

    std::sort(out.begin(), out.end(), [](const MonitorInfo& a, const MonitorInfo& b) {
        if (a.primary != b.primary) return a.primary;
        return a.x < b.x;
    });
    return out;
}

void* MonitorForDeviceName(const std::wstring& device_name) {
    if (!device_name.empty()) {
        for (const MonitorInfo& info : EnumerateMonitors())
            if (info.device_name == device_name) return info.handle;
        RF_WARN("the configured display is not attached - using the primary one");
    }
    return ::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
}

}
