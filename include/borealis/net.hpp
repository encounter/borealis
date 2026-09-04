#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace borealis::net {

enum class Error {
    None,
    InvalidEndpoint,
    Resolve,
    Timeout,
    Refused,
    Unreachable,
    Reset,
    AddressInUse,
    Permission,
    TooLarge,
    Canceled,
    Network,
};

using SocketId = uint64_t;

enum class SendResult {
    Ok,
    NotOpen,
    QueueFull,
    TooLarge,
    InvalidEndpoint,
};

struct ParsedEndpoint {
    std::string scheme;
    std::string host;
    uint16_t port = 0;
    bool literal = false;
    bool ipv6 = false;
};

struct ContextOptions {
    size_t maxQueuedBytes = 16 * 1024 * 1024;
    size_t maxQueuedEvents = 4096;
    size_t maxStreams = 0;
    size_t maxListeners = 0;
    size_t maxDatagramSockets = 0;
    size_t maxResolvers = 0;
};

struct StreamOptions {
    std::chrono::milliseconds connectTimeout{10000};
    std::chrono::milliseconds closeTimeout{5000};
    std::chrono::milliseconds inboundStallTimeout{30000};
    size_t maxSendQueueBytes = 1024 * 1024;
    size_t readChunkBytes = 64 * 1024;
    bool noDelay = true;
};

struct ListenOptions {
    StreamOptions accepted;
    int backlog = 64;
    bool reuseAddress = true;
};

struct DatagramOptions {
    size_t maxSendQueueBytes = 1024 * 1024;
    size_t recvBufferBytes = 1024 * 1024;
    size_t sendBufferBytes = 1024 * 1024;
};

struct Event {
    enum class Kind {
        Connected,
        Accepted,
        StreamData,
        Datagram,
        Dropped,
        Resolved,
        Closed,
    };

    Kind kind = Kind::Closed;
    SocketId id = 0;
    void* userData = nullptr;
    SocketId accepted = 0;
    std::string endpoint;
    /** Data remains valid until the context's next poll call. */
    std::span<const std::byte> data;
    uint32_t dropped = 0;
    Error error = Error::None;
    std::string message;
};

struct Stats {
    size_t queuedSendBytes = 0;
    uint64_t inboundDropped = 0;
    uint64_t sendFailures = 0;
    uint64_t bytesSent = 0;
    uint64_t bytesReceived = 0;
};

class Context {
public:
    explicit Context(ContextOptions options = {});
    ~Context();

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&&) = delete;
    Context& operator=(Context&&) = delete;

    SocketId connect(
        std::string_view endpoint, StreamOptions options = {}, void* userData = nullptr);

    struct BindResult {
        SocketId id = 0;
        std::string localEndpoint;
        Error error = Error::None;
        std::string message;
    };

    BindResult listen(std::string_view bind, ListenOptions options = {}, void* userData = nullptr);
    BindResult open_datagram(
        std::string_view bind, DatagramOptions options = {}, void* userData = nullptr);
    /** Resolves an endpoint without blocking. */
    SocketId resolve(std::string_view endpoint, void* userData = nullptr);

    /** Copies bytes into a connected stream's outbound queue. */
    SendResult send(SocketId stream, std::span<const std::byte> bytes);
    /** Copies one datagram into a UDP socket's outbound queue. */
    SendResult send_to(
        SocketId datagram, std::string_view endpoint, std::span<const std::byte> bytes);
    void set_user_data(SocketId id, void* userData);
    std::optional<Stats> stats(SocketId id) const;
    /** Gracefully closes a TCP stream, or immediately closes another socket kind. */
    void close(SocketId id);
    /** Returns false without blocking when the context's event queue is empty. */
    bool poll(Event& out);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

bool available() noexcept;
/** Parses and canonicalizes a TCP or UDP endpoint. */
std::optional<ParsedEndpoint> parse_endpoint(std::string_view text);
std::string format_endpoint(const ParsedEndpoint& endpoint);

}  // namespace borealis::net
