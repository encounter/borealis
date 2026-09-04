#pragma once

#include "borealis/http.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace borealis::ws {

enum class Error {
    None,
    NoBackend,
    InvalidUrl,
    UnsupportedScheme,
    Timeout,
    TooLarge,
    Canceled,
    Network,
    Protocol,
    Handshake,
};

struct Options {
    std::string url;
    std::vector<http::Header> headers;
    std::vector<std::string> protocols;
    std::chrono::milliseconds connectTimeout{10000};
    std::chrono::milliseconds closeTimeout{5000};
    std::chrono::milliseconds keepaliveInterval{0};
    size_t maxMessageBytes = 1024 * 1024;
    size_t maxQueuedBytes = 16 * 1024 * 1024;
    size_t maxSendQueueBytes = 4 * 1024 * 1024;
    bool allowPlaintext = false;
};

enum class MessageKind {
    Text,
    Binary,
};

enum class SendResult {
    Ok,
    NotOpen,
    QueueFull,
    TooLarge,
    InvalidText,
};

enum class CloseResult {
    Ok,
    NotOpen,
    InvalidCode,
    ReasonTooLarge,
    InvalidText,
};

struct Event {
    enum class Kind {
        Open,
        Message,
        Closed,
    } kind = Kind::Closed;

    std::string protocol;
    std::vector<http::Header> headers;
    MessageKind messageKind = MessageKind::Text;
    std::string data;
    Error error = Error::None;
    std::string message;
    int status = 0;
    uint16_t code = 0;
    std::string reason;
};

namespace detail {
class ConnectionAccess;
class ConnectionState;
}  // namespace detail

class Connection {
public:
    enum class State {
        Connecting,
        Open,
        Closing,
        Closed,
    };

    Connection() = default;
    Connection(Connection&& other) noexcept;
    Connection& operator=(Connection&& other) noexcept;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    ~Connection();

    explicit operator bool() const noexcept;
    State state() const noexcept;
    /** Returns false without blocking when no event is queued. */
    bool poll(Event& out);
    /** Copies a message into the outbound queue. */
    SendResult send(MessageKind kind, std::string_view data);
    /** Starts a close handshake, respecting Options::closeTimeout. */
    CloseResult close(uint16_t code = 1000, std::string_view reason = {});

private:
    explicit Connection(std::shared_ptr<detail::ConnectionState> state);
    void reset() noexcept;

    std::shared_ptr<detail::ConnectionState> m_state;

    friend class detail::ConnectionAccess;
    friend Connection connect(Options options);
};

bool available() noexcept;
Connection connect(Options options);

}  // namespace borealis::ws
