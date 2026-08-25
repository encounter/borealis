#include "http_internal.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <string_view>
#include <system_error>

namespace borealis::http::detail {
namespace {

constexpr int MetadataVersion = 1;

bool ascii_iequals(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (size_t i = 0; i < lhs.size(); ++i) {
        char lhsChar = lhs[i];
        char rhsChar = rhs[i];
        if (lhsChar >= 'A' && lhsChar <= 'Z') {
            lhsChar = static_cast<char>(lhsChar - 'A' + 'a');
        }
        if (rhsChar >= 'A' && rhsChar <= 'Z') {
            rhsChar = static_cast<char>(rhsChar - 'A' + 'a');
        }
        if (lhsChar != rhsChar) {
            return false;
        }
    }
    return true;
}

std::string_view trim(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' ||
                                 value.back() == '\n'))
    {
        value.remove_suffix(1);
    }
    return value;
}

const std::string* header_value(const std::vector<Header>& headers, std::string_view name) {
    for (const Header& header : headers) {
        if (ascii_iequals(header.name, name)) {
            return &header.value;
        }
    }
    return nullptr;
}

bool parse_unsigned(std::string_view value, std::uint64_t& result) {
    value = trim(value);
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    return error == std::errc{} && end == value.data() + value.size();
}

std::optional<std::uint64_t> content_length(const Response& response) {
    const std::string* value = header_value(response.headers, "Content-Length");
    std::uint64_t length = 0;
    if (value != nullptr && parse_unsigned(*value, length)) {
        return length;
    }
    return std::nullopt;
}

struct ContentRange {
    std::optional<std::uint64_t> start;
    std::optional<std::uint64_t> end;
    std::optional<std::uint64_t> total;
};

