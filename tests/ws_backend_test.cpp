#include "borealis/ws.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace borealis::ws {
namespace {

using namespace std::chrono_literals;

bool wait_event(Connection& connection, Event& event, std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (connection.poll(event)) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

std::optional<std::string> test_url(std::string_view path) {
    const char* configured = std::getenv("BOREALIS_WS_TEST_URL");
    if (configured == nullptr || *configured == '\0') {
        return std::nullopt;
    }
    const std::string_view url{configured};
    const size_t authority = url.find('/', url.find("://") + 3);
    return std::string{url.substr(0, authority)} + std::string{path};
}

Options test_options(const std::string& url) {
    return {
        .url = url,
        .connectTimeout = 2s,
        .closeTimeout = 2s,
        .allowPlaintext = url.starts_with("ws://"),
    };
}

TEST(WebSocketBackendTest, EchoesTextAndBinary) {
    const auto url = test_url("/echo");
    if (!url) {
        GTEST_SKIP() << "Set BOREALIS_WS_TEST_URL to run the backend integration test";
    }
    Connection connection = connect(test_options(*url));
    ASSERT_TRUE(connection);

    Event event;
    ASSERT_TRUE(wait_event(connection, event));
    ASSERT_EQ(event.kind, Event::Kind::Open) << event.message;

    const std::string text{"backend text echo"};
    const std::string binary{"\x00\x7f\x80\xff", 4};
    ASSERT_EQ(connection.send(MessageKind::Text, text), SendResult::Ok);
    ASSERT_EQ(connection.send(MessageKind::Binary, binary), SendResult::Ok);

    bool receivedText = false;
    bool receivedBinary = false;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while ((!receivedText || !receivedBinary) && std::chrono::steady_clock::now() < deadline) {
        if (!wait_event(connection, event, 100ms)) {
            continue;
        }
        ASSERT_NE(event.kind, Event::Kind::Closed) << event.message;
        if (event.kind == Event::Kind::Message && event.messageKind == MessageKind::Text) {
            receivedText = event.data == text;
        } else if (event.kind == Event::Kind::Message && event.messageKind == MessageKind::Binary) {
            receivedBinary = event.data == binary;
        }
    }
    EXPECT_TRUE(receivedText);
    EXPECT_TRUE(receivedBinary);

    connection.close(1000, "integration complete");
    ASSERT_TRUE(wait_event(connection, event));
    EXPECT_EQ(event.kind, Event::Kind::Closed);
    EXPECT_EQ(event.error, Error::None) << event.message;
    EXPECT_EQ(event.code, 1000);
}

TEST(WebSocketBackendTest, SurvivesIdlePastConnectTimeout) {
    const auto url = test_url("/echo");
    if (!url) {
        GTEST_SKIP() << "Set BOREALIS_WS_TEST_URL to run the backend integration test";
    }
    Options options = test_options(*url);
    options.connectTimeout = 500ms;
    Connection connection = connect(std::move(options));
    ASSERT_TRUE(connection);

    Event event;
    ASSERT_TRUE(wait_event(connection, event));
    ASSERT_EQ(event.kind, Event::Kind::Open) << event.message;
    std::this_thread::sleep_for(750ms);
    while (connection.poll(event)) {
        ASSERT_NE(event.kind, Event::Kind::Closed) << event.message;
    }
    ASSERT_EQ(connection.state(), Connection::State::Open);

    constexpr std::string_view Message{"echo after idle"};
    ASSERT_EQ(connection.send(MessageKind::Text, Message), SendResult::Ok);
    ASSERT_TRUE(wait_event(connection, event));
    ASSERT_EQ(event.kind, Event::Kind::Message) << event.message;
    EXPECT_EQ(event.data, Message);

    ASSERT_EQ(connection.close(1000, "idle integration complete"), CloseResult::Ok);
    ASSERT_TRUE(wait_event(connection, event));
    EXPECT_EQ(event.kind, Event::Kind::Closed);
    EXPECT_EQ(event.error, Error::None) << event.message;
}

TEST(WebSocketBackendTest, ReportsRejectedUpgradeHeaders) {
    const auto url = test_url("/reject");
    if (!url) {
        GTEST_SKIP() << "Set BOREALIS_WS_TEST_URL to run the backend integration test";
    }
    Connection connection = connect(test_options(*url));
    Event event;
    ASSERT_TRUE(wait_event(connection, event));
    ASSERT_EQ(event.kind, Event::Kind::Closed);
    EXPECT_EQ(event.error, Error::Handshake) << event.message;
    EXPECT_EQ(event.status, 401);
    EXPECT_TRUE(std::ranges::any_of(event.headers, [](const http::Header& header) {
        std::string name = header.name;
        std::ranges::transform(name, name.begin(),
            [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        return name == "x-borealis-test" && header.value == "rejected";
    }));
}

TEST(WebSocketBackendTest, ReassemblesFragmentedMessages) {
    const auto url = test_url("/fragment");
    if (!url) {
        GTEST_SKIP() << "Set BOREALIS_WS_TEST_URL to run the backend integration test";
    }
    Connection connection = connect(test_options(*url));
    Event event;
    ASSERT_TRUE(wait_event(connection, event));
    ASSERT_EQ(event.kind, Event::Kind::Open) << event.message;
    ASSERT_TRUE(wait_event(connection, event));
    ASSERT_EQ(event.kind, Event::Kind::Message) << event.message;
    EXPECT_EQ(event.messageKind, MessageKind::Text);
    EXPECT_EQ(event.data, "fragmented");
}

TEST(WebSocketBackendTest, CompletesPeerInitiatedClose) {
    const auto url = test_url("/server-close");
    if (!url) {
        GTEST_SKIP() << "Set BOREALIS_WS_TEST_URL to run the backend integration test";
    }
    Connection connection = connect(test_options(*url));
    Event event;
    ASSERT_TRUE(wait_event(connection, event));
    ASSERT_EQ(event.kind, Event::Kind::Open) << event.message;
    ASSERT_TRUE(wait_event(connection, event));
    ASSERT_EQ(event.kind, Event::Kind::Closed) << event.message;
    EXPECT_EQ(event.error, Error::None) << event.message;
    EXPECT_EQ(event.code, 1000);
    EXPECT_EQ(event.reason, "server close");
}

}  // namespace
}  // namespace borealis::ws
