#include "borealis/update.hpp"
#include "update_internal.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

using borealis::AppInfo;
using borealis::update::Release;
using borealis::update::Status;
using borealis::update::Version;
namespace http = borealis::http;
namespace update = borealis::update;

namespace {

constexpr AppInfo TestApp{
    .orgName = "TwilitRealm",
    .appName = "Dusklight",
    .githubOwner = "TwilitRealm",
    .githubRepo = "dusklight",
    .discordApplicationId = "1495632471994405035",
};

std::string read_fixture(const char* name) {
    const std::string path = std::string(BOREALIS_TEST_FIXTURE_DIR) + "/" + name;
    std::ifstream stream(path, std::ios::binary);
    EXPECT_TRUE(stream.is_open());
    std::ostringstream contents;
    contents << stream.rdbuf();
    return contents.str();
}

Version parsed(std::string_view value) {
    const auto version = update::parse_version(value);
    if (!version) {
        ADD_FAILURE() << "Invalid test version: " << value;
        return {};
    }
    return *version;
}

int compare(std::string_view lhs, std::string_view rhs) {
    return update::compare_version(parsed(lhs), parsed(rhs));
}

http::Result ok_response(std::string body) {
    return {
        .response =
            {
                .statusCode = 200,
                .body = std::move(body),
            },
    };
}

TEST(Update, ParseBasics) {
    const Version version = parsed("1.2.3");
    EXPECT_EQ(version.major, 1);
    EXPECT_EQ(version.minor, 2);
    EXPECT_EQ(version.patch, 3);
    EXPECT_TRUE(version.prerelease.empty());

    EXPECT_EQ(parsed("v10.0.42").major, 10);
    EXPECT_EQ(parsed("v10.0.42").patch, 42);

    const Version prerelease = parsed("v1.0.0-rc.1");
    EXPECT_EQ(prerelease.prerelease.size(), 2);
    EXPECT_EQ(prerelease.prerelease[0], "rc");
    EXPECT_EQ(prerelease.prerelease[1], "1");

    // Build metadata is validated and discarded.
    EXPECT_TRUE(parsed("1.2.3+build.5").prerelease.empty());
    EXPECT_EQ(parsed("1.2.3-rc.1+build.5").prerelease.size(), 2);
}

TEST(Update, ParseRejectsMalformed) {
    EXPECT_FALSE(update::parse_version(""));
    EXPECT_FALSE(update::parse_version("v"));
    EXPECT_FALSE(update::parse_version("1"));
    EXPECT_FALSE(update::parse_version("1.2"));
    EXPECT_FALSE(update::parse_version("1.2.x"));
    EXPECT_FALSE(update::parse_version("1.2.3.4"));
    EXPECT_FALSE(update::parse_version("nightly"));
    EXPECT_FALSE(update::parse_version("UNKNOWN-VERSION"));
    EXPECT_FALSE(update::parse_version("1.2.3-"));
    EXPECT_FALSE(update::parse_version("1.2.3-rc..1"));
    EXPECT_FALSE(update::parse_version("1.2.3-rc_1"));
    EXPECT_FALSE(update::parse_version("1.2.3+"));
    EXPECT_FALSE(update::parse_version("1.2.3+build..5"));
}

TEST(Update, GitDescribeSuffixes) {
    // Git-describe distance does not make a build older than its tag.
    EXPECT_TRUE(parsed("v1.2.3-4").prerelease.empty());
    EXPECT_EQ(compare("v1.2.3-4", "v1.2.3"), 0);
    EXPECT_EQ(compare("v1.2.3-4-dirty", "v1.2.3"), 0);

    // Prerelease tags retain their prerelease identifiers.
    const Version afterPrerelease = parsed("v1.0.0-rc.1-4");
    EXPECT_EQ(afterPrerelease.prerelease.size(), 2);
    EXPECT_EQ(afterPrerelease.prerelease[1], "1");
    EXPECT_EQ(compare("v1.0.0-rc.1-4", "v1.0.0-rc.1"), 0);
    EXPECT_EQ(compare("v1.0.0-rc.1-4-dirty", "v1.0.0-rc.1"), 0);

    EXPECT_EQ(parsed("v1.0.0-beta").prerelease.size(), 1);
    EXPECT_EQ(parsed("v1.0.0-beta").prerelease[0], "beta");

    // Numeric-only prereleases are indistinguishable from git-describe distance.
    // Borealis tags do not use them.
    EXPECT_TRUE(parsed("1.0.0-1").prerelease.empty());
    EXPECT_EQ(compare("1.0.0-1", "1.0.0"), 0);
}

TEST(Update, CompareOrdering) {
    EXPECT_EQ(compare("1.2.3", "1.2.3"), 0);
    EXPECT_EQ(compare("2.0.0", "1.9.9"), 1);
    EXPECT_EQ(compare("1.10.0", "1.9.0"), 1);
    EXPECT_EQ(compare("1.2.4", "1.2.3"), 1);
    EXPECT_EQ(compare("1.2.3", "2.0.0"), -1);

    EXPECT_EQ(compare("1.0.0-rc.1", "1.0.0"), -1);
    EXPECT_EQ(compare("1.0.0", "1.0.0-rc.1"), 1);

    // Semver 11.4: alpha < alpha.1 < alpha.beta < beta < beta.2 < beta.11 < rc.1.
    EXPECT_EQ(compare("1.0.0-alpha", "1.0.0-alpha.1"), -1);
    EXPECT_EQ(compare("1.0.0-alpha.1", "1.0.0-alpha.beta"), -1);
    EXPECT_EQ(compare("1.0.0-alpha.beta", "1.0.0-beta"), -1);
    EXPECT_EQ(compare("1.0.0-beta", "1.0.0-beta.2"), -1);
    // Numeric identifiers compare numerically.
    EXPECT_EQ(compare("1.0.0-beta.2", "1.0.0-beta.11"), -1);
    EXPECT_EQ(compare("1.0.0-beta.11", "1.0.0-rc.1"), -1);
    // Numeric identifiers precede alphanumeric identifiers.
    EXPECT_EQ(compare("1.0.0-1.x", "1.0.0-alpha.x"), -1);
    // Leading zeroes do not affect numeric identifiers.
    EXPECT_EQ(compare("1.0.0-rc.007", "1.0.0-rc.7"), 0);

    // Build metadata is ignored.
    EXPECT_EQ(compare("1.2.3+build.1", "1.2.3+build.2"), 0);
}

TEST(Update, ParseGithubReleaseFixture) {
    // The full GitHub response fixture includes ignored fields.
    const Release release =
        update::parse_github_release(read_fixture("github_release_latest.json"));
    EXPECT_EQ(release.tagName, "v1.4.1");
    EXPECT_EQ(release.name, "v1.4.1");
    EXPECT_EQ(release.htmlUrl, "https://github.com/TwilitRealm/dusklight/releases/tag/v1.4.1");
    EXPECT_TRUE(release.body.starts_with("**What's new:**"));

    ASSERT_EQ(release.assets.size(), 8);
    EXPECT_EQ(release.assets[0].name, "Dusklight-v1.4.1-android-arm64.apk");
    EXPECT_TRUE(release.assets[0].browserDownloadUrl ==
                "https://github.com/TwilitRealm/dusklight/releases/download/v1.4.1/"
                "Dusklight-v1.4.1-android-arm64.apk");
    for (const update::Asset& asset : release.assets) {
        EXPECT_FALSE(asset.name.empty());
        EXPECT_TRUE(asset.browserDownloadUrl.starts_with("https://github.com/"));
        EXPECT_TRUE(asset.digest.starts_with("sha256:"));
    }
}

TEST(Update, ParseGithubReleaseToleratesMissingFields) {
    // Digests and asset lists are optional.
    const Release release = update::parse_github_release(
        R"({"tag_name": "v1.0.0", "assets": [{"name": "old.zip"}, "not-an-object"]})");
    EXPECT_EQ(release.tagName, "v1.0.0");
    EXPECT_TRUE(release.name.empty());
    EXPECT_TRUE(release.body.empty());
    ASSERT_EQ(release.assets.size(), 1);
    EXPECT_EQ(release.assets[0].name, "old.zip");
    EXPECT_TRUE(release.assets[0].digest.empty());
    EXPECT_TRUE(release.assets[0].browserDownloadUrl.empty());

    EXPECT_TRUE(update::parse_github_release(R"({"tag_name": "v1.0.0"})").assets.empty());
}

TEST(Update, CheckReportsUpdate) {
    const std::string body = read_fixture("github_release_latest.json");
    for (const std::string_view current : {"v1.4.0", "v1.4.1-rc.1", "v0.9.0-3-dirty"}) {
        const auto result = update::detail::result_from_response(ok_response(body), current, false);
        EXPECT_EQ(result.status, Status::UpdateAvailable);
        EXPECT_EQ(result.latest.tagName, "v1.4.1");
        EXPECT_EQ(result.latest.assets.size(), 8);
    }
}

TEST(Update, CheckReportsUpToDate) {
    const std::string body = read_fixture("github_release_latest.json");
    for (const std::string_view current : {"v1.4.1", "v1.4.1-6-dirty", "v1.5.0"}) {
        const auto result = update::detail::result_from_response(ok_response(body), current, false);
        EXPECT_EQ(result.status, Status::UpToDate);
        EXPECT_EQ(result.latest.tagName, "v1.4.1");
    }
}

TEST(Update, CheckBuildsExpectedRequest) {
    const http::Request request =
        update::detail::make_request(TestApp, {
                                                  .currentVersion = "v1.4.0",
                                                  .timeout = std::chrono::milliseconds{2500},
                                              });
    EXPECT_EQ(request.url, "https://api.github.com/repos/TwilitRealm/dusklight/releases/latest");
    EXPECT_EQ(request.connectTimeout, std::chrono::milliseconds{2500});
    EXPECT_EQ(request.idleTimeout, std::chrono::milliseconds{2500});
    EXPECT_EQ(request.totalTimeout, std::chrono::milliseconds{2500});

    bool sawUserAgent = false;
    bool sawAccept = false;
    for (const http::Header& header : request.headers) {
        if (header.name == "User-Agent") {
            sawUserAgent = true;
            EXPECT_EQ(header.value, std::string("Dusklight/") + BOREALIS_APP_DESCRIBE);
        } else if (header.name == "Accept") {
            sawAccept = true;
            EXPECT_EQ(header.value, "application/vnd.github+json");
        }
    }
    EXPECT_TRUE(sawUserAgent);
    EXPECT_TRUE(sawAccept);
}

TEST(Update, CheckCanIncludePrereleases) {
    const auto result = update::detail::result_from_response(
        ok_response(R"([{"tag_name":"v1.5.0-rc.1","prerelease":true,"draft":false}])"), "v1.4.0",
        true);
    EXPECT_EQ(result.status, Status::UpdateAvailable);
    EXPECT_EQ(result.latest.tagName, "v1.5.0-rc.1");

    const http::Request request =
        update::detail::make_request(TestApp, {.includePrereleases = true});
    EXPECT_EQ(
        request.url, "https://api.github.com/repos/TwilitRealm/dusklight/releases?per_page=10");
}

TEST(Update, CheckFailurePaths) {
    auto result = update::detail::result_from_response(
        {.error = http::Error::Timeout, .message = "Request timed out"}, "v1.0.0", false);
    EXPECT_EQ(result.status, Status::Failed);
    EXPECT_EQ(result.message, "Request timed out");

    result =
        update::detail::result_from_response({.response = {.statusCode = 403}}, "v1.0.0", false);
    EXPECT_EQ(result.status, Status::Failed);
    EXPECT_NE(result.message.find("403"), std::string::npos);

    result = update::detail::result_from_response(ok_response("{ not json"), "v1.0.0", false);
    EXPECT_TRUE(result.message.starts_with("Failed to parse GitHub release JSON"));

    result = update::detail::result_from_response(
        ok_response(R"({"tag_name": "nightly"})"), "v1.0.0", false);
    EXPECT_NE(result.message.find("nightly"), std::string::npos);
    EXPECT_EQ(result.latest.tagName, "nightly");

    result = update::detail::result_from_response(
        ok_response(read_fixture("github_release_latest.json")), "UNKNOWN-VERSION", false);
    EXPECT_NE(result.message.find("UNKNOWN-VERSION"), std::string::npos);
}

TEST(Update, CheckRequiresAppInfo) {
    constexpr AppInfo empty{};
    auto check = update::check_latest_github_release(empty);
    EXPECT_TRUE(check.ready());
    const auto result = check.try_take();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status, http::available() ? Status::Failed : Status::Disabled);
}

TEST(Update, CheckWithoutBackend) {
    if (http::available()) {
        return;
    }
    auto check = update::check_latest_github_release(TestApp);
    EXPECT_TRUE(check.ready());
    const auto result = check.try_take();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status, Status::Disabled);
    EXPECT_EQ(result->message, "No HTTP backend is available");
}

TEST(Update, CheckRequiresHttpInitialization) {
    if (!http::available()) {
        return;
    }
    http::shutdown();
    auto check = update::check_latest_github_release(TestApp);
    EXPECT_TRUE(check.ready());
    const auto result = check.try_take();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status, Status::Failed);
    EXPECT_EQ(result->message, "HTTP worker pool is not initialized");
    EXPECT_FALSE(check.try_take().has_value());
}

}  // namespace
