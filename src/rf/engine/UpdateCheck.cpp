#include "rf/engine/UpdateCheck.h"

#include <windows.h>

#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <fstream>
#include <vector>

#include "rf/core/Log.h"
#include "rf/core/Strings.h"

#pragma comment(lib, "bcrypt.lib")

namespace rf {
namespace {

constexpr wchar_t kApiHost[] = L"api.github.com";
constexpr wchar_t kLatestRelease[] = L"/repos/shadowyohan/reframe-plus-plus/releases/latest";
constexpr auto kFirstCheckDelay = std::chrono::seconds(5);
constexpr auto kCheckInterval = std::chrono::minutes(10);
constexpr int kTimeoutMs = 10'000;
constexpr int kDownloadTimeoutMs = 30'000;
constexpr std::string_view kReleaseDownloads =
    "https://github.com/shadowyohan/reframe-plus-plus/releases/download/";
constexpr std::string_view kInstallerSuffix = "-setup.exe";
constexpr std::uint64_t kLargestInstaller = 512ull << 20;

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

std::optional<std::uint64_t> JsonNumber(std::string_view json, std::string_view key) {
    const std::string quoted = "\"" + std::string(key) + "\"";
    std::size_t at = json.find(quoted);
    if (at == std::string_view::npos) return std::nullopt;
    at = json.find(':', at + quoted.size());
    if (at == std::string_view::npos) return std::nullopt;
    ++at;
    while (at < json.size() && json[at] == ' ') ++at;
    if (at >= json.size() || json[at] < '0' || json[at] > '9') return std::nullopt;
    std::uint64_t value = 0;
    while (at < json.size() && json[at] >= '0' && json[at] <= '9')
        value = value * 10 + static_cast<std::uint64_t>(json[at++] - '0');
    return value;
}

std::vector<std::string_view> JsonObjectsIn(std::string_view json, std::string_view array_key) {
    std::vector<std::string_view> objects;
    const std::string quoted = "\"" + std::string(array_key) + "\"";
    std::size_t at = json.find(quoted);
    if (at == std::string_view::npos) return objects;
    at = json.find('[', at + quoted.size());
    if (at == std::string_view::npos) return objects;

    int depth = 0;
    bool in_string = false;
    std::size_t start = 0;
    for (std::size_t i = at + 1; i < json.size(); ++i) {
        const char c = json[i];
        if (in_string) {
            if (c == '\\') ++i;
            else if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') in_string = true;
        else if (c == '{' && depth++ == 0) start = i;
        else if (c == '}' && --depth == 0) objects.push_back(json.substr(start, i - start + 1));
        else if (c == ']' && depth == 0) break;
    }
    return objects;
}

bool IsSafeFileName(std::string_view name) {
    if (name.empty() || name.size() > 128 || !name.ends_with(kInstallerSuffix)) return false;
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '.' || c == '-' || c == '_' || c == '+';
    });
}

bool IsHexDigest(std::string_view hex) {
    return hex.size() == 64 && std::all_of(hex.begin(), hex.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

std::optional<Installer> FindInstaller(std::string_view json) {
    for (const std::string_view asset : JsonObjectsIn(json, "assets")) {
        Installer installer;
        installer.name = JsonString(asset, "name").value_or("");
        if (!IsSafeFileName(installer.name)) continue;
        installer.url = JsonString(asset, "browser_download_url").value_or("");
        installer.size = JsonNumber(asset, "size").value_or(0);
        const std::string digest = JsonString(asset, "digest").value_or("");
        if (digest.starts_with("sha256:")) installer.sha256 = digest.substr(7);
        if (!IsTrustedInstallerUrl(installer.url) || !IsHexDigest(installer.sha256) ||
            installer.size == 0 || installer.size > kLargestInstaller)
            continue;
        return installer;
    }
    return std::nullopt;
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
    release.installer = FindInstaller(json);
    return release;
}

bool IsTrustedInstallerUrl(std::string_view url) {
    if (!url.starts_with(kReleaseDownloads)) return false;
    const std::string_view rest = url.substr(kReleaseDownloads.size());
    return rest.find("..") == std::string_view::npos && rest.find('?') == std::string_view::npos &&
           rest.find('#') == std::string_view::npos;
}

bool IsTrustedDownloadHost(std::wstring_view host) {
    constexpr std::wstring_view kAssetHosts = L".githubusercontent.com";
    return host == L"github.com" ||
           (host.size() > kAssetHosts.size() && host.ends_with(kAssetHosts));
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

namespace {

class Sha256 {
public:
    Sha256() {
        if (BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM,
                                                         nullptr, 0)))
            ::BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0);
    }
    ~Sha256() {
        if (hash_) ::BCryptDestroyHash(hash_);
        if (algorithm_) ::BCryptCloseAlgorithmProvider(algorithm_, 0);
    }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    [[nodiscard]] bool ready() const { return hash_ != nullptr; }

    void Add(const char* data, std::size_t size) {
        ::BCryptHashData(hash_, reinterpret_cast<PUCHAR>(const_cast<char*>(data)),
                         static_cast<ULONG>(size), 0);
    }

    std::string Hex() {
        unsigned char digest[32] = {};
        ::BCryptFinishHash(hash_, digest, sizeof(digest), 0);
        std::string hex;
        for (const unsigned char byte : digest) hex += std::format("{:02x}", byte);
        return hex;
    }

private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
};

std::wstring FinalHost(HINTERNET request) {
    DWORD length = 0;
    ::WinHttpQueryOption(request, WINHTTP_OPTION_URL, nullptr, &length);
    std::wstring url(length / sizeof(wchar_t), L'\0');
    if (!::WinHttpQueryOption(request, WINHTTP_OPTION_URL, url.data(), &length)) return {};
    url.resize(length / sizeof(wchar_t));

    URL_COMPONENTS parts{sizeof(parts)};
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    if (!::WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS)
        return {};
    return std::wstring(parts.lpszHostName, parts.dwHostNameLength);
}

}

Status DownloadInstaller(const Installer& installer, const std::filesystem::path& dir,
                         const std::function<void(float)>& on_progress,
                         std::filesystem::path& saved_to) {
    if (!IsTrustedInstallerUrl(installer.url) || !IsSafeFileName(installer.name) ||
        !IsHexDigest(installer.sha256))
        return Status::Fail("the release does not offer an installer reframe++ can trust");

    const std::wstring wide_url = ToWide(installer.url);
    URL_COMPONENTS parts{sizeof(parts)};
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    if (!::WinHttpCrackUrl(wide_url.c_str(), 0, 0, &parts))
        return Status::Fail(HRESULT_FROM_WIN32(::GetLastError()), "WinHttpCrackUrl");
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    const std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);

