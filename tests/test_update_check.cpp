#include "rf/engine/UpdateCheck.h"

#include "test_framework.h"

using namespace rf;

TEST(UpdateCheck_ComparesVersionsNumerically) {
    CHECK(IsNewerVersion("v2.2", "2.1.0"));
    CHECK(IsNewerVersion("v2.10", "2.9"));
    CHECK(IsNewerVersion("3.0", "2.99.99"));
    CHECK(IsNewerVersion("v2.1.1", "2.1"));
    CHECK(!IsNewerVersion("v2.1", "2.1.0"));
    CHECK(!IsNewerVersion("v2.0", "2.1.0"));
    CHECK(!IsNewerVersion("", "2.1.0"));
}

TEST(UpdateCheck_ReadsTheTagAndPageOfARelease) {
    const auto release = ParseLatestRelease(
        R"({"url": "https://api.github.com/x", "html_url": "https://github.com/shadowyohan/reframe-plus-plus/releases/tag/v2.2", "tag_name": "v2.2", "author": {"html_url": "https://github.com/shadowyohan"}})");
    CHECK(release.has_value());
    CHECK(release->tag == "v2.2");
    CHECK(release->url == "https://github.com/shadowyohan/reframe-plus-plus/releases/tag/v2.2");
    CHECK(!ParseLatestRelease(R"({"message": "Not Found"})").has_value());
}

TEST(UpdateCheck_AsksGitHubForTheLatestRelease) {
    Release release;
    if (const Status fetched = FetchLatestRelease(release); !fetched.ok()) {
        SKIP("GitHub is not reachable from here");
        return;
    }
    CHECK(!release.tag.empty());
    CHECK(release.url.find("github.com/shadowyohan/reframe-plus-plus/releases") != std::string::npos);
}
