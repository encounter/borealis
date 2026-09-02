#include "borealis/http.hpp"

#include "http_internal.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string_view>
#include <thread>

namespace http = borealis::http;

namespace {

std::optional<http::Result> wait_for(borealis::Task<http::Result>& task) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    while (!task.ready() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return task.try_take();
}

bool ascii_iequals(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (size_t i = 0; i < lhs.size(); ++i) {
        const unsigned char lhsChar = static_cast<unsigned char>(lhs[i]);
        const unsigned char rhsChar = static_cast<unsigned char>(rhs[i]);
        if (std::tolower(lhsChar) != std::tolower(rhsChar)) {
            return false;
        }
    }
    return true;
}

// Invalid requests keep these tests offline for every backend.

class HttpTest : public testing::Test {};

TEST_F(HttpTest, BackendIdentity) {
    EXPECT_NE(http::backend_name(), nullptr);
    EXPECT_GT(std::strlen(http::backend_name()), 0);
    EXPECT_EQ(http::available(), (http::backend() != http::Backend::None));
}

TEST(HttpMethods, ForwardsMethodAndRequestBody) {
    struct MethodCase {
        http::Method method;
        std::string_view name;
        bool hasRequestBody;
    };
    constexpr std::array cases{
        MethodCase{http::Method::Get, "GET", false},
        MethodCase{http::Method::Post, "POST", true},
        MethodCase{http::Method::Head, "HEAD", false},
    };
    constexpr std::string_view RequestBody = "request body";
    constexpr std::string_view ResponseBody = "response body";

    for (const auto& test : cases) {
        SCOPED_TRACE(test.name);
        borealis::detail::TaskSignals signals;
        bool called = false;
        const http::Request request{
            .method = test.method,
            .url = "https://example.com/endpoint",
            .body = std::string{RequestBody},
        };
        const auto result = http::detail::perform(
            request, &signals, [&](const http::detail::TransportRequest& transport) {
                called = true;
                EXPECT_EQ(transport.method, test.method);
                EXPECT_EQ(http::detail::method_name(transport.method), test.name);
                EXPECT_EQ(transport.body, test.hasRequestBody ? RequestBody : std::string_view{});
                EXPECT_EQ(
                    transport.observer->on_response(200,
                        {{.name = "Content-Length", .value = std::to_string(ResponseBody.size())}}),
                    http::detail::TransportObserver::Directive::Continue);
                EXPECT_EQ(transport.observer->on_data(
                              std::as_bytes(std::span{ResponseBody.data(), ResponseBody.size()})),
                    http::detail::TransportObserver::Directive::Continue);
                return http::detail::TransportResult{};
            });
        ASSERT_TRUE(called);
        ASSERT_EQ(result.error, http::Error::None) << result.message;
        EXPECT_EQ(result.response.statusCode, 200);
        EXPECT_EQ(result.response.body,
            test.method == http::Method::Head ? std::string_view{} : ResponseBody);
        if (test.method == http::Method::Head) {
            EXPECT_FALSE(signals.totalKnown.load(std::memory_order_acquire));
        }
    }
}

TEST_F(HttpTest, EmptyUrlRejected) {
    auto task = http::start({});
    ASSERT_TRUE(task.ready());
    const auto result = task.try_take();
    ASSERT_TRUE(result.has_value());
    if (!http::available()) {
        EXPECT_EQ(result->error, http::Error::NoBackend);
        return;
    }
    EXPECT_EQ(result->error, http::Error::InvalidUrl);
    EXPECT_FALSE(result->message.empty());
    EXPECT_EQ(result->response.statusCode, 0);
}

TEST_F(HttpTest, PlaintextSchemeRejected) {
    // HTTPS-only behavior is shared by every backend.
    for (const char* url : {"http://example.com/", "ftp://example.com/", "example.com"}) {
        auto task = http::start({.url = url});
        ASSERT_TRUE(task.ready());
        const auto result = task.try_take();
        ASSERT_TRUE(result.has_value());
        if (!http::available()) {
            EXPECT_EQ(result->error, http::Error::NoBackend);
            continue;
        }
        EXPECT_TRUE(result->error == http::Error::UnsupportedScheme ||
                    result->error == http::Error::InvalidUrl);
        EXPECT_TRUE(result->response.body.empty());
    }
}

TEST_F(HttpTest, AsyncResultCanBeTakenOnce) {
    borealis::Task<http::Result> task = http::start({});
    ASSERT_TRUE(task);
    ASSERT_TRUE(task.ready());

    const auto result = task.try_take();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->error, http::available() ? http::Error::InvalidUrl : http::Error::NoBackend);
    EXPECT_FALSE(task.try_take().has_value());
}

