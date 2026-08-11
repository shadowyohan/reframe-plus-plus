#include "rf/gpu/VendorApi.h"

#include <windows.h>

#include <format>
#include <string>

#include "rf/core/Log.h"
#include "rf/gpu/GpuInfo.h"

namespace rf::vendor {
namespace {

constexpr unsigned kNvApiInitialize = 0x0150E828;
constexpr unsigned kNvApiUnload = 0xD22BDD7E;
constexpr unsigned kNvApiGetDriverAndBranchVersion = 0x2926AAAD;

using NvQueryInterfaceFn = void*(__cdecl*)(unsigned);
using NvInitFn = int(__cdecl*)();
using NvUnloadFn = int(__cdecl*)();
using NvDriverVerFn = int(__cdecl*)(unsigned*, char[64]);

struct Nvapi {
    HMODULE dll = nullptr;
    NvQueryInterfaceFn query = nullptr;
    bool initialized = false;

    Nvapi() {
        dll = ::LoadLibraryW(L"nvapi64.dll");
        if (!dll) return;
        query = reinterpret_cast<NvQueryInterfaceFn>(
            ::GetProcAddress(dll, "nvapi_QueryInterface"));
        if (!query) return;
        if (auto init = reinterpret_cast<NvInitFn>(query(kNvApiInitialize)))
            initialized = (init() == 0);
        RF_DEBUG("NVAPI load: dll={} init={}", dll != nullptr, initialized);
    }

    ~Nvapi() {
        if (initialized && query)
            if (auto unload = reinterpret_cast<NvUnloadFn>(query(kNvApiUnload))) unload();
        if (dll) ::FreeLibrary(dll);
    }

    template <typename Fn>
    Fn Get(unsigned id) const {
        return query ? reinterpret_cast<Fn>(query(id)) : nullptr;
    }
};

const Nvapi& GetNvapi() {
    static const Nvapi instance;
    return instance;
}

std::wstring NvidiaBranding() {
    const auto& nv = GetNvapi();
    if (!nv.initialized) return {};

    auto fn = nv.Get<NvDriverVerFn>(kNvApiGetDriverAndBranchVersion);
    if (!fn) return {};

    unsigned version = 0;
    char branch[64] = {};
    if (fn(&version, branch) != 0) return {};

    const auto text = std::format(L"{}.{:02}", version / 100, version % 100);
    RF_DEBUG("NVIDIA driver {}.{:02} branch {}", version / 100, version % 100, branch);
    return text;
}

std::wstring AmdBranding() {
    static constexpr const wchar_t* kKeys[] = {
        L"SOFTWARE\\AMD\\CN",
        L"SOFTWARE\\ATI Technologies\\Install",
    };
    static constexpr const wchar_t* kValues[] = {
        L"RadeonSoftwareVersion",
        L"DriverVersion",
    };

    for (const wchar_t* key : kKeys) {
        for (const wchar_t* value : kValues) {
            wchar_t buffer[128] = {};
            DWORD size = sizeof(buffer);
            if (::RegGetValueW(HKEY_LOCAL_MACHINE, key, value, RRF_RT_REG_SZ, nullptr, buffer,
                               &size) == ERROR_SUCCESS &&
                buffer[0]) {
                return buffer;
            }
        }
    }
    return {};
}

bool ModuleAvailable(const wchar_t* name) {
    HMODULE h = ::LoadLibraryExW(name, nullptr, LOAD_LIBRARY_AS_DATAFILE);
    if (!h) return false;
    ::FreeLibrary(h);
    return true;
}

}

void EnrichAdapter(AdapterInfo& info) {
    switch (info.vendor) {
        case GpuVendor::Nvidia: info.driver_branding = NvidiaBranding(); break;
        case GpuVendor::Amd:    info.driver_branding = AmdBranding(); break;
        default: break;
    }
}

bool NvapiAvailable() { return GetNvapi().initialized; }

bool AmdDriverAvailable() {
    return ModuleAvailable(L"amfrt64.dll") || ModuleAvailable(L"atiadlxx.dll");
}

}
