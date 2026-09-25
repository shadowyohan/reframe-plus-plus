#include "rf/engine/UpdateCheck.h"

#include <windows.h>

#include <winhttp.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <format>

#include "rf/core/Log.h"

namespace rf {
namespace {

constexpr wchar_t kApiHost[] = L"api.github.com";
constexpr wchar_t kLatestRelease[] = L"/repos/shadowyohan/reframe-plus-plus/releases/latest";
constexpr auto kFirstCheckDelay = std::chrono::seconds(5);
constexpr auto kCheckInterval = std::chrono::minutes(10);
constexpr int kTimeoutMs = 10'000;

std::array<std::uint32_t, 4> VersionParts(std::string_view version) {
    std::array<std::uint32_t, 4> parts{};
    std::size_t at = 0;
    while (at < version.size() && (version[at] < '0' || version[at] > '9')) ++at;
    for (std::size_t index = 0; index < parts.size() && at < version.size(); ++index) {
        while (at < version.size() && version[at] >= '0' && version[at] <= '9')
            parts[index] = parts[index] * 10 + static_cast<std::uint32_t>(version[at++] - '0');
        if (at >= version.size() || version[at] != '.') break;
        ++at;
    }
    return parts;
}

std::optional<std::string> JsonString(std::string_view json, std::string_view key) {
    const std::string quoted = "\"" + std::string(key) + "\"";
    std::size_t at = json.find(quoted);
    if (at == std::string_view::npos) return std::nullopt;
    at = json.find(':', at + quoted.size());
    if (at == std::string_view::npos) return std::nullopt;
    at = json.find('"', at);
    if (at == std::string_view::npos) return std::nullopt;
    const std::size_t end = json.find('"', at + 1);
    if (end == std::string_view::npos) return std::nullopt;
    return std::string(json.substr(at + 1, end - at - 1));
}

struct InternetHandle {
    HINTERNET handle = nullptr;
    ~InternetHandle() {
        if (handle) ::WinHttpCloseHandle(handle);
    }
};

}

bool IsNewerVersion(std::string_view candidate, std::string_view current) {
    return VersionParts(candidate) > VersionParts(current);
}

std::optional<Release> ParseLatestRelease(std::string_view json) {
    auto tag = JsonString(json, "tag_name");
    if (!tag || tag->empty()) return std::nullopt;
    Release release;
    release.tag = std::move(*tag);
    release.url = JsonString(json, "html_url").value_or("");
    return release;
}

Status FetchLatestRelease(Release& out) {
    InternetHandle session{::WinHttpOpen(L"reframe++", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                         WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.handle)
        return Status::Fail(HRESULT_FROM_WIN32(::GetLastError()), "WinHttpOpen");
    ::WinHttpSetTimeouts(session.handle, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);

    InternetHandle connection{
        ::WinHttpConnect(session.handle, kApiHost, INTERNET_DEFAULT_HTTPS_PORT, 0)};
    if (!connection.handle)
        return Status::Fail(HRESULT_FROM_WIN32(::GetLastError()), "WinHttpConnect");

    InternetHandle request{::WinHttpOpenRequest(connection.handle, L"GET", kLatestRelease, nullptr,
                                                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                WINHTTP_FLAG_SECURE)};
    if (!request.handle)
        return Status::Fail(HRESULT_FROM_WIN32(::GetLastError()), "WinHttpOpenRequest");

    constexpr wchar_t kHeaders[] = L"Accept: application/vnd.github+json\r\n";
    if (!::WinHttpSendRequest(request.handle, kHeaders, static_cast<DWORD>(-1L),
                              WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !::WinHttpReceiveResponse(request.handle, nullptr))
        return Status::Fail(HRESULT_FROM_WIN32(::GetLastError()), "requesting the latest release");

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    ::WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                          WINHTTP_NO_HEADER_INDEX);
    if (status != 200) return Status::Fail(std::format("GitHub answered {}", status));

    std::string body;
    for (DWORD available = 0;
         ::WinHttpQueryDataAvailable(request.handle, &available) && available > 0;) {
        const std::size_t offset = body.size();
        body.resize(offset + available);
        DWORD received = 0;
        if (!::WinHttpReadData(request.handle, body.data() + offset, available, &received)) break;
        body.resize(offset + received);
    }

    auto release = ParseLatestRelease(body);
    if (!release) return Status::Fail("the latest release has no tag");
    out = std::move(*release);
    return Status::Ok();
}

UpdateChecker::~UpdateChecker() { Stop(); }

void UpdateChecker::Start(std::string current_version) {
    std::scoped_lock lock(mutex_);
    if (running_) return;
    current_ = std::move(current_version);
    running_ = true;
    thread_ = std::thread([this] { Loop(); });
}

void UpdateChecker::Stop() {
    {
        std::scoped_lock lock(mutex_);
        running_ = false;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void UpdateChecker::Loop() {
    ::SetThreadDescription(::GetCurrentThread(), L"rf-update-check");
    auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(kFirstCheckDelay);
    while (true) {
        {
            std::unique_lock lock(mutex_);
            if (wake_.wait_for(lock, delay, [this] { return !running_; })) return;
        }
        delay = std::chrono::duration_cast<std::chrono::milliseconds>(kCheckInterval);

        Release release;
        if (auto s = FetchLatestRelease(release); !s.ok()) {
            RF_INFO("update check skipped: {}", s.str());
            continue;
        }
        if (!IsNewerVersion(release.tag, current_) || release.tag == announced_) continue;

        announced_ = release.tag;
        RF_INFO("reframe++ {} is available (running {})", release.tag, current_);
        if (on_update) on_update(release);
    }
}

}