TEST_F(HttpTest, TaskCanMapResult) {
    auto task = http::start({}).map([](http::Result result) { return result.error; });
    ASSERT_TRUE(task.ready());
    const auto error = task.try_take();
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(*error, http::available() ? http::Error::InvalidUrl : http::Error::NoBackend);
}

TEST_F(HttpTest, LiveRangeResume) {
    const char* url = std::getenv("BOREALIS_HTTP_RESUME_TEST_URL");
    if (url == nullptr || *url == '\0') {
        GTEST_SKIP() << "BOREALIS_HTTP_RESUME_TEST_URL is not set";
    }

    auto sourceTask = http::start({
        .url = url,
        .headers = {{.name = "Accept-Encoding", .value = "identity"}},
        .totalTimeout = std::chrono::seconds{30},
        .maxBodyBytes = 16 * 1024 * 1024,
    });
    const auto source = wait_for(sourceTask);
    ASSERT_TRUE(source.has_value());
    ASSERT_EQ(source->error, http::Error::None) << source->message;
    ASSERT_EQ(source->response.statusCode, 200);
    ASSERT_GT(source->response.body.size(), 1);

    std::string etag;
    for (const http::Header& header : source->response.headers) {
        if (ascii_iequals(header.name, "ETag")) {
            etag = header.value;
            break;
        }
    }
    ASSERT_GE(etag.size(), 2);
    ASSERT_EQ(etag.front(), '"');
    ASSERT_EQ(etag.back(), '"');

    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("borealis-http-live-test-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(directory));
    const std::filesystem::path destination = directory / "download.tmp";
    const size_t partialSize = source->response.body.size() / 2;
    {
        std::ofstream output{destination, std::ios::binary};
        ASSERT_TRUE(output);
        output.write(source->response.body.data(), static_cast<std::streamsize>(partialSize));
        ASSERT_TRUE(output.good());
    }
    {
        const nlohmann::json metadata{
            {"version", 1},
            {"url", url},
            {"encoding", "identity"},
            {"validator", {{"type", "etag"}, {"value", etag}}},
            {"expectedSize", source->response.body.size()},
        };
        std::ofstream output{http::detail::resume_metadata_path(destination)};
        ASSERT_TRUE(output);
        output << metadata;
        ASSERT_TRUE(output.good());
    }

    auto resumeTask = http::start({
        .url = url,
        .downloadTo = destination,
        .totalTimeout = std::chrono::seconds{30},
    });
    const auto resumed = wait_for(resumeTask);
    ASSERT_TRUE(resumed.has_value());
    ASSERT_EQ(resumed->error, http::Error::None) << resumed->message;
    EXPECT_EQ(resumed->response.statusCode, 206);
    EXPECT_EQ(resumeTask.progress().completed, source->response.body.size());
    EXPECT_EQ(resumeTask.progress().total, source->response.body.size());

    std::ifstream input{destination, std::ios::binary};
    ASSERT_TRUE(input);
    const std::string downloaded{std::istreambuf_iterator<char>{input}, {}};
    EXPECT_EQ(downloaded, source->response.body);
    EXPECT_FALSE(std::filesystem::exists(http::detail::resume_metadata_path(destination)));

    {
        std::ofstream output{destination, std::ios::binary | std::ios::trunc};
        ASSERT_TRUE(output);
        output.write(source->response.body.data(), static_cast<std::streamsize>(partialSize));
        ASSERT_TRUE(output.good());
    }
    {
        const nlohmann::json metadata{
            {"version", 1},
            {"url", url},
            {"encoding", "identity"},
            {"validator", {{"type", "etag"}, {"value", "\"stale-validator\""}}},
            {"expectedSize", source->response.body.size()},
        };
        std::ofstream output{http::detail::resume_metadata_path(destination)};
        ASSERT_TRUE(output);
        output << metadata;
        ASSERT_TRUE(output.good());
    }
    auto replaceTask = http::start({
        .url = url,
        .downloadTo = destination,
        .totalTimeout = std::chrono::seconds{30},
    });
    const auto replaced = wait_for(replaceTask);
    ASSERT_TRUE(replaced.has_value());
    ASSERT_EQ(replaced->error, http::Error::None) << replaced->message;
    EXPECT_EQ(replaced->response.statusCode, 200);
    {
        std::ifstream replacement{destination, std::ios::binary};
        ASSERT_TRUE(replacement);
        const std::string replacementBody{std::istreambuf_iterator<char>{replacement}, {}};
        EXPECT_EQ(replacementBody, source->response.body);
    }

    {
        const nlohmann::json metadata{
            {"version", 1},
            {"url", url},
            {"encoding", "identity"},
            {"validator", {{"type", "etag"}, {"value", etag}}},
            {"expectedSize", source->response.body.size()},
        };
        std::ofstream output{http::detail::resume_metadata_path(destination)};
        ASSERT_TRUE(output);
        output << metadata;
        ASSERT_TRUE(output.good());
    }
    auto completeTask = http::start({
        .url = url,
        .downloadTo = destination,
        .totalTimeout = std::chrono::seconds{30},
    });
    const auto complete = wait_for(completeTask);
    ASSERT_TRUE(complete.has_value());
    ASSERT_EQ(complete->error, http::Error::None) << complete->message;
    EXPECT_EQ(complete->response.statusCode, 200);
    EXPECT_EQ(std::filesystem::file_size(destination), source->response.body.size());
    EXPECT_FALSE(std::filesystem::exists(http::detail::resume_metadata_path(destination)));

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

TEST_F(HttpTest, LiveHeadMethod) {
    const char* url = std::getenv("BOREALIS_HTTP_METHOD_TEST_URL");
    if (url == nullptr || *url == '\0') {
        GTEST_SKIP() << "BOREALIS_HTTP_METHOD_TEST_URL is not set";
    }

    auto task = http::start({
        .method = http::Method::Head,
        .url = url,
        .headers = {{.name = "User-Agent", .value = "borealis-http-test"}},
        .totalTimeout = std::chrono::seconds{10},
    });
    const auto result = wait_for(task);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->error, http::Error::None) << result->message;
    EXPECT_GE(result->response.statusCode, 200);
    EXPECT_LT(result->response.statusCode, 300);
    EXPECT_TRUE(result->response.body.empty());
}

class HttpDownload : public testing::Test {
protected:
    void SetUp() override {
        static std::atomic_uint64_t nextId = 0;
        const auto* testInfo = testing::UnitTest::GetInstance()->current_test_info();
        directory = std::filesystem::temp_directory_path() /
                    ("borealis-http-test-" + std::string{testInfo->name()} + "-" +
                        std::to_string(nextId.fetch_add(1)));
        ASSERT_TRUE(std::filesystem::create_directory(directory));
        destination = directory / "download.tmp";
    }

    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }

    void write_partial(std::string_view contents = "partial") {
        std::ofstream output{destination, std::ios::binary};
        ASSERT_TRUE(output);
        output << contents;
        ASSERT_TRUE(output.good());
    }

    void write_metadata(std::uint64_t expectedSize = 20) {
        const nlohmann::json metadata{
            {"version", 1},
            {"url", "https://example.com/file"},
            {"encoding", "identity"},
            {"validator", {{"type", "etag"}, {"value", "\"revision-1\""}}},
            {"expectedSize", expectedSize},
        };
        std::ofstream output{http::detail::resume_metadata_path(destination)};
        ASSERT_TRUE(output);
        output << metadata;
        ASSERT_TRUE(output.good());
    }

    http::Request request() const {
        return {
            .url = "https://example.com/file",
            .downloadTo = destination,
        };
    }

    std::filesystem::path directory;
    std::filesystem::path destination;
};

