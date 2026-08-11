#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace rf {

enum class GpuVendor { Unknown, Nvidia, Amd, Intel, Microsoft };

const char* ToString(GpuVendor v);

struct DriverVersion {
    std::uint16_t product = 0, major = 0, minor = 0, build = 0;
    [[nodiscard]] std::string str() const;

    [[nodiscard]] std::uint64_t packed() const;
    auto operator<=>(const DriverVersion&) const = default;
};

struct AdapterInfo {
    std::uint32_t index = 0;
    std::wstring description;
    GpuVendor vendor = GpuVendor::Unknown;
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;
    std::uint64_t dedicated_vram = 0;
    std::int32_t luid_low = 0;
    std::int32_t luid_high = 0;

    DriverVersion driver;
    std::wstring driver_branding;
    bool is_software = false;
};

std::vector<AdapterInfo> EnumerateAdapters();

bool FindAdapterForOutput(void* hwnd, AdapterInfo& out);

}
