#include "borealis/ws.hpp"

#include "borealis/log.hpp"
#include "borealis/task.hpp"
#include "borealis/url.hpp"
#include "ws_internal.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace {
constexpr borealis::Log Log{"borealis::ws"};
}

namespace borealis::ws {
namespace detail {

bool valid_utf8(std::string_view text) {
    const auto* cursor = reinterpret_cast<const unsigned char*>(text.data());
    const auto* end = cursor + text.size();
    while (cursor < end) {
        const unsigned char first = *cursor++;
        if (first <= 0x7f) {
            continue;
        }
        int continuation = 0;
        uint32_t value = 0;
        uint32_t minimum = 0;
        if ((first & 0xe0) == 0xc0) {
            continuation = 1;
            value = first & 0x1f;
            minimum = 0x80;
        } else if ((first & 0xf0) == 0xe0) {
            continuation = 2;
            value = first & 0x0f;
            minimum = 0x800;
        } else if ((first & 0xf8) == 0xf0) {
            continuation = 3;
            value = first & 0x07;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (end - cursor < continuation) {
            return false;
        }
        for (int index = 0; index < continuation; ++index) {
            if ((*cursor & 0xc0) != 0x80) {
                return false;
            }
            value = (value << 6) | (*cursor++ & 0x3f);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) {
            return false;
        }
    }
    return true;
}

Options canonicalize_options(Options options) {
    if (!options.protocols.empty()) {
        std::string value;
        for (size_t index = 0; index < options.protocols.size(); ++index) {
            if (index != 0) {
                value += ", ";
            }
            value += options.protocols[index];
        }
        options.headers.push_back({
            .name = "Sec-WebSocket-Protocol",
            .value = std::move(value),
        });
    }
    if (options.keepaliveInterval.count() != 0) {
        options.keepaliveInterval =
            std::max(options.keepaliveInterval, std::chrono::milliseconds{15000});
    }
    return options;
}

class TransportOwner;

class TransportCall {
public:
    TransportCall(const TransportCall&) = delete;
    TransportCall& operator=(const TransportCall&) = delete;

    TransportCall(TransportCall&& other) noexcept
        : owner{std::exchange(other.owner, nullptr)}, transport{other.transport} {}

    ~TransportCall();

    explicit operator bool() const noexcept { return transport != nullptr; }
    Transport* operator->() const noexcept { return transport; }

private:
    TransportCall(TransportOwner* value, Transport* target) : owner{value}, transport{target} {}

    TransportOwner* owner = nullptr;
    Transport* transport = nullptr;

    friend class TransportOwner;
};

class TransportOwner {
public:
    ~TransportOwner() { abort(); }

    bool install(std::unique_ptr<Transport> value) {
        std::lock_guard lock{mutex};
        if (stopping || transport != nullptr) {
            return false;
        }
        transport = std::move(value);
        return transport != nullptr;
    }

    TransportCall call() {
        std::lock_guard lock{mutex};
        if (stopping || transport == nullptr) {
            return {nullptr, nullptr};
        }
        ++activeCalls;
        return {this, transport.get()};
    }

    void abort() noexcept {
        std::unique_ptr<Transport> target;
        {
            std::unique_lock lock{mutex};
            if (stopping) {
                return;
            }
            stopping = true;
            callsComplete.wait(lock, [this] { return activeCalls == 0; });
            target = std::move(transport);
        }
        if (target != nullptr) {
            target->abort();
        }
    }

private:
    void complete_call() noexcept {
        std::lock_guard lock{mutex};
        --activeCalls;
        if (activeCalls == 0) {
            callsComplete.notify_all();
        }
    }

    std::mutex mutex;
    std::condition_variable callsComplete;
    std::unique_ptr<Transport> transport;
    size_t activeCalls = 0;
    bool stopping = false;

    friend class TransportCall;
};

TransportCall::~TransportCall() {
    if (owner != nullptr) {
        owner->complete_call();
    }
}

class ConnectionState final : public EventSink,
                              public std::enable_shared_from_this<ConnectionState> {
public:
    explicit ConnectionState(Options value) : options{std::move(value)} {}

    void start(const TransportFactory& factory) {
        if (!transport.install(factory())) {
            closed(Error::NoBackend, "No WebSocket backend is available", 0, 0, {}, {});
            return;
        }
        connectDeadline = std::chrono::steady_clock::now() + options.connectTimeout;
        auto call = transport.call();
        if (!call) {
            closed(Error::Canceled, "Connection canceled", 0, 0, {}, {});
            return;
        }
        call->start(options, shared_from_this());
        arm_deadline(shared_from_this());
    }

    Connection::State state() const noexcept {
        std::lock_guard lock{mutex};
        return currentState;
    }

    bool poll(Event& output) {
        std::lock_guard lock{mutex};
        if (events.empty()) {
            return false;
        }
        output = std::move(events.front());
        events.pop_front();
        if (output.kind == Event::Kind::Message) {
            queuedInboundBytes -= output.data.size();
        }
        return true;
    }

    SendResult send(MessageKind kind, std::string_view data) {
        if (data.size() > options.maxMessageBytes) {
            return SendResult::TooLarge;
        }
        if (kind == MessageKind::Text && !valid_utf8(data)) {
            return SendResult::InvalidText;
        }
        {
            std::lock_guard lock{mutex};
            if (currentState != Connection::State::Open) {
                return SendResult::NotOpen;
            }
            if (data.size() >
                options.maxSendQueueBytes - std::min(queuedSendBytes, options.maxSendQueueBytes))
            {
                return SendResult::QueueFull;
            }
            queuedSendBytes += data.size();
        }
        auto call = transport.call();
        if (call && call->send(kind, std::string{data})) {
            return SendResult::Ok;
        }
        std::lock_guard lock{mutex};
        queuedSendBytes -= std::min(queuedSendBytes, data.size());
        return SendResult::NotOpen;
    }

    CloseResult close(uint16_t code, std::string_view reason) {
        const CloseResult validation = validate_close(code, reason);
        if (validation != CloseResult::Ok) {
            return validation;
        }
        std::unique_lock lock{mutex};
        if (currentState == Connection::State::Closed || currentState == Connection::State::Closing)
        {
            return CloseResult::Ok;
        }
        if (currentState == Connection::State::Connecting) {
            currentState = Connection::State::Closed;
            events.push_back({
                .kind = Event::Kind::Closed,
                .error = Error::Canceled,
                .message = "Connection canceled",
            });
            lock.unlock();
            transport.abort();
            return CloseResult::Ok;
        }
        currentState = Connection::State::Closing;
        closeDeadline = std::chrono::steady_clock::now() + options.closeTimeout;
        lock.unlock();
        arm_deadline(shared_from_this());
        if (auto call = transport.call()) {
            call->close(code, std::string{reason});
        }
        return CloseResult::Ok;
    }

    void abort_discard() noexcept {
        bool sendClose = false;
        {
            std::lock_guard lock{mutex};
            if (discarded) {
                return;
            }
            sendClose = currentState != Connection::State::Closed;
            discarded = true;
            currentState = Connection::State::Closed;
            events.clear();
            queuedInboundBytes = 0;
            queuedSendBytes = 0;
        }
        if (sendClose) {
            if (auto call = transport.call()) {
                call->close(1001, "Connection owner released");
            }
        }
        transport.abort();
    }

    std::optional<std::chrono::steady_clock::time_point> deadline() const {
        std::lock_guard lock{mutex};
        if (currentState != Connection::State::Connecting &&
            currentState != Connection::State::Closing)
        {
            return std::nullopt;
        }
        return currentState == Connection::State::Connecting ? connectDeadline : closeDeadline;
    }

    void deadline_expired() {
        {
            std::lock_guard lock{mutex};
            const auto now = std::chrono::steady_clock::now();
            const bool connectExpired =
                currentState == Connection::State::Connecting && now >= connectDeadline;
            const bool closeExpired =
                currentState == Connection::State::Closing && now >= closeDeadline;
            if (currentState == Connection::State::Closed || (!connectExpired && !closeExpired)) {
                return;
            }
            currentState = Connection::State::Closed;
            queuedSendBytes = 0;
            if (!discarded) {
                const bool forced = forcedError != Error::None;
                events.push_back({
                    .kind = Event::Kind::Closed,
                    .error = forced ? forcedError : Error::Timeout,
                    .message = forced         ? forcedMessage :
                               connectExpired ? "WebSocket handshake timed out" :
                                                "Close handshake timed out",
                });
            }
        }
        transport.abort();
    }

    void opened(std::string protocol, std::vector<http::Header> headers) override {
        {
            std::lock_guard lock{mutex};
            if (discarded || currentState != Connection::State::Connecting) {
                return;
            }
            const bool offered = protocol.empty() ||
                                 std::ranges::any_of(options.protocols,
                                     [&](const auto& candidate) { return candidate == protocol; });
            if (offered) {
                currentState = Connection::State::Open;
                events.push_back({
                    .kind = Event::Kind::Open,
                    .protocol = std::move(protocol),
                    .headers = std::move(headers),
                });
                return;
            }
            forcedError = Error::Protocol;
            forcedMessage = "Server selected an unoffered WebSocket protocol";
            currentState = Connection::State::Closing;
            closeDeadline = std::chrono::steady_clock::now() + options.closeTimeout;
        }
        arm_deadline(shared_from_this());
        if (auto call = transport.call()) {
            call->close(1002, "Server selected an unoffered WebSocket protocol");
        }
    }

    void message(MessageKind kind, std::string data) override {
        bool overflow = false;
        {
            std::lock_guard lock{mutex};
            if (discarded || currentState != Connection::State::Open) {
                return;
            }
            const bool invalidText = kind == MessageKind::Text && !valid_utf8(data);
            const bool tooLarge =
                data.size() > options.maxMessageBytes ||
                data.size() >
                    options.maxQueuedBytes - std::min(queuedInboundBytes, options.maxQueuedBytes);
            overflow = invalidText || tooLarge;
            if (!overflow) {
                queuedInboundBytes += data.size();
                events.push_back({
                    .kind = Event::Kind::Message,
                    .messageKind = kind,
                    .data = std::move(data),
                });
                return;
            }
            forcedError = tooLarge ? Error::TooLarge : Error::Protocol;
            forcedMessage = tooLarge ? "Message limit exceeded" : "Invalid UTF-8";
            currentState = Connection::State::Closing;
            closeDeadline = std::chrono::steady_clock::now() + options.closeTimeout;
        }
        arm_deadline(shared_from_this());
        if (auto call = transport.call()) {
            call->close(forcedError == Error::TooLarge ? 1009 : 1007,
                forcedError == Error::TooLarge ? "Message limit exceeded" : "Invalid UTF-8");
        }
    }

    void send_complete(size_t bytes, Error error, std::string message) override {
        {
            std::lock_guard lock{mutex};
            queuedSendBytes -= std::min(queuedSendBytes, bytes);
        }
        if (error != Error::None) {
            closed(error, std::move(message), 0, 0, {}, {});
        }
    }

    void closed(Error error, std::string message, int status, uint16_t code, std::string reason,
        std::vector<http::Header> headers) override {
        std::lock_guard lock{mutex};
        if (currentState == Connection::State::Closed) {
            return;
        }
        currentState = Connection::State::Closed;
        queuedSendBytes = 0;
        if (discarded) {
            return;
        }
        if (forcedError != Error::None) {
            error = forcedError;
            message = forcedMessage;
        }
        events.push_back({
            .kind = Event::Kind::Closed,
            .headers = std::move(headers),
            .error = error,
            .message = std::move(message),
            .status = status,
            .code = code,
            .reason = std::move(reason),
        });
    }

private:
    static CloseResult validate_close(uint16_t code, std::string_view reason) {
        if (code != 1000 && code != 1001 && (code < 3000 || code > 4999)) {
            return CloseResult::InvalidCode;
        }
        if (reason.size() > 123) {
            return CloseResult::ReasonTooLarge;
        }
        return valid_utf8(reason) ? CloseResult::Ok : CloseResult::InvalidText;
    }

    static void arm_deadline(const std::shared_ptr<ConnectionState>& state);

    mutable std::mutex mutex;
    Options options;
    TransportOwner transport;
    std::deque<Event> events;
    Connection::State currentState = Connection::State::Connecting;
    size_t queuedInboundBytes = 0;
    size_t queuedSendBytes = 0;
    std::chrono::steady_clock::time_point connectDeadline{};
    std::chrono::steady_clock::time_point closeDeadline{};
    Error forcedError = Error::None;
    std::string forcedMessage;
    bool discarded = false;

    friend class DeadlineManager;
};

class DeadlineManager {
public:
    ~DeadlineManager() { shutdown(); }

    void arm(const std::shared_ptr<ConnectionState>& state) {
        std::lock_guard lock{mutex};
        states.emplace_back(state);
        if (!thread.joinable()) {
            stopping = false;
            thread = std::thread{[this] { run(); }};
        }
        condition.notify_all();
    }

    void shutdown() noexcept {
        std::thread worker;
        {
            std::lock_guard lock{mutex};
            if (!thread.joinable()) {
                states.clear();
                return;
            }
            stopping = true;
            worker = std::move(thread);
            condition.notify_all();
        }
        try {
            worker.join();
        }
        BOREALIS_CATCH_FATAL()
        std::lock_guard lock{mutex};
        states.clear();
        stopping = false;
    }

private:
    void run() noexcept try {
        std::unique_lock lock{mutex};
        while (!stopping) {
            std::vector<std::shared_ptr<ConnectionState>> expired;
            const auto now = std::chrono::steady_clock::now();
            std::optional<std::chrono::steady_clock::time_point> nextDeadline;
            std::erase_if(states, [&](const auto& weak) {
                auto state = weak.lock();
                if (!state) {
                    return true;
                }
                const auto deadline = state->deadline();
                if (!deadline) {
                    return true;
                }
                if (*deadline <= now) {
                    expired.emplace_back(std::move(state));
                    return true;
                }
                if (!nextDeadline || *deadline < *nextDeadline) {
                    nextDeadline = *deadline;
                }
                return false;
            });
            if (!expired.empty()) {
                lock.unlock();
                for (const auto& state : expired) {
                    state->deadline_expired();
                }
                lock.lock();
                continue;
            }
            if (nextDeadline) {
                condition.wait_until(lock, *nextDeadline);
            } else {
                condition.wait(lock, [this] { return stopping || !states.empty(); });
            }
        }
    }
    BOREALIS_CATCH()

    std::mutex mutex;
    std::condition_variable condition;
    std::vector<std::weak_ptr<ConnectionState>> states;
    std::thread thread;
    bool stopping = false;
};

DeadlineManager& deadlines() {
    static DeadlineManager manager;
    return manager;
}

void ConnectionState::arm_deadline(const std::shared_ptr<ConnectionState>& state) {
    deadlines().arm(state);
}

std::mutex g_connectionsMutex;
std::vector<std::weak_ptr<ConnectionState>> g_connections;

void shutdown_connections() noexcept {
    std::vector<std::shared_ptr<ConnectionState>> connections;
    std::lock_guard lock{g_connectionsMutex};
    for (const auto& weak : g_connections) {
        if (auto state = weak.lock()) {
            connections.emplace_back(std::move(state));
        }
    }
    g_connections.clear();
    for (const auto& state : connections) {
        state->abort_discard();
    }
    deadlines().shutdown();
}

bool valid_url(const Options& options, Error& error, std::string& message) {
    const auto parsed = borealis::url::parse(options.url);
    if (!parsed) {
        error = Error::InvalidUrl;
        message = "Invalid WebSocket URL";
        return false;
    }
    if (parsed->scheme != "wss" && parsed->scheme != "ws") {
        error = Error::UnsupportedScheme;
        message = "Only wss:// and explicitly enabled ws:// URLs are supported";
        return false;
    }
    if (parsed->scheme == "wss" || options.allowPlaintext) {
        return true;
    }
    error = Error::UnsupportedScheme;
    message = "Plaintext WebSocket URLs are disabled";
    return false;
}

class ConnectionAccess {
public:
    static Connection create(
        Options options, const TransportFactory& factory, bool transportAvailable) {
        static std::once_flag registered;
        std::call_once(
            registered, [] { borealis::detail::register_shutdown_hook(shutdown_connections); });

        options = canonicalize_options(std::move(options));
        Error error = Error::None;
        std::string message;
        const bool urlValid = valid_url(options, error, message);
        auto state = std::make_shared<ConnectionState>(std::move(options));
        {
            std::lock_guard lock{g_connectionsMutex};
            std::erase_if(g_connections, [](const auto& value) { return value.expired(); });
            g_connections.emplace_back(state);
            if (!urlValid) {
                state->closed(error, std::move(message), 0, 0, {}, {});
            } else if (!transportAvailable) {
                state->closed(Error::NoBackend, "No WebSocket backend is available", 0, 0, {}, {});
            } else {
                state->start(factory);
            }
        }
        return Connection{std::move(state)};
    }
};

Connection connect_for_testing(Options options, TransportFactory factory) {
    return ConnectionAccess::create(std::move(options), factory, true);
}

}  // namespace detail

Connection::Connection(std::shared_ptr<detail::ConnectionState> state)
    : m_state{std::move(state)} {}

Connection::Connection(Connection&& other) noexcept : m_state{std::move(other.m_state)} {}

Connection& Connection::operator=(Connection&& other) noexcept {
    if (this != &other) {
        reset();
        m_state = std::move(other.m_state);
    }
    return *this;
}

Connection::~Connection() {
    reset();
}

void Connection::reset() noexcept {
    if (m_state) {
        m_state->abort_discard();
        m_state.reset();
    }
}

Connection::operator bool() const noexcept {
    return m_state != nullptr;
}

Connection::State Connection::state() const noexcept {
    return m_state ? m_state->state() : State::Closed;
}

bool Connection::poll(Event& out) {
    return m_state && m_state->poll(out);
}

SendResult Connection::send(MessageKind kind, std::string_view data) {
    return m_state ? m_state->send(kind, data) : SendResult::NotOpen;
}

CloseResult Connection::close(uint16_t code, std::string_view reason) {
    return m_state ? m_state->close(code, reason) : CloseResult::NotOpen;
}

bool available() noexcept {
    return detail::backend_available();
}

Connection connect(Options options) {
    return detail::ConnectionAccess::create(
        std::move(options), [] { return detail::make_transport(); }, available());
}

}  // namespace borealis::ws