TEST_F(HttpDownload, NonEmptySinkWithMetadataRequestsResume) {
    write_partial();
    write_metadata();

    const http::detail::DownloadPlan plan = http::detail::prepare_download(request());
    ASSERT_TRUE(plan.managed);
    ASSERT_TRUE(plan.resume);
    EXPECT_EQ(plan.offset, 7);
    EXPECT_EQ(plan.validatorName, "ETag");
    EXPECT_EQ(plan.validatorValue, "\"revision-1\"");

    const auto headers = http::detail::request_headers(request(), plan, true);
    ASSERT_EQ(headers.size(), 3);
    EXPECT_EQ(headers[0].name, "Accept-Encoding");
    EXPECT_EQ(headers[0].value, "identity");
    EXPECT_EQ(headers[1].name, "Range");
    EXPECT_EQ(headers[1].value, "bytes=7-");
    EXPECT_EQ(headers[2].name, "If-Range");
    EXPECT_EQ(headers[2].value, "\"revision-1\"");
}

TEST_F(HttpDownload, MissingOrMismatchedMetadataRestartsFromZero) {
    const http::detail::DownloadPlan missing = http::detail::prepare_download(request());
    EXPECT_TRUE(missing.managed);
    EXPECT_TRUE(missing.error.empty());
    EXPECT_EQ(missing.offset, 0);
    EXPECT_FALSE(missing.resume);

    write_partial();
    EXPECT_FALSE(http::detail::prepare_download(request()).resume);

    write_metadata();
    http::Request otherRequest = request();
    otherRequest.url = "https://example.com/other";
    EXPECT_FALSE(http::detail::prepare_download(otherRequest).resume);
}

