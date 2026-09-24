#include "rf/audio/MaxineSetup.h"

#include <windows.h>

#include <shellapi.h>
#include <winhttp.h>

#include <algorithm>
#include <format>
#include <fstream>
#include <memory>
#include <vector>

#include "rf/audio/NoiseSuppressor.h"
#include "rf/core/Log.h"

#pragma comment(lib, "winhttp.lib")

namespace rf {
namespace {

constexpr int kCuComputeCapabilityMajor = 75;
constexpr int kCuComputeCapabilityMinor = 76;

constexpr DWORD kReadChunk = 1 << 20;

struct InternetHandleCloser {
    void operator()(HINTERNET handle) const { ::WinHttpCloseHandle(handle); }
};
using InternetHandle = std::unique_ptr<void, InternetHandleCloser>;

Status DownloadToFile(const std::wstring& url, const std::filesystem::path& target,
                      std::atomic<float>& progress) {
    wchar_t host[256]{};
    wchar_t path[2048]{};
    URL_COMPONENTS parts{sizeof(parts)};
    parts.lpszHostName = host;
    parts.dwHostNameLength = ARRAYSIZE(host);
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = ARRAYSIZE(path);
    if (!::WinHttpCrackUrl(url.c_str(), 0, 0, &parts))
        return Status::Fail(HRESULT_FROM_WIN32(::GetLastError()), "bad NVIDIA Maxine URL");

    const auto failed = [](const char* what) {
        return Status::Fail(HRESULT_FROM_WIN32(::GetLastError()), what);
    };
    InternetHandle session(::WinHttpOpen(L"reframe++", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                         WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) return failed("WinHttpOpen");
    InternetHandle connection(::WinHttpConnect(session.get(), host, parts.nPort, 0));
    if (!connection) return failed("WinHttpConnect");
    InternetHandle request(::WinHttpOpenRequest(
        connection.get(), L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
    if (!request) return failed("WinHttpOpenRequest");
    if (!::WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                              WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !::WinHttpReceiveResponse(request.get(), nullptr))
        return failed("NVIDIA Maxine download request");

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    ::WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                          WINHTTP_NO_HEADER_INDEX);
    if (status != 200) return Status::Fail(std::format("NVIDIA Maxine download: HTTP {}", status));

    std::uint64_t expected = 0;
    DWORD expected_size = sizeof(expected);
    ::WinHttpQueryHeaders(request.get(),
                          WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER64,
                          WINHTTP_HEADER_NAME_BY_INDEX, &expected, &expected_size,
                          WINHTTP_NO_HEADER_INDEX);

    std::ofstream file(target, std::ios::binary | std::ios::trunc);
    if (!file) return Status::Fail("cannot create the NVIDIA Maxine installer file");

    std::vector<char> buffer(kReadChunk);
    std::uint64_t received = 0;
    for (;;) {
        DWORD read = 0;
        if (!::WinHttpReadData(request.get(), buffer.data(), kReadChunk, &read))
            return failed("NVIDIA Maxine download interrupted");
        if (read == 0) break;
        file.write(buffer.data(), read);
        if (!file) return Status::Fail("cannot write the NVIDIA Maxine installer - disk full?");
        received += read;
        if (expected > 0)
            progress.store(std::min(1.0f, static_cast<float>(received) / static_cast<float>(expected)));
    }
    file.close();

    if (expected > 0 && received != expected)
        return Status::Fail(std::format("NVIDIA Maxine download stopped at {} of {} bytes",
                                        received, expected));
    return Status::Ok();
}

Status RunInstallerAndWait(const std::filesystem::path& installer) {
    SHELLEXECUTEINFOW info{sizeof(info)};
    std::error_code ec;
    if (!std::filesystem::exists(installer, ec))
        return Status::Fail("the NVIDIA Maxine installer vanished after the download - "
                            "an antivirus may have removed it");
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"open";
    info.lpFile = installer.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!::ShellExecuteExW(&info) || !info.hProcess)
        return Status::Fail(HRESULT_FROM_WIN32(::GetLastError()), "cannot start the Maxine installer");
    ::WaitForSingleObject(info.hProcess, INFINITE);
    ::CloseHandle(info.hProcess);
    return Status::Ok();
}

}

RtxGeneration RtxGenerationFromComputeCapability(int major, int minor) {
    if (major == 7 && minor >= 5) return RtxGeneration::Turing;
    if (major == 8 && minor == 9) return RtxGeneration::Ada;
    if (major == 8) return RtxGeneration::Ampere;
    if (major >= 10) return RtxGeneration::Blackwell;
    return RtxGeneration::Unsupported;
}

RtxGeneration DetectRtxGeneration() {
    using CuInit = int(__stdcall*)(unsigned);
    using CuDeviceGetCount = int(__stdcall*)(int*);
    using CuDeviceGet = int(__stdcall*)(int*, int);
    using CuDeviceGetAttribute = int(__stdcall*)(int*, int, int);

    HMODULE cuda = ::LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!cuda) return RtxGeneration::Unsupported;

