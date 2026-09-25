#pragma once
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "rf/core/Status.h"

namespace rf {

struct Release {
    std::string tag;
    std::string url;
};

[[nodiscard]] bool IsNewerVersion(std::string_view candidate, std::string_view current);
[[nodiscard]] std::optional<Release> ParseLatestRelease(std::string_view json);
Status FetchLatestRelease(Release& out);

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
