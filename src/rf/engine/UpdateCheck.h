#pragma once
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "rf/core/Status.h"

namespace rf {

struct Installer {
    std::string name;
    std::string url;
    std::string sha256;
    std::uint64_t size = 0;
};

struct Release {
    std::string tag;
    std::string url;
    std::optional<Installer> installer;
};

[[nodiscard]] bool IsNewerVersion(std::string_view candidate, std::string_view current);
[[nodiscard]] std::optional<Release> ParseLatestRelease(std::string_view json);
[[nodiscard]] bool IsTrustedInstallerUrl(std::string_view url);
[[nodiscard]] bool IsTrustedDownloadHost(std::wstring_view host);
Status FetchLatestRelease(Release& out);
Status DownloadInstaller(const Installer& installer, const std::filesystem::path& dir,
                         const std::function<void(float)>& on_progress,
                         std::filesystem::path& saved_to);

class UpdateChecker {
public:
    ~UpdateChecker();

    std::function<void(const Release&)> on_update;

    void Start(std::string current_version);
    void Stop();

private:
    void Loop();

    std::string current_;
    std::string announced_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool running_ = false;
};

}