    InternetHandle session{::WinHttpOpen(L"reframe++", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                         WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.handle)
        return Status::Fail(HRESULT_FROM_WIN32(::GetLastError()), "WinHttpOpen");
    ::WinHttpSetTimeouts(session.handle, kDownloadTimeoutMs, kDownloadTimeoutMs,
                         kDownloadTimeoutMs, kDownloadTimeoutMs);
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    ::WinHttpSetOption(session.handle, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects));

    InternetHandle connection{
        ::WinHttpConnect(session.handle, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0)};
    if (!connection.handle)
        return Status::Fail(HRESULT_FROM_WIN32(::GetLastError()), "WinHttpConnect");
    InternetHandle request{::WinHttpOpenRequest(connection.handle, L"GET", path.c_str(), nullptr,
                                                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                WINHTTP_FLAG_SECURE)};
    if (!request.handle)
        return Status::Fail(HRESULT_FROM_WIN32(::GetLastError()), "WinHttpOpenRequest");
    if (!::WinHttpSendRequest(request.handle, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                              WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !::WinHttpReceiveResponse(request.handle, nullptr))
        return Status::Fail(HRESULT_FROM_WIN32(::GetLastError()), "downloading the installer");

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    ::WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                          WINHTTP_NO_HEADER_INDEX);
    if (status != 200) return Status::Fail(std::format("the download answered {}", status));
    if (!IsTrustedDownloadHost(FinalHost(request.handle)))
        return Status::Fail("the download was redirected somewhere unexpected");

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path target = dir / ToWide(installer.name);
    std::filesystem::path partial = target;
    partial += L".part";

    Sha256 hash;
    if (!hash.ready()) return Status::Fail("SHA-256 is unavailable");
    std::uint64_t received_total = 0;
    {
        std::ofstream out(partial, std::ios::binary | std::ios::trunc);
        if (!out) return Status::Fail("cannot write the installer to the temporary folder");

        std::string chunk;
        for (DWORD available = 0;
             ::WinHttpQueryDataAvailable(request.handle, &available) && available > 0;) {
            chunk.resize(available);
            DWORD received = 0;
            if (!::WinHttpReadData(request.handle, chunk.data(), available, &received) ||
                received == 0)
                break;
            received_total += received;
            if (received_total > installer.size) break;
            hash.Add(chunk.data(), received);
            out.write(chunk.data(), received);
            if (on_progress)
                on_progress(static_cast<float>(static_cast<double>(received_total) /
                                               static_cast<double>(installer.size)));
        }
        if (!out) return Status::Fail("writing the installer failed");
    }

    if (received_total != installer.size || hash.Hex() != installer.sha256) {
        std::filesystem::remove(partial, ec);
        return Status::Fail("the downloaded installer does not match the release checksum");
    }

    std::filesystem::rename(partial, target, ec);
    if (ec) return Status::Fail(std::format("cannot finish the download: {}", ec.message()));
    saved_to = target;
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