TEST_F(HttpDownload, LastModifiedCanValidateResume) {
    write_partial();
    const nlohmann::json metadata{
        {"version", 1},
        {"url", request().url},
        {"encoding", "identity"},
        {"validator", {{"type", "last-modified"}, {"value", "Wed, 21 Oct 2015 07:28:00 GMT"}}},
        {"expectedSize", 20},
    };
    std::ofstream output{http::detail::resume_metadata_path(destination)};
    ASSERT_TRUE(output);
    output << metadata;
    output.close();

    const http::detail::DownloadPlan plan = http::detail::prepare_download(request());
    ASSERT_TRUE(plan.resume);
    EXPECT_EQ(plan.validatorName, "Last-Modified");
    EXPECT_EQ(plan.validatorValue, "Wed, 21 Oct 2015 07:28:00 GMT");
}

TEST_F(HttpDownload, UnmanagedDownloadClearsStaleSidecar) {
    write_partial();
    write_metadata();
    http::Request manual = request();
    manual.headers.push_back({.name = "Range", .value = "bytes=10-"});
    const http::detail::DownloadPlan plan = http::detail::prepare_download(manual);
    ASSERT_FALSE(plan.managed);

    borealis::detail::TaskSignals signals;
    http::detail::ResponseEngine engine{manual, plan, false, &signals};
    EXPECT_EQ(engine.on_response(206, {{.name = "Content-Length", .value = "10"}}),
        http::detail::TransportObserver::Directive::Continue);
    EXPECT_FALSE(std::filesystem::exists(http::detail::resume_metadata_path(destination)));
    EXPECT_EQ(engine.finish({}).result.error, http::Error::None);
}

