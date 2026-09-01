#pragma once

#include "borealis/task.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace borealis::http {

/** Configured HTTP backend. */
enum class Backend {
    None,
    WinHttp,
    UrlSession,
    LibCurl,
    Android,
};

enum class Error {
    None,
    NoBackend,
    NotInitialized,
    InvalidUrl,
    UnsupportedScheme,
    Timeout,
    TooLarge,
    Canceled,
    Io,
    Network,
};

enum class Method {
    Get,
    Post,
    Head,
};

struct Header {
    std::string name;
    std::string value;
};

struct Request {
    Method method = Method::Get;
    std::string url;
    std::vector<Header> headers;
    std::string body;
    /**
     * File managed by the caller. Preserved on failure or cancellation. A non-empty file
     * used by a GET request attempts to resume if existing metadata is available.
     */
    std::filesystem::path downloadTo;
    std::chrono::milliseconds connectTimeout{10000};
    /** Maximum time without network progress. */
    std::chrono::milliseconds idleTimeout{10000};
    /** Maximum total time in the request. */
    std::optional<std::chrono::milliseconds> totalTimeout;
    /** Maximum decoded response bytes. Ignored when downloadTo is set. */
    size_t maxBodyBytes = 1024 * 1024;
};

struct Response {
    int statusCode = 0;
    std::vector<Header> headers;
    std::string body;
};

struct Result {
    Error error = Error::None;
    std::string message;
    Response response;
};

/** Returns false when no HTTP backend is available. */
bool available() noexcept;
Backend backend() noexcept;
const char* backend_name() noexcept;

bool initialize() noexcept;
void shutdown() noexcept;

/** Starts an asynchronous HTTPS request. */
Task<Result> start(Request request);

}  // namespace borealis::http
