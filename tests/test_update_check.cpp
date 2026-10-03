#include <windows.h>

#include <filesystem>

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

TEST(UpdateCheck_FindsTheInstallerAndItsChecksum) {
    const auto release = ParseLatestRelease(R"({"html_url": "https://github.com/shadowyohan/reframe-plus-plus/releases/tag/v2.2", "tag_name": "v2.2", "assets": [
        {"name": "reframe++-2.2-standalone.zip", "size": 10, "digest": "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "browser_download_url": "https://github.com/shadowyohan/reframe-plus-plus/releases/download/v2.2/reframe%2B%2B-2.2-standalone.zip"},
        {"name": "reframe++-2.2-setup.exe", "uploader": {"login": "shadowyohan", "url": "x"}, "size": 29800107, "digest": "sha256:6f053dcbae405b4dd16be84c1d6af51c4b3dd1d0e0aec3121c5d4bda649533fd", "browser_download_url": "https://github.com/shadowyohan/reframe-plus-plus/releases/download/v2.2/reframe%2B%2B-2.2-setup.exe"}
    ]})");
    CHECK(release.has_value());
    CHECK(release->installer.has_value());
    CHECK(release->installer->name == "reframe++-2.2-setup.exe");
    CHECK_EQ(release->installer->size, 29800107ull);
    CHECK(release->installer->sha256 == "6f053dcbae405b4dd16be84c1d6af51c4b3dd1d0e0aec3121c5d4bda649533fd");
}

TEST(UpdateCheck_RefusesAnInstallerWithoutAChecksumOrFromElsewhere) {
    const auto unsigned_release = ParseLatestRelease(R"({"tag_name": "v2.2", "assets": [
        {"name": "reframe++-2.2-setup.exe", "size": 5, "browser_download_url": "https://github.com/shadowyohan/reframe-plus-plus/releases/download/v2.2/reframe-2.2-setup.exe"}]})");
    CHECK(unsigned_release.has_value());
    CHECK(!unsigned_release->installer.has_value());

    CHECK(!IsTrustedInstallerUrl("https://github.com/someone-else/reframe-plus-plus/releases/download/v2.2/x-setup.exe"));
    CHECK(!IsTrustedInstallerUrl("http://github.com/shadowyohan/reframe-plus-plus/releases/download/v2.2/x-setup.exe"));
    CHECK(!IsTrustedInstallerUrl("https://github.com/shadowyohan/reframe-plus-plus/releases/download/../../x-setup.exe"));
    CHECK(IsTrustedDownloadHost(L"release-assets.githubusercontent.com"));
    CHECK(IsTrustedDownloadHost(L"github.com"));
    CHECK(!IsTrustedDownloadHost(L"githubusercontent.com.evil.example"));
    CHECK(!IsTrustedDownloadHost(L"evilgithubusercontent.com"));
}

TEST(UpdateCheck_DownloadsAndVerifiesTheInstaller) {
    char flag[8] = {};
    if (::GetEnvironmentVariableA("RF_LIVE_DOWNLOAD", flag, sizeof(flag)) == 0) {
        SKIP("set RF_LIVE_DOWNLOAD=1 to download the real installer");
        return;
    }
    Release release;
    CHECK(FetchLatestRelease(release).ok());
    CHECK(release.installer.has_value());
    const auto dir = std::filesystem::temp_directory_path() / "reframe-update-test";
    std::filesystem::path saved;
    float last = 0.0f;
    const Status s = DownloadInstaller(*release.installer, dir, [&](float p) { last = p; }, saved);
    CHECK(s.ok());
    CHECK(std::filesystem::file_size(saved) == release.installer->size);
    CHECK(last > 0.99f);

    Installer tampered = *release.installer;
    tampered.sha256[0] = tampered.sha256[0] == 'a' ? 'b' : 'a';
    std::filesystem::path refused;
    CHECK(!DownloadInstaller(tampered, dir / "tampered", nullptr, refused).ok());
    CHECK(!std::filesystem::exists(dir / "tampered" / tampered.name));
    std::filesystem::remove_all(dir);
}