TEST_F(HttpDownload, ValidatesPartialResponseBeforeAppending) {
    write_partial();
    write_metadata();
    const http::detail::DownloadPlan plan = http::detail::prepare_download(request());

    const http::Response valid{
        .statusCode = 206,
        .headers =
            {
                {.name = "Content-Range", .value = "bytes 7-19/20"},
                {.name = "ETag", .value = "\"revision-1\""},
            },
    };
    const auto append = http::detail::evaluate_download_response(plan, valid, true);
    EXPECT_EQ(append.action, http::detail::DownloadAction::Append);
    EXPECT_EQ(append.totalSize, 20);

    http::Response invalid = valid;
    invalid.headers[0].value = "bytes 6-19/20";
    EXPECT_EQ(http::detail::evaluate_download_response(plan, invalid, true).action,
        http::detail::DownloadAction::Restart);

    http::Response changed = valid;
    changed.headers[1].value = "\"revision-2\"";
    EXPECT_EQ(http::detail::evaluate_download_response(plan, changed, true).action,
        http::detail::DownloadAction::Restart);

    http::Response wrongLength = valid;
    wrongLength.headers.push_back({.name = "Content-Length", .value = "12"});
    EXPECT_EQ(http::detail::evaluate_download_response(plan, wrongLength, true).action,
        http::detail::DownloadAction::Restart);
}

TEST_F(HttpDownload, ServerCanReplaceOrConfirmCompleteSink) {
    write_partial("complete");
    write_metadata(8);
    const http::detail::DownloadPlan plan = http::detail::prepare_download(request());

    const auto replace = http::detail::evaluate_download_response(
        plan, {.statusCode = 200, .headers = {{.name = "Content-Length", .value = "12"}}}, true);
    EXPECT_EQ(replace.action, http::detail::DownloadAction::Replace);
    EXPECT_EQ(replace.totalSize, 12);

    const auto complete = http::detail::evaluate_download_response(plan,
        {.statusCode = 416, .headers = {{.name = "Content-Range", .value = "bytes */8"}}}, true);
    EXPECT_EQ(complete.action, http::detail::DownloadAction::Complete);
    EXPECT_EQ(complete.totalSize, 8);
}

TEST_F(HttpDownload, SidecarIsJsonAndRemovedAfterCompletion) {
    std::ofstream{destination}.close();
    const http::detail::DownloadPlan plan = http::detail::prepare_download(request());
    borealis::detail::TaskSignals signals;
    http::detail::ResponseEngine engine{request(), plan, false, &signals};
    ASSERT_EQ(engine.on_response(200,
                  {
                      {.name = "ETag", .value = "\"revision-1\""},
                      {.name = "Content-Length", .value = "20"},
                  }),
        http::detail::TransportObserver::Directive::Continue);
    constexpr std::string_view Partial = "partial";
    ASSERT_EQ(engine.on_data(std::as_bytes(std::span{Partial.data(), Partial.size()})),
        http::detail::TransportObserver::Directive::Continue);

    const auto metadataPath = http::detail::resume_metadata_path(destination);
    std::ifstream input{metadataPath};
    ASSERT_TRUE(input);
    const nlohmann::json metadata = nlohmann::json::parse(input);
    EXPECT_EQ(metadata["version"], 1);
    EXPECT_EQ(metadata["url"], request().url);
    EXPECT_EQ(metadata["validator"]["type"], "etag");
    EXPECT_EQ(metadata["expectedSize"], 20);
    input.close();

    const auto result = engine.finish({});
    EXPECT_EQ(result.result.error, http::Error::None);
    EXPECT_FALSE(std::filesystem::exists(metadataPath));
    EXPECT_EQ(std::filesystem::file_size(destination), 7);
}

TEST(HttpResponseEngine, BoundsDecodedMemoryBody) {
    http::Request request{
        .url = "https://example.com/data",
        .maxBodyBytes = 4,
    };
    const http::detail::DownloadPlan plan = http::detail::prepare_download(request);
    borealis::detail::TaskSignals signals;
    http::detail::ResponseEngine engine{request, plan, false, &signals};

    ASSERT_EQ(engine.on_response(200, {{.name = "Content-Length", .value = "8"}}),
        http::detail::TransportObserver::Directive::Continue);
    constexpr std::string_view First = "data";
    constexpr std::string_view Extra = "!";
    EXPECT_EQ(engine.on_data(std::as_bytes(std::span{First.data(), First.size()})),
        http::detail::TransportObserver::Directive::Continue);
    EXPECT_EQ(engine.on_data(std::as_bytes(std::span{Extra.data(), Extra.size()})),
        http::detail::TransportObserver::Directive::Abort);

    const auto result = engine.finish({});
    EXPECT_EQ(result.result.error, http::Error::TooLarge);
    EXPECT_EQ(result.result.response.body, "data");
    EXPECT_EQ(signals.completed.load(), 4);
    EXPECT_TRUE(signals.totalKnown.load());
    EXPECT_EQ(signals.total.load(), 8);
}

