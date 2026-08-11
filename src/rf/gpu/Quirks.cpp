#include "rf/gpu/Quirks.h"

#include <windows.h>

#include <algorithm>
#include <format>

#include "rf/core/Log.h"

namespace rf {
namespace {

std::uint32_t WindowsBuild() {
    static const std::uint32_t build = [] {

        DWORD value = 0, size = sizeof(value);
        ::RegGetValueW(HKEY_LOCAL_MACHINE,
                       L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"CurrentBuildNumber",
                       RRF_RT_REG_SZ, nullptr, nullptr, &size);
        wchar_t buffer[32] = {};
        size = sizeof(buffer);
        if (::RegGetValueW(HKEY_LOCAL_MACHINE,
                           L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                           L"CurrentBuildNumber", RRF_RT_REG_SZ, nullptr, buffer,
                           &size) == ERROR_SUCCESS) {
            return static_cast<std::uint32_t>(_wtoi(buffer));
        }
        return 0u;
    }();
    return build;
}

void Add(QuirkSet& set, Quirk::Id id, std::string reason) {
    set.quirks.push_back(Quirk{id, std::move(reason)});
}

}

bool QuirkSet::has(Quirk::Id id) const {
    return std::any_of(quirks.begin(), quirks.end(), [id](const Quirk& q) { return q.id == id; });
}

std::string QuirkSet::describe() const {
    if (quirks.empty()) return "none";
    std::string out;
    for (const auto& q : quirks) {
        if (!out.empty()) out += "; ";
        out += q.reason;
    }
    return out;
}

QuirkSet DetectQuirks(const AdapterInfo& adapter) {
    QuirkSet set;
    const std::uint32_t build = WindowsBuild();

    Add(set, Quirk::Id::DuplRecreateOnModeChange,
        "recreate Desktop Duplication on ACCESS_LOST / mode change");

    Add(set, Quirk::Id::DuplIgnoreZeroPresentTime,
        "treat LastPresentTime == 0 as 'no new frame' to avoid duplicated frames");

    if (build && build < 20348)
        Add(set, Quirk::Id::WgcBorderUnsupported,
            std::format("Windows build {} predates WGC border control", build));

    switch (adapter.vendor) {
        case GpuVendor::Nvidia:

            Add(set, Quirk::Id::EncoderLimitAsyncDepth,
                "cap NVENC in-flight frames at 4 to bound capture->encode latency");
            break;

        case GpuVendor::Amd:

            Add(set, Quirk::Id::DisableBFrames,
                "disable B-frames on AMF until per-VCN-generation support is probed");
            break;

        case GpuVendor::Intel:
            Add(set, Quirk::Id::DisableBFrames, "conservative default for QSV low-power mode");
            break;

        default:
            break;
    }

    if (adapter.vendor != GpuVendor::Unknown && adapter.dedicated_vram == 0)
        Add(set, Quirk::Id::ForceEncoderOnCaptureAdapter,
            "integrated adapter drives the output - keep the encoder on it");

    RF_INFO("quirks for {}: {}", ToString(adapter.vendor), set.describe());
    return set;
}

}
