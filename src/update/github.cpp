#include "../update_internal.hpp"

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <exception>
#include <stdexcept>
#include <utility>

namespace borealis::update {
namespace {

using json = nlohmann::json;

constexpr std::string_view GitHubApiVersion = "2026-03-10";

std::string json_string(const json& value, const char* key) {
    const auto iter = value.find(key);
    if (iter == value.end() || !iter->is_string()) {
        return {};
    }
    return iter->get<std::string>();
}

Release release_from_json(const json& value) {
    Release release{
        .tagName = json_string(value, "tag_name"),
        .name = json_string(value, "name"),
        .htmlUrl = json_string(value, "html_url"),
        .body = json_string(value, "body"),
    };

    const auto assets = value.find("assets");
    if (assets != value.end() && assets->is_array()) {
        for (const auto& asset : *assets) {
            if (!asset.is_object()) {
                continue;
            }
            release.assets.push_back({
                .name = json_string(asset, "name"),
                .browserDownloadUrl = json_string(asset, "browser_download_url"),
                .digest = json_string(asset, "digest"),
            });
        }
    }

    return release;
}

std::string release_url(std::string_view owner, std::string_view repo, bool includePrereleases) {
    if (includePrereleases) {
        return fmt::format("https://api.github.com/repos/{}/{}/releases?per_page=10", owner, repo);
    }
    return fmt::format("https://api.github.com/repos/{}/{}/releases/latest", owner, repo);
}

Release parse_release_response(std::string_view value, bool includePrereleases) {
    const json parsed = json::parse(value);
    if (!includePrereleases) {
        return release_from_json(parsed);
    }
    if (!parsed.is_array()) {
        throw std::runtime_error("expected a GitHub releases array");
    }
    for (const auto& release : parsed) {
        if (!release.is_object() || release.value("draft", false)) {
            continue;
        }
        return release_from_json(release);
    }
    throw std::runtime_error("GitHub returned no published releases");
}

Result failed_exception(const std::exception& exception) {
    return {
        .status = Status::Failed,
        .message = fmt::format("Update check failed with exception: {}", exception.what()),
    };
}

Result failed_exception() {
    return {
        .status = Status::Failed,
        .message = "Update check failed with an unknown exception",
    };
}

}  // namespace

namespace detail {

http::Request make_request(const AppInfo& info, const Options& options) {
    return {
        .url = release_url(info.githubOwner, info.githubRepo, options.includePrereleases),
        .headers =
            {
                {.name = "User-Agent", .value = user_agent(info)},
                {.name = "Accept", .value = "application/vnd.github+json"},
                {.name = "X-GitHub-Api-Version", .value = std::string(GitHubApiVersion)},
            },
        .connectTimeout = options.timeout,
        .idleTimeout = options.timeout,
        .totalTimeout = options.timeout,
    };
}

Result result_from_response(
    http::Result response, std::string_view currentVersion, bool includePrereleases) {
    if (response.error != http::Error::None) {
        return {
            .status = Status::Failed,
            .message = std::move(response.message),
        };
    }
    if (response.response.statusCode != 200) {
        return {
            .status = Status::Failed,
            .message = fmt::format("GitHub returned HTTP {}", response.response.statusCode),
        };
    }

    Release latest;
    try {
        latest = parse_release_response(response.response.body, includePrereleases);
    } catch (const std::exception& e) {
        return {
            .status = Status::Failed,
            .message = fmt::format("Failed to parse GitHub release JSON: {}", e.what()),
        };
    }

    const std::optional<Version> latestVersion = parse_version(latest.tagName);
    if (!latestVersion) {
        return {
            .status = Status::Failed,
            .message = fmt::format("Failed to parse release tag '{}'", latest.tagName),
            .latest = std::move(latest),
        };
    }
    const std::optional<Version> parsedCurrentVersion = parse_version(currentVersion);
    if (!parsedCurrentVersion) {
        return {
            .status = Status::Failed,
            .message = fmt::format("Failed to parse current version '{}'", currentVersion),
            .latest = std::move(latest),
        };
    }

    const bool updateAvailable = compare_version(*latestVersion, *parsedCurrentVersion) > 0;
    return {
        .status = updateAvailable ? Status::UpdateAvailable : Status::UpToDate,
        .message = updateAvailable ? "Update available" : "Up to date",
        .latest = std::move(latest),
    };
}

}  // namespace detail

Release parse_github_release(std::string_view value) {
    return release_from_json(json::parse(value));
}

Task<Result> check_latest_github_release(const AppInfo& info, const Options& options) {
    if (!http::available()) {
        return borealis::detail::make_ready_task(Result{
            .status = Status::Disabled,
            .message = "No HTTP backend is available",
        });
    }
    if (info.appName.empty() || info.githubOwner.empty() || info.githubRepo.empty()) {
        return borealis::detail::make_ready_task(Result{
            .status = Status::Failed,
            .message = "AppInfo is missing appName, githubOwner, or githubRepo",
        });
    }

    std::string currentVersion{options.currentVersion};
    const bool includePrereleases = options.includePrereleases;
    return http::start(detail::make_request(info, options))
        .map([currentVersion = std::move(currentVersion), includePrereleases](
                 http::Result response) {
            try {
                return detail::result_from_response(
                    std::move(response), currentVersion, includePrereleases);
            } catch (const std::exception& exception) {
                return failed_exception(exception);
            } catch (...) {
                return failed_exception();
            }
        });
}

}  // namespace borealis::update