TEST(HttpResponseEngine, EncodedLengthIsNotAProgressTotal) {
    http::Request request{
        .url = "https://example.com/data",
        .maxBodyBytes = 16,
    };
    const http::detail::DownloadPlan plan = http::detail::prepare_download(request);
    borealis::detail::TaskSignals signals;
    http::detail::ResponseEngine engine{request, plan, false, &signals};

    ASSERT_EQ(engine.on_response(200,
                  {
                      {.name = "Content-Length", .value = "4"},
                      {.name = "Content-Encoding", .value = "gzip"},
                  }),
        http::detail::TransportObserver::Directive::Continue);
    constexpr std::string_view Decoded = "decoded";
    EXPECT_EQ(engine.on_data(std::as_bytes(std::span{Decoded.data(), Decoded.size()})),
        http::detail::TransportObserver::Directive::Continue);

    const auto result = engine.finish({});
    EXPECT_EQ(result.result.error, http::Error::None);
    EXPECT_EQ(result.result.response.body, Decoded);
    EXPECT_EQ(signals.completed.load(), Decoded.size());
    EXPECT_FALSE(signals.totalKnown.load());
}

TEST_F(HttpDownload, ReportsFileOpenReason) {
    http::Request missingParent = request();
    missingParent.downloadTo = directory / "missing" / "download.tmp";
    const http::detail::DownloadPlan plan = http::detail::prepare_download(missingParent);
    ASSERT_TRUE(plan.error.empty());
    borealis::detail::TaskSignals signals;
    http::detail::ResponseEngine engine{missingParent, plan, false, &signals};

    EXPECT_EQ(engine.on_response(200, {}), http::detail::TransportObserver::Directive::Abort);
    const auto result = engine.finish({});
    EXPECT_EQ(result.result.error, http::Error::Io);
    EXPECT_NE(result.result.message.find("Failed to open download destination"), std::string::npos);
    EXPECT_NE(result.result.message.find(":"), std::string::npos);
}

TEST_F(HttpDownload, SharedDriverRetriesInvalidResumeOnce) {
    write_partial();
    write_metadata();
    borealis::detail::TaskSignals signals;
    int attempts = 0;

    const http::Result result = http::detail::perform(
        request(), &signals, [&](const http::detail::TransportRequest& transportRequest) {
            ++attempts;
            if (attempts == 1) {
                EXPECT_EQ(transportRequest.headers.size(), 3);
                EXPECT_FALSE(transportRequest.allowCompression);
                EXPECT_EQ(transportRequest.observer->on_response(206,
                              {
                                  {.name = "Content-Range", .value = "bytes 6-19/20"},
                                  {.name = "ETag", .value = "\"revision-1\""},
                              }),
                    http::detail::TransportObserver::Directive::Abort);
                return http::detail::TransportResult{};
            }

            EXPECT_EQ(transportRequest.headers.size(), 1);
            EXPECT_EQ(transportRequest.observer->on_response(200,
                          {
                              {.name = "Content-Length", .value = "5"},
                              {.name = "ETag", .value = "\"revision-2\""},
                          }),
                http::detail::TransportObserver::Directive::Continue);
            constexpr std::string_view Replacement = "fresh";
            EXPECT_EQ(transportRequest.observer->on_data(
                          std::as_bytes(std::span{Replacement.data(), Replacement.size()})),
                http::detail::TransportObserver::Directive::Continue);
            return http::detail::TransportResult{};
        });

    EXPECT_EQ(result.error, http::Error::None) << result.message;
    EXPECT_EQ(attempts, 2);
    std::ifstream input{destination, std::ios::binary};
    ASSERT_TRUE(input);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>{input}, {}), "fresh");
    EXPECT_FALSE(std::filesystem::exists(http::detail::resume_metadata_path(destination)));
}

}  // namespace