std::optional<ContentRange> parse_content_range(std::string_view value) {
    value = trim(value);
    constexpr std::string_view Prefix = "bytes ";
    if (value.size() < Prefix.size() || !ascii_iequals(value.substr(0, Prefix.size()), Prefix)) {
        return std::nullopt;
    }
    value.remove_prefix(Prefix.size());

    const size_t slash = value.find('/');
    if (slash == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view range = value.substr(0, slash);
    const std::string_view totalValue = value.substr(slash + 1);

    ContentRange result;
    if (totalValue != "*") {
        std::uint64_t total = 0;
        if (!parse_unsigned(totalValue, total)) {
            return std::nullopt;
        }
        result.total = total;
    }

    if (range == "*") {
        return result;
    }
    const size_t dash = range.find('-');
    if (dash == std::string_view::npos) {
        return std::nullopt;
    }
    std::uint64_t start = 0;
    std::uint64_t end = 0;
    if (!parse_unsigned(range.substr(0, dash), start) ||
        !parse_unsigned(range.substr(dash + 1), end) || end < start)
    {
        return std::nullopt;
    }
    result.start = start;
    result.end = end;
    return result;
}

bool identity_encoded(const Response& response) {
    const std::string* value = header_value(response.headers, "Content-Encoding");
    return value == nullptr || ascii_iequals(trim(*value), "identity");
}

bool strong_etag(std::string_view value) {
    value = trim(value);
    return value.size() >= 2 && value.front() == '"' && value.back() == '"';
}

std::pair<std::string, std::string> response_validator(const Response& response) {
    if (const std::string* etag = header_value(response.headers, "ETag");
        etag != nullptr && strong_etag(*etag))
    {
        return {"ETag", std::string{trim(*etag)}};
    }
    if (const std::string* modified = header_value(response.headers, "Last-Modified");
        modified != nullptr && !trim(*modified).empty())
    {
        return {"Last-Modified", std::string{trim(*modified)}};
    }
    return {};
}

bool header_is(const Request& request, std::string_view name, std::string_view value = {}) {
    for (const Header& header : request.headers) {
        if (!ascii_iequals(header.name, name)) {
            continue;
        }
        return value.empty() || ascii_iequals(trim(header.value), value);
    }
    return false;
}

bool read_metadata(DownloadPlan& plan) {
    try {
        std::ifstream input{plan.metadataPath};
        if (!input) {
            return false;
        }
        const nlohmann::json metadata = nlohmann::json::parse(input);
        if (!metadata.is_object() || metadata.value("version", 0) != MetadataVersion ||
            metadata.value("url", "") != plan.requestUrl ||
            metadata.value("encoding", "") != "identity")
        {
            return false;
        }

        const auto validator = metadata.find("validator");
        if (validator == metadata.end() || !validator->is_object()) {
            return false;
        }
        const std::string type = validator->value("type", "");
        const std::string value = validator->value("value", "");
        if ((type != "etag" && type != "last-modified") || value.empty() ||
            (type == "etag" && !strong_etag(value)))
        {
            return false;
        }

        plan.validatorName = type == "etag" ? "ETag" : "Last-Modified";
        plan.validatorValue = value;
        if (const auto size = metadata.find("expectedSize");
            size != metadata.end() && size->is_number_unsigned())
        {
            plan.expectedSize = size->get<std::uint64_t>();
            if (*plan.expectedSize < plan.offset) {
                return false;
            }
        }
        return true;
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}

bool remove_metadata(const DownloadPlan& plan, std::string& error) {
    std::error_code removeError;
    std::filesystem::remove(plan.metadataPath, removeError);
    if (removeError) {
        error = "Failed to remove download resume metadata: " + removeError.message();
        return false;
    }
    return true;
}

std::span<const std::byte> as_bytes(std::string_view value) {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

bool write_metadata(const DownloadPlan& plan, const std::string& validatorName,
    const std::string& validatorValue, std::optional<std::uint64_t> expectedSize,
    std::string& error) {
    nlohmann::json metadata{
        {"version", MetadataVersion},
        {"url", plan.requestUrl},
        {"encoding", "identity"},
        {"validator", {{"type", validatorName == "ETag" ? "etag" : "last-modified"},
                          {"value", validatorValue}}},
    };
    if (expectedSize) {
        metadata["expectedSize"] = *expectedSize;
    }

    std::filesystem::path temporary = plan.metadataPath;
    temporary += ".tmp";
    io::File output;
    if (!output.open(temporary, io::File::Mode::Truncate, error)) {
        error = "Failed to create download resume metadata: " + error;
        return false;
    }
    const std::string encoded = metadata.dump(2) + '\n';
    if (!output.write(as_bytes(encoded), error) || !output.close(error)) {
        error = "Failed to write download resume metadata: " + error;
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }
    if (!io::atomic_replace(temporary, plan.metadataPath, error)) {
        error = "Failed to install download resume metadata: " + error;
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }
    return true;
}

std::chrono::milliseconds positive_timeout(std::chrono::milliseconds timeout) {
    return std::max(timeout, std::chrono::milliseconds{1});
}

}  // namespace

std::string trim_header_value(std::string_view value) {
    return std::string{trim(value)};
}

std::filesystem::path resume_metadata_path(const std::filesystem::path& destination) {
    std::filesystem::path result = destination;
    result += ".borealis-resume.json";
    return result;
}

DownloadPlan prepare_download(const Request& request) {
    DownloadPlan plan{
        .destination = request.downloadTo,
        .metadataPath = resume_metadata_path(request.downloadTo),
        .requestUrl = request.url,
    };
    if (request.downloadTo.empty() || request.method != Method::Get ||
        header_is(request, "Range") || header_is(request, "If-Range"))
    {
        return plan;
    }
    if (header_is(request, "Accept-Encoding") && !header_is(request, "Accept-Encoding", "identity"))
    {
        return plan;
    }
    plan.managed = true;

    std::error_code sizeError;
    plan.offset = std::filesystem::file_size(plan.destination, sizeError);
    if (sizeError == std::errc::no_such_file_or_directory) {
        plan.offset = 0;
        return plan;
    }
    if (sizeError) {
        plan.error = "Failed to inspect download destination: " + sizeError.message();
        return plan;
    }
    if (plan.offset > 0) {
        plan.resume = read_metadata(plan);
    }
    return plan;
}

std::vector<Header> request_headers(
    const Request& request, const DownloadPlan& plan, bool allowResume) {
    std::vector<Header> headers = request.headers;
    if (!plan.managed) {
        return headers;
    }
    if (!header_is(request, "Accept-Encoding")) {
        headers.push_back({.name = "Accept-Encoding", .value = "identity"});
    }
    if (allowResume && plan.resume) {
        headers.push_back({.name = "Range", .value = "bytes=" + std::to_string(plan.offset) + "-"});
        headers.push_back({.name = "If-Range", .value = plan.validatorValue});
    }
    return headers;
}

DownloadDecision evaluate_download_response(
    const DownloadPlan& plan, const Response& response, bool allowResume) {
    if (plan.destination.empty()) {
        return {};
    }
    if (!plan.managed) {
        return {.action = DownloadAction::Replace, .totalSize = content_length(response)};
    }

    const bool resuming = allowResume && plan.resume;
    if (!resuming) {
        if (response.statusCode == 206) {
            return {
                .action = DownloadAction::Restart,
                .error = "Server returned a partial response without a resume request",
            };
        }
        return {.action = DownloadAction::Replace, .totalSize = content_length(response)};
    }

    if (response.statusCode == 200) {
        return {.action = DownloadAction::Replace, .totalSize = content_length(response)};
    }
    if (response.statusCode == 416) {
        const std::string* value = header_value(response.headers, "Content-Range");
        const auto range = value == nullptr ? std::nullopt : parse_content_range(*value);
        if (const std::string* validator = header_value(response.headers, plan.validatorName);
            validator != nullptr && trim(*validator) != plan.validatorValue)
        {
            return {
                .action = DownloadAction::Restart,
                .error = "Server changed the download validator during resume",
            };
        }
        if (range && !range->start && range->total && *range->total == plan.offset &&
            (!plan.expectedSize || *plan.expectedSize == *range->total))
        {
            return {.action = DownloadAction::Complete, .totalSize = range->total};
        }
        return {
            .action = DownloadAction::Restart,
            .error = "Server rejected the download resume range",
        };
    }
    if (response.statusCode != 206) {
        return {.action = DownloadAction::Replace, .totalSize = content_length(response)};
    }
    if (!identity_encoded(response)) {
        return {
            .action = DownloadAction::Restart,
            .error = "Server encoded a resumed response",
        };
    }

    const std::string* value = header_value(response.headers, "Content-Range");
    const auto range = value == nullptr ? std::nullopt : parse_content_range(*value);
    if (!range || !range->start || !range->end || *range->start != plan.offset ||
        (range->total && (*range->end >= *range->total ||
                             (plan.expectedSize && *plan.expectedSize != *range->total))))
    {
        return {
            .action = DownloadAction::Restart,
            .error = "Server returned an invalid download resume range",
        };
    }
    if (const auto length = content_length(response);
        length && (*length == 0 || *range->end - *range->start != *length - 1))
    {
        return {
            .action = DownloadAction::Restart,
            .error = "Server returned an inconsistent download resume length",
        };
    }

    if (const std::string* validator = header_value(response.headers, plan.validatorName);
        validator != nullptr && trim(*validator) != plan.validatorValue)
    {
        return {
            .action = DownloadAction::Restart,
            .error = "Server changed the download validator during resume",
        };
    }
    return {.action = DownloadAction::Append, .totalSize = range->total};
}

void set_download_progress(borealis::detail::TaskSignals* signals, const DownloadPlan& plan,
    const DownloadDecision& decision) {
    const std::uint64_t completed =
        decision.action == DownloadAction::Append || decision.action == DownloadAction::Complete ?
            plan.offset :
            0;
    signals->completed.store(completed, std::memory_order_relaxed);
    if (decision.totalSize) {
        signals->total.store(*decision.totalSize, std::memory_order_relaxed);
        signals->totalKnown.store(true, std::memory_order_release);
    } else {
        signals->total.store(0, std::memory_order_relaxed);
        signals->totalKnown.store(false, std::memory_order_release);
    }
}

void finish_download(const DownloadPlan& plan) noexcept {
    if (!plan.managed) {
        return;
    }
    std::error_code ignored;
    std::filesystem::remove(plan.metadataPath, ignored);
}

Deadline::Tracker::Tracker() noexcept {
    reset();
}

void Deadline::Tracker::reset() noexcept {
    m_attemptStarted = Clock::now();
    m_lastActivity.store(m_attemptStarted.time_since_epoch().count(), std::memory_order_relaxed);
    m_responseStarted.store(false, std::memory_order_relaxed);
}

void Deadline::Tracker::touch() noexcept {
    m_lastActivity.store(Clock::now().time_since_epoch().count(), std::memory_order_relaxed);
}

void Deadline::Tracker::start_response() noexcept {
    touch();
    m_responseStarted.store(true, std::memory_order_release);
}

Deadline::Deadline(const Request& request)
    : m_connectTimeout{positive_timeout(request.connectTimeout)},
      m_idleTimeout{positive_timeout(request.idleTimeout)} {
    if (request.totalTimeout) {
        m_totalDeadline = Clock::now() + positive_timeout(*request.totalTimeout);
    }
}

std::chrono::milliseconds Deadline::connect_timeout() const noexcept {
    return m_connectTimeout;
}

std::chrono::milliseconds Deadline::idle_timeout() const noexcept {
    return m_idleTimeout;
}

std::optional<std::chrono::milliseconds> Deadline::remaining_total() const noexcept {
    if (!m_totalDeadline) {
        return std::nullopt;
    }
    const auto remaining = *m_totalDeadline - Clock::now();
    if (remaining <= Clock::duration::zero()) {
        return std::chrono::milliseconds{0};
    }
    const auto whole = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
    return std::max(whole, std::chrono::milliseconds{1});
}

std::chrono::milliseconds Deadline::bounded_timeout(
    std::chrono::milliseconds timeout) const noexcept {
    timeout = positive_timeout(timeout);
    if (const auto remaining = remaining_total()) {
        return std::max(std::min(timeout, *remaining), std::chrono::milliseconds{1});
    }
    return timeout;
}

bool Deadline::total_expired() const noexcept {
    return m_totalDeadline && Clock::now() >= *m_totalDeadline;
}

bool Deadline::expired(const Tracker& tracker) const noexcept {
    const TimePoint now = Clock::now();
    if (m_totalDeadline && now >= *m_totalDeadline) {
        return true;
    }
    const bool responseStarted = tracker.m_responseStarted.load(std::memory_order_acquire);
    const TimePoint lastActivity{
        Clock::duration{tracker.m_lastActivity.load(std::memory_order_relaxed)}};
    const TimePoint phaseStarted = responseStarted ? lastActivity : tracker.m_attemptStarted;
    const auto phaseTimeout = responseStarted ? m_idleTimeout : m_connectTimeout;
    return now - phaseStarted >= phaseTimeout;
}

ResponseEngine::ResponseEngine(const Request& request, const DownloadPlan& downloadPlan,
    bool allowResume, borealis::detail::TaskSignals* signals)
    : m_downloadPlan{downloadPlan}, m_maxBodyBytes{request.maxBodyBytes},
      m_allowResume{allowResume}, m_signals{signals} {}

void ResponseEngine::fail(Error error, std::string message) {
    if (m_error != Error::None) {
        return;
    }
    m_error = error;
    m_message = std::move(message);
}

bool ResponseEngine::prepare_sink() {
    if (m_downloadDecision.action != DownloadAction::Replace &&
        m_downloadDecision.action != DownloadAction::Append)
    {
        return true;
    }

    if (m_downloadDecision.action == DownloadAction::Append) {
        std::error_code sizeError;
        const std::uint64_t size =
            std::filesystem::file_size(m_downloadPlan.destination, sizeError);
        if (sizeError || size != m_downloadPlan.offset) {
            m_message = "Download destination changed before resume";
            if (sizeError) {
                m_message += ": " + sizeError.message();
            }
            return false;
        }
    }

    std::string ioError;
    const io::File::Mode mode = m_downloadDecision.action == DownloadAction::Append ?
                                    io::File::Mode::Append :
                                    io::File::Mode::Truncate;
    if (!m_output.open(m_downloadPlan.destination, mode, ioError)) {
        m_message = "Failed to open download destination: " + ioError;
        return false;
    }

    if (m_downloadPlan.managed) {
        const bool resumableStatus = m_response.statusCode == 200 || m_response.statusCode == 206;
        const auto [responseValidatorName, responseValidatorValue] = response_validator(m_response);
        std::string validatorName = responseValidatorName;
        std::string validatorValue = responseValidatorValue;
        if (m_downloadDecision.action == DownloadAction::Append && validatorValue.empty()) {
            validatorName = m_downloadPlan.validatorName;
            validatorValue = m_downloadPlan.validatorValue;
        }

        if (resumableStatus && identity_encoded(m_response) && !validatorValue.empty()) {
            if (!write_metadata(m_downloadPlan, validatorName, validatorValue,
                    m_downloadDecision.totalSize, ioError))
            {
                m_output.close(ioError);
                m_message = std::move(ioError);
                return false;
            }
        } else if (!remove_metadata(m_downloadPlan, ioError)) {
            m_output.close(ioError);
            m_message = std::move(ioError);
            return false;
        }
    } else if (!remove_metadata(m_downloadPlan, ioError)) {
        m_output.close(ioError);
        m_message = std::move(ioError);
        return false;
    }
    return true;
}

TransportObserver::Directive ResponseEngine::on_response(int status, std::vector<Header> headers) {
    try {
        if (m_responseReceived) {
            fail(Error::Network, "HTTP transport reported more than one final response");
            return Directive::Abort;
        }
        m_responseReceived = true;
        m_response.statusCode = status;
        m_response.headers = std::move(headers);

        if (m_downloadPlan.destination.empty()) {
            m_signals->completed.store(0, std::memory_order_relaxed);
            if (const auto total = content_length(m_response);
                total && identity_encoded(m_response))
            {
                m_signals->total.store(*total, std::memory_order_relaxed);
                m_signals->totalKnown.store(true, std::memory_order_release);
            } else {
                m_signals->total.store(0, std::memory_order_relaxed);
                m_signals->totalKnown.store(false, std::memory_order_release);
            }
            return Directive::Continue;
        }

        m_downloadDecision = evaluate_download_response(m_downloadPlan, m_response, m_allowResume);
        set_download_progress(m_signals, m_downloadPlan, m_downloadDecision);
        if (m_downloadDecision.action == DownloadAction::Restart) {
            m_restart = true;
            return Directive::Abort;
        }
        if (m_downloadDecision.action == DownloadAction::Complete) {
            m_alreadyComplete = true;
            return Directive::Abort;
        }
        if (!prepare_sink()) {
            fail(Error::Io, m_message);
            return Directive::Abort;
        }
        return Directive::Continue;
    } catch (const std::exception& exception) {
        fail(Error::Network, exception.what());
        return Directive::Abort;
    } catch (...) {
        fail(Error::Network, "HTTP response callback failed");
        return Directive::Abort;
    }
}

TransportObserver::Directive ResponseEngine::on_data(std::span<const std::byte> chunk) {
    try {
        if (!m_responseReceived) {
            fail(Error::Network, "HTTP transport delivered data before a response");
            return Directive::Abort;
        }
        if (m_restart || m_alreadyComplete || m_error != Error::None) {
            return Directive::Abort;
        }

        if (!m_downloadPlan.destination.empty()) {
            std::string ioError;
            if (!m_output.is_open() || !m_output.write(chunk, ioError)) {
                fail(Error::Io, ioError.empty() ?
                                    "Failed to write download destination" :
                                    "Failed to write download destination: " + ioError);
                return Directive::Abort;
            }
        } else {
            if (chunk.size() > m_maxBodyBytes ||
                m_response.body.size() > m_maxBodyBytes - chunk.size())
            {
                fail(Error::TooLarge, "Response body exceeded the configured limit");
                return Directive::Abort;
            }
            m_response.body.append(reinterpret_cast<const char*>(chunk.data()), chunk.size());
        }

        m_signals->completed.fetch_add(chunk.size(), std::memory_order_relaxed);
        return Directive::Continue;
    } catch (const std::exception& exception) {
        fail(Error::Network, exception.what());
        return Directive::Abort;
    } catch (...) {
        fail(Error::Network, "HTTP data callback failed");
        return Directive::Abort;
    }
}

AttemptResult ResponseEngine::finish(TransportResult transportResult) {
    std::string closeError;
    if (!m_output.close(closeError)) {
        fail(Error::Io, "Failed to close download destination: " + closeError);
    }

    if (m_restart) {
        return {
            .result =
                {
                    .error = Error::Network,
                    .message = m_downloadDecision.error,
                    .response = std::move(m_response),
                },
            .restart = true,
        };
    }
    if (m_alreadyComplete) {
        finish_download(m_downloadPlan);
        m_response.statusCode = 200;
        return {.result = {.response = std::move(m_response)}};
    }
    if (m_error != Error::None) {
        return {.result = {
                    .error = m_error,
                    .message = std::move(m_message),
                    .response = std::move(m_response),
                }};
    }
    if (transportResult.error != Error::None) {
        return {.result = {
                    .error = transportResult.error,
                    .message = std::move(transportResult.message),
                    .response = std::move(m_response),
                }};
    }
    if (!m_responseReceived) {
        return {.result = {
                    .error = Error::Network,
                    .message = "HTTP transport completed without a response",
                }};
    }

    if (!m_downloadPlan.destination.empty()) {
        finish_download(m_downloadPlan);
    }
    return {.result = {.response = std::move(m_response)}};
}

Result perform(
    const Request& request, borealis::detail::TaskSignals* signals, const Transport& transport) {
    const DownloadPlan downloadPlan = prepare_download(request);
    if (!downloadPlan.error.empty()) {
        return {
            .error = Error::Io,
            .message = downloadPlan.error,
        };
    }

    Deadline deadline{request};
    bool allowResume = downloadPlan.resume;
    for (;;) {
        if (signals->cancelRequested.load(std::memory_order_relaxed)) {
            return {
                .error = Error::Canceled,
                .message = "Request canceled",
            };
        }
        if (deadline.total_expired()) {
            return {
                .error = Error::Timeout,
                .message = "Request timed out",
            };
        }

        ResponseEngine engine{request, downloadPlan, allowResume, signals};
        TransportResult transportResult;
        try {
            transportResult = transport({
                .method = request.method,
                .url = request.url,
                .body = request.method == Method::Post ? std::string_view{request.body} :
                                                         std::string_view{},
                .headers = request_headers(request, downloadPlan, allowResume),
                .allowCompression = request.downloadTo.empty(),
                .deadline = &deadline,
                .observer = &engine,
                .signals = signals,
            });
        } catch (const std::exception& exception) {
            transportResult = {
                .error = Error::Network,
                .message = exception.what(),
            };
        } catch (...) {
            transportResult = {
                .error = Error::Network,
                .message = "HTTP transport failed",
            };
        }

        AttemptResult result = engine.finish(std::move(transportResult));
        if (!result.restart) {
            return std::move(result.result);
        }
        if (!allowResume) {
            return std::move(result.result);
        }
        allowResume = false;
    }
}

Result perform(const Request& request, borealis::detail::TaskSignals* signals) {
    return perform(request, signals, send_request);
}

}  // namespace borealis::http::detail
