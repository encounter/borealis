#pragma once

#include "borealis/http.hpp"
#include "borealis/io.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace borealis::http::detail {

class Deadline {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    static constexpr auto PollInterval = std::chrono::milliseconds{50};

    class Tracker {
    public:
        Tracker() noexcept;

        void reset() noexcept;
        void touch() noexcept;
        void start_response() noexcept;

    private:
        friend class Deadline;

        TimePoint m_attemptStarted;
        std::atomic<Clock::duration::rep> m_lastActivity;
        std::atomic_bool m_responseStarted = false;
    };

    explicit Deadline(const Request& request);

    std::chrono::milliseconds connect_timeout() const noexcept;
    std::chrono::milliseconds idle_timeout() const noexcept;
    std::optional<std::chrono::milliseconds> remaining_total() const noexcept;
    std::chrono::milliseconds bounded_timeout(std::chrono::milliseconds timeout) const noexcept;
    bool total_expired() const noexcept;
    bool expired(const Tracker& tracker) const noexcept;

private:
    std::chrono::milliseconds m_connectTimeout;
    std::chrono::milliseconds m_idleTimeout;
    std::optional<TimePoint> m_totalDeadline;
};

struct DownloadPlan {
    std::filesystem::path destination;
    std::filesystem::path metadataPath;
    std::string requestUrl;
    std::string validatorName;
    std::string validatorValue;
    std::optional<std::uint64_t> expectedSize;
    std::uint64_t offset = 0;
    bool managed = false;
    bool resume = false;
    std::string error;
};

enum class DownloadAction {
    Memory,
    Replace,
    Append,
    Complete,
    Restart,
};

struct DownloadDecision {
    DownloadAction action = DownloadAction::Memory;
    std::optional<std::uint64_t> totalSize;
    std::string error;
};

/** Abort means the observer captured the outcome; transports may report their native error. */
class TransportObserver {
public:
    enum class Directive {
        Continue,
        Abort,
    };

    virtual ~TransportObserver() = default;
    virtual Directive on_response(int status, std::vector<Header> headers) = 0;
    virtual Directive on_data(std::span<const std::byte> chunk) = 0;
};

struct TransportRequest {
    Method method = Method::Get;
    std::string_view url;
    std::string_view body;
    std::vector<Header> headers;
    /** Enables native transfer decompression for in-memory responses. */
    bool allowCompression = false;
    Deadline* deadline = nullptr;
    TransportObserver* observer = nullptr;
    borealis::detail::TaskSignals* signals = nullptr;
};

struct TransportResult {
    Error error = Error::None;
    std::string message;
};

struct AttemptResult {
    Result result;
    bool restart = false;
};

class ResponseEngine final : public TransportObserver {
public:
    ResponseEngine(const Request& request, const DownloadPlan& downloadPlan, bool allowResume,
        borealis::detail::TaskSignals* signals);
    ResponseEngine(const ResponseEngine&) = delete;
    ResponseEngine& operator=(const ResponseEngine&) = delete;

    Directive on_response(int status, std::vector<Header> headers) override;
    Directive on_data(std::span<const std::byte> chunk) override;
    AttemptResult finish(TransportResult transportResult);

private:
    void fail(Error error, std::string message);
    bool prepare_sink();

    const DownloadPlan& m_downloadPlan;
    size_t m_maxBodyBytes;
    bool m_allowResume;
    borealis::detail::TaskSignals* m_signals;
    Response m_response;
    DownloadDecision m_downloadDecision;
    io::File m_output;
    Error m_error = Error::None;
    std::string m_message;
    bool m_responseReceived = false;
    bool m_restart = false;
    bool m_alreadyComplete = false;
};

using Transport = std::function<TransportResult(const TransportRequest&)>;

TransportResult send_request(const TransportRequest& request);
Result perform(
    const Request& request, borealis::detail::TaskSignals* signals, const Transport& transport);
Result perform(const Request& request, borealis::detail::TaskSignals* signals);

std::string trim_header_value(std::string_view value);

std::filesystem::path resume_metadata_path(const std::filesystem::path& destination);
DownloadPlan prepare_download(const Request& request);
std::vector<Header> request_headers(
    const Request& request, const DownloadPlan& plan, bool allowResume);
DownloadDecision evaluate_download_response(
    const DownloadPlan& plan, const Response& response, bool allowResume);
void set_download_progress(borealis::detail::TaskSignals* signals, const DownloadPlan& plan,
    const DownloadDecision& decision);
void finish_download(const DownloadPlan& plan) noexcept;

}  // namespace borealis::http::detail
