#include "borealis/task.hpp"
#include "borealis/ws.hpp"
#include "ws_internal.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

namespace borealis::ws {
namespace {

using namespace std::chrono_literals;

uint16_t g_lastCloseCode = 0;
int g_closeCalls = 0;
std::atomic_int g_abortCalls = 0;
std::atomic_bool g_transportCallActive = false;
std::atomic_bool g_concurrentAbort = false;
std::atomic_int g_closeDelayMs = 0;

class FakeTransport final : public detail::Transport {
public:
    void start(const Options& value, std::shared_ptr<detail::EventSink> eventSink) override {
        startedOptions = value;
        sink = std::move(eventSink);
    }

    bool send(MessageKind kind, std::string data) override {
        sentKind = kind;
        sent = std::move(data);
        return acceptSends;
    }

    void close(uint16_t code, std::string reason) override {
        g_transportCallActive.store(true, std::memory_order_release);
        if (const int delay = g_closeDelayMs.load(std::memory_order_relaxed); delay != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds{delay});
        }
        closeCode = code;
        closeReason = std::move(reason);
        ++closeCalls;
        g_lastCloseCode = code;
        ++g_closeCalls;
        g_transportCallActive.store(false, std::memory_order_release);
    }

    void abort() noexcept override {
        if (g_transportCallActive.load(std::memory_order_acquire)) {
            g_concurrentAbort.store(true, std::memory_order_relaxed);
        }
        ++abortCalls;
        ++g_abortCalls;
    }

    std::shared_ptr<detail::EventSink> sink;
    Options startedOptions;
    std::string sent;
    std::string closeReason;
    MessageKind sentKind = MessageKind::Text;
    uint16_t closeCode = 0;
    int closeCalls = 0;
    int abortCalls = 0;
    bool acceptSends = true;
};

FakeTransport* g_transport = nullptr;

std::unique_ptr<detail::Transport> make_fake() {
    auto transport = std::make_unique<FakeTransport>();
    g_transport = transport.get();
    return transport;
}

class WebSocketTest : public ::testing::Test {
protected:
    void SetUp() override {
        g_transport = nullptr;
        g_lastCloseCode = 0;
        g_closeCalls = 0;
        g_abortCalls = 0;
        g_transportCallActive.store(false, std::memory_order_relaxed);
        g_concurrentAbort.store(false, std::memory_order_relaxed);
        g_closeDelayMs.store(0, std::memory_order_relaxed);
    }

    void TearDown() override {
        borealis::shutdown();
        g_transport = nullptr;
    }