    const auto init = reinterpret_cast<CuInit>(::GetProcAddress(cuda, "cuInit"));
    const auto count = reinterpret_cast<CuDeviceGetCount>(::GetProcAddress(cuda, "cuDeviceGetCount"));
    const auto get = reinterpret_cast<CuDeviceGet>(::GetProcAddress(cuda, "cuDeviceGet"));
    const auto attribute =
        reinterpret_cast<CuDeviceGetAttribute>(::GetProcAddress(cuda, "cuDeviceGetAttribute"));

    RtxGeneration best = RtxGeneration::Unsupported;
    int devices = 0;
    if (init && count && get && attribute && init(0) == 0 && count(&devices) == 0) {
        for (int i = 0; i < devices; ++i) {
            int device = 0, major = 0, minor = 0;
            if (get(&device, i) != 0 || attribute(&major, kCuComputeCapabilityMajor, device) != 0 ||
                attribute(&minor, kCuComputeCapabilityMinor, device) != 0)
                continue;
            best = std::max(best, RtxGenerationFromComputeCapability(major, minor));
        }
    }
    ::FreeLibrary(cuda);
    return best;
}

const wchar_t* MaxineInstallerUrl(RtxGeneration generation) {
    switch (generation) {
        case RtxGeneration::Turing:
            return L"https://international.download.nvidia.com/Windows/broadcast/sdk/AFX/"
                   L"2025-01-21_NVIDIA_AFX_SDK_Win_v1.6.1.2-GA_Turing.exe";
        case RtxGeneration::Ampere:
            return L"https://international.download.nvidia.com/Windows/broadcast/sdk/AFX/"
                   L"2025-01-21_NVIDIA_AFX_SDK_Win_v1.6.1.2-GA_Ampere.exe";
        case RtxGeneration::Ada:
            return L"https://international.download.nvidia.com/Windows/broadcast/sdk/AFX/"
                   L"2025-01-21_NVIDIA_AFX_SDK_Win_v1.6.1.2-GA_Ada.exe";
        case RtxGeneration::Blackwell:
            return L"https://international.download.nvidia.com/Windows/broadcast/sdk/AFX/"
                   L"2025-01-21_NVIDIA_AFX_SDK_Win_v1.6.1.2-GA_Blackwell.exe";
        case RtxGeneration::Unsupported:
            break;
    }
    return nullptr;
}

MaxineSetup::~MaxineSetup() {
    if (worker_.joinable()) worker_.join();
}

Status MaxineSetup::Start(const std::filesystem::path& download_dir) {
    if (running_) return Status::Ok();
    const wchar_t* url = MaxineInstallerUrl(DetectRtxGeneration());
    if (!url) return Status::Fail("NVIDIA Maxine needs an RTX graphics card");

    if (worker_.joinable()) worker_.join();
    std::error_code ec;
    std::filesystem::create_directories(download_dir, ec);

    progress_ = 0.0f;
    running_ = true;
    worker_ = std::thread([this, url = std::wstring(url), target = download_dir / L"nvidia-maxine-setup.exe"] {
        Run(url, target);
    });
    return Status::Ok();
}

void MaxineSetup::Run(std::wstring url, std::filesystem::path target) {
    ::SetThreadDescription(::GetCurrentThread(), L"rf-maxine-setup");
    RF_INFO("downloading NVIDIA Maxine from {}", std::string(url.begin(), url.end()));
    Status result = DownloadToFile(url, target, progress_);
    if (result.ok()) {
        progress_ = 1.0f;
        result = RunInstallerAndWait(target);
        if (result.ok() && !MaxineInstalled())
            result = Status::Fail("NVIDIA Maxine setup did not finish");
    }

    std::error_code ec;
    std::filesystem::remove(target, ec);

    if (result.ok()) RF_INFO("NVIDIA Maxine installed at {}", MaxineSdkDir().string());
    else RF_WARN("{}", result.str());

    running_ = false;
    if (on_finished) on_finished(result);
}

}