    Connection connect_fake(Options options) {
        return detail::connect_for_testing(std::move(options), make_fake);
    }
};

TEST_F(WebSocketTest, OrdersOpenMessagesAndClosed) {
    Connection connection = connect_fake({
        .url = "wss://example.test/socket",
        .protocols = {"chat"},
    });
    ASSERT_NE(g_transport, nullptr);
    g_transport->sink->opened("chat", {{"X-Test", "value"}});
    g_transport->sink->message(MessageKind::Text, "hello");
    g_transport->sink->closed(Error::None, {}, 0, 1000, "done");

    Event event;
    ASSERT_TRUE(connection.poll(event));
    EXPECT_EQ(event.kind, Event::Kind::Open);
    EXPECT_EQ(event.protocol, "chat");
    ASSERT_TRUE(connection.poll(event));
    EXPECT_EQ(event.kind, Event::Kind::Message);
    EXPECT_EQ(event.data, "hello");
    ASSERT_TRUE(connection.poll(event));
    EXPECT_EQ(event.kind, Event::Kind::Closed);
    EXPECT_EQ(event.error, Error::None);
    EXPECT_EQ(event.code, 1000);
    EXPECT_EQ(event.reason, "done");
    EXPECT_FALSE(connection.poll(event));
}

TEST_F(WebSocketTest, EnforcesInboundAndMessageCaps) {
    Connection connection = connect_fake({
        .url = "wss://example.test/socket",
        .closeTimeout = 1s,
        .maxMessageBytes = 4,
        .maxQueuedBytes = 4,
    });
    ASSERT_NE(g_transport, nullptr);
    g_transport->sink->opened({}, {});
    g_transport->sink->message(MessageKind::Binary, "12345");
    EXPECT_EQ(g_transport->closeCode, 1009);
    EXPECT_EQ(g_transport->closeCalls, 1);
    g_transport->sink->closed(Error::None, {}, 0, 1009, {});

    Event event;
    ASSERT_TRUE(connection.poll(event));
    EXPECT_EQ(event.kind, Event::Kind::Open);
    ASSERT_TRUE(connection.poll(event));
    EXPECT_EQ(event.kind, Event::Kind::Closed);
    EXPECT_EQ(event.error, Error::TooLarge);
}

TEST_F(WebSocketTest, EnforcesUnreadQueueCapWithoutDiscardingEarlierMessages) {
    Connection connection = connect_fake({
        .url = "wss://example.test/socket",
        .closeTimeout = 1s,
        .maxMessageBytes = 8,
        .maxQueuedBytes = 4,
    });
    g_transport->sink->opened({}, {});
    g_transport->sink->message(MessageKind::Text, "123");
    g_transport->sink->message(MessageKind::Text, "45");
    EXPECT_EQ(g_transport->closeCode, 1009);
    g_transport->sink->closed(Error::None, {}, 0, 1009, {});

    Event event;
    ASSERT_TRUE(connection.poll(event));
    EXPECT_EQ(event.kind, Event::Kind::Open);
    ASSERT_TRUE(connection.poll(event));
    EXPECT_EQ(event.kind, Event::Kind::Message);
    EXPECT_EQ(event.data, "123");
    ASSERT_TRUE(connection.poll(event));
    EXPECT_EQ(event.kind, Event::Kind::Closed);
    EXPECT_EQ(event.error, Error::TooLarge);
}

TEST_F(WebSocketTest, AccountsOutboundBytesUntilCompletion) {
    Connection connection = connect_fake({
        .url = "wss://example.test/socket",
        .maxMessageBytes = 8,
        .maxSendQueueBytes = 4,
    });
    EXPECT_EQ(connection.send(MessageKind::Text, "open"), SendResult::NotOpen);
    g_transport->sink->opened({}, {});
    EXPECT_EQ(connection.send(MessageKind::Binary, "123456789"), SendResult::TooLarge);
    EXPECT_EQ(connection.send(MessageKind::Text, "1234"), SendResult::Ok);
    EXPECT_EQ(connection.send(MessageKind::Text, "5"), SendResult::QueueFull);
    EXPECT_EQ(g_transport->sent, "1234");
    g_transport->sink->send_complete(4, Error::None, {});
    EXPECT_EQ(connection.send(MessageKind::Text, "5"), SendResult::Ok);
    EXPECT_EQ(connection.send(MessageKind::Text, std::string{"\xff", 1}), SendResult::InvalidText);
}

TEST_F(WebSocketTest, CloseDeadlineAbortsTransport) {
    Connection connection = connect_fake({
        .url = "wss://example.test/socket",
        .closeTimeout = 20ms,
    });
    g_transport->sink->opened({}, {});
    EXPECT_EQ(connection.close(1000, "done"), CloseResult::Ok);
    EXPECT_EQ(g_transport->closeCalls, 1);

    Event event;
    ASSERT_TRUE(connection.poll(event));
    ASSERT_EQ(event.kind, Event::Kind::Open);
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (!connection.poll(event) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_EQ(event.kind, Event::Kind::Closed);
    EXPECT_EQ(event.error, Error::Timeout);
}

TEST_F(WebSocketTest, ConnectDeadlineAbortsTransport) {
    Connection connection = connect_fake({
        .url = "wss://example.test/socket",
        .connectTimeout = 20ms,
    });

    Event event;
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (!connection.poll(event) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_EQ(event.kind, Event::Kind::Closed);
    EXPECT_EQ(event.error, Error::Timeout);
}

TEST_F(WebSocketTest, TeardownWaitsForActiveTransportCall) {
    Connection connection = connect_fake({
        .url = "wss://example.test/socket",
        .closeTimeout = 5ms,
    });
    g_transport->sink->opened({}, {});
    g_closeDelayMs.store(40, std::memory_order_relaxed);
    EXPECT_EQ(connection.close(), CloseResult::Ok);

    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (g_abortCalls == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_EQ(g_abortCalls.load(std::memory_order_relaxed), 1);
    EXPECT_FALSE(g_concurrentAbort.load(std::memory_order_relaxed));
}

TEST_F(WebSocketTest, CanonicalizesSharedBackendOptions) {
    Connection connection = connect_fake({
        .url = "wss://example.test/socket",
        .protocols = {"chat", "events"},
        .keepaliveInterval = 1s,
    });
    ASSERT_NE(g_transport, nullptr);
    EXPECT_EQ(g_transport->startedOptions.protocols, (std::vector<std::string>{"chat", "events"}));
    EXPECT_EQ(g_transport->startedOptions.keepaliveInterval, 15s);
    ASSERT_FALSE(g_transport->startedOptions.headers.empty());
    EXPECT_EQ(g_transport->startedOptions.headers.back().name, "Sec-WebSocket-Protocol");
    EXPECT_EQ(g_transport->startedOptions.headers.back().value, "chat, events");
}

TEST_F(WebSocketTest, RejectsUnofferedSelectedProtocol) {
    Connection connection = connect_fake({
        .url = "wss://example.test/socket",
        .protocols = {"chat"},
    });
    ASSERT_NE(g_transport, nullptr);
    g_transport->sink->opened("events", {{"Sec-WebSocket-Protocol", "events"}});
    EXPECT_EQ(connection.state(), Connection::State::Closing);
    EXPECT_EQ(g_transport->closeCalls, 1);
    EXPECT_EQ(g_transport->closeCode, 1002);
    g_transport->sink->closed(Error::None, {}, 0, 1002, {});

    Event event;
    ASSERT_TRUE(connection.poll(event));
    EXPECT_EQ(event.kind, Event::Kind::Closed);
    EXPECT_EQ(event.error, Error::Protocol);
    EXPECT_EQ(event.message, "Server selected an unoffered WebSocket protocol");
    EXPECT_FALSE(connection.poll(event));
}

TEST_F(WebSocketTest, RejectsUnsolicitedSelectedProtocol) {
    Connection connection = connect_fake({
        .url = "wss://example.test/socket",
        .closeTimeout = 20ms,
    });
    ASSERT_NE(g_transport, nullptr);
    g_transport->sink->opened("chat", {{"Sec-WebSocket-Protocol", "chat"}});
    EXPECT_EQ(connection.state(), Connection::State::Closing);
    EXPECT_EQ(g_transport->closeCalls, 1);
    EXPECT_EQ(g_transport->closeCode, 1002);

    Event event;
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (!connection.poll(event) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_EQ(event.kind, Event::Kind::Closed);
    EXPECT_EQ(event.error, Error::Protocol);
    EXPECT_EQ(event.message, "Server selected an unoffered WebSocket protocol");
}

TEST_F(WebSocketTest, CloseIsValidatedAndIdempotent) {
    Connection connection = connect_fake({.url = "wss://example.test/socket"});
    g_transport->sink->opened({}, {});
    EXPECT_EQ(connection.close(1002, "reserved"), CloseResult::InvalidCode);
    std::string longReason;
    longReason.assign(124, 'x');
    EXPECT_EQ(connection.close(1000, longReason), CloseResult::ReasonTooLarge);
    EXPECT_EQ(connection.close(1000, std::string{"\xff", 1}), CloseResult::InvalidText);
    EXPECT_EQ(connection.state(), Connection::State::Open);
    EXPECT_EQ(g_transport->closeCalls, 0);

    EXPECT_EQ(connection.close(3000, "done"), CloseResult::Ok);
    EXPECT_EQ(connection.close(3001, "ignored"), CloseResult::Ok);
    EXPECT_EQ(connection.state(), Connection::State::Closing);
    EXPECT_EQ(g_transport->closeCalls, 1);
    EXPECT_EQ(g_transport->closeCode, 3000);
    EXPECT_EQ(g_transport->closeReason, "done");
}

TEST_F(WebSocketTest, CloseBeforeOpenIsCanceled) {
    Connection connection = connect_fake({.url = "wss://example.test/socket"});
    EXPECT_EQ(connection.close(), CloseResult::Ok);
    EXPECT_EQ(connection.state(), Connection::State::Closed);
    EXPECT_EQ(g_abortCalls.load(std::memory_order_relaxed), 1);
    Event event;
    ASSERT_TRUE(connection.poll(event));
    EXPECT_EQ(event.kind, Event::Kind::Closed);
    EXPECT_EQ(event.error, Error::Canceled);
}

TEST_F(WebSocketTest, RejectsPlaintextUnlessEnabled) {
    Connection refused = connect_fake({.url = "ws://example.test/socket"});
    EXPECT_EQ(g_transport, nullptr);
    Event event;
    ASSERT_TRUE(refused.poll(event));
    EXPECT_EQ(event.error, Error::UnsupportedScheme);

    Connection allowed = connect_fake({
        .url = "ws://localhost:1234/socket",
        .allowPlaintext = true,
    });
    EXPECT_NE(g_transport, nullptr);
}

TEST_F(WebSocketTest, RejectsMalformedUrlsBeforeStartingTransport) {
    constexpr std::string_view invalid[]{
        "wss://",
        "wss://:443/socket",
        "wss://example.test:not-a-port/socket",
        "wss://example.test:70000/socket",
        "wss://user@example.test/socket",
        "wss://example.test/socket#fragment",
        "wss://::1/socket",
    };
    for (const auto url : invalid) {
        g_transport = nullptr;
        Connection connection = connect_fake({.url = std::string{url}});
        Event event;
        ASSERT_TRUE(connection.poll(event)) << url;
        EXPECT_EQ(event.error, Error::InvalidUrl) << url;
        EXPECT_EQ(g_transport, nullptr) << url;
    }
}

TEST_F(WebSocketTest, SurfacesHandshakeStatusAndHeaders) {
    Connection connection = connect_fake({.url = "wss://example.test/socket"});
    g_transport->sink->closed(
        Error::Handshake, "unauthorized", 401, 0, {}, {{"WWW-Authenticate", "Bearer"}});
    Event event;
    ASSERT_TRUE(connection.poll(event));
    EXPECT_EQ(event.kind, Event::Kind::Closed);
    EXPECT_EQ(event.error, Error::Handshake);
    EXPECT_EQ(event.status, 401);
    ASSERT_EQ(event.headers.size(), 1u);
    EXPECT_EQ(event.headers[0].name, "WWW-Authenticate");
    EXPECT_EQ(event.headers[0].value, "Bearer");
}

TEST_F(WebSocketTest, DroppingConnectionAttemptsGoingAwayClose) {
    {
        Connection connection = connect_fake({.url = "wss://example.test/socket"});
        g_transport->sink->opened({}, {});
    }
    EXPECT_EQ(g_closeCalls, 1);
    EXPECT_EQ(g_lastCloseCode, 1001);
}

TEST_F(WebSocketTest, ShutdownDiscardsLiveConnectionsAndAllowsRestart) {
    Connection connection = connect_fake({.url = "wss://example.test/socket"});
    g_transport->sink->opened({}, {});
    borealis::shutdown();
    EXPECT_EQ(connection.state(), Connection::State::Closed);
    EXPECT_EQ(g_closeCalls, 1);
    EXPECT_EQ(g_lastCloseCode, 1001);
    EXPECT_EQ(g_abortCalls.load(std::memory_order_relaxed), 1);
    Event event;
    EXPECT_FALSE(connection.poll(event));

    g_transport = nullptr;
    Connection restarted = connect_fake({.url = "wss://example.test/socket"});
    EXPECT_NE(g_transport, nullptr);
    EXPECT_EQ(restarted.state(), Connection::State::Connecting);
}

}  // namespace
}  // namespace borealis::ws
