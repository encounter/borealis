#include "borealis/net.hpp"
#include "borealis/task.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <ranges>
#include <string>
#include <thread>
#include <vector>

namespace borealis::net {
namespace {

using namespace std::chrono_literals;

std::vector<Event> wait_for_events(
    Context& context, size_t count, std::chrono::milliseconds timeout = 2s) {
    std::vector<Event> result;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (result.size() < count && std::chrono::steady_clock::now() < deadline) {
        Event event;
        if (context.poll(event)) {
            if (!event.data.empty()) {
                event.message.assign(
                    reinterpret_cast<const char*>(event.data.data()), event.data.size());
                event.data = {};
            }
            result.emplace_back(std::move(event));
        } else {
            std::this_thread::sleep_for(1ms);
        }
    }
    return result;
}

bool wait_for_received(
    Context& context, SocketId id, uint64_t bytes, std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto stats = context.stats(id);
        if (stats && stats->bytesReceived >= bytes) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

TEST(NetEndpointTest, ParsesAndCanonicalizesEndpoints) {
    const auto tcp = parse_endpoint("TCP://127.0.0.1:34197");
    ASSERT_TRUE(tcp);
    EXPECT_EQ(tcp->scheme, "tcp");
    EXPECT_EQ(tcp->host, "127.0.0.1");
    EXPECT_EQ(format_endpoint(*tcp), "tcp://127.0.0.1:34197");

    const auto ipv6 = parse_endpoint("udp://[0:0:0:0:0:0:0:1]:0");
    ASSERT_TRUE(ipv6);
    EXPECT_TRUE(ipv6->literal);
    EXPECT_TRUE(ipv6->ipv6);
    EXPECT_EQ(format_endpoint(*ipv6), "udp://[::1]:0");

    const auto mapped = parse_endpoint("tcp://[::ffff:203.0.113.10]:80");
    ASSERT_TRUE(mapped);
    EXPECT_EQ(format_endpoint(*mapped), "tcp://203.0.113.10:80");

    EXPECT_FALSE(parse_endpoint("tcp://::1:80"));
    EXPECT_FALSE(parse_endpoint("ws://localhost:80"));
    EXPECT_FALSE(parse_endpoint("tcp://localhost:65536"));
    EXPECT_FALSE(parse_endpoint("tcp://localhost:80/"));
}

TEST(NetContextTest, TcpRoundTripAndGracefulClose) {
    Context context;
    const auto listener = context.listen("tcp://127.0.0.1:0");
    ASSERT_NE(listener.id, 0u) << listener.message;
    const SocketId client = context.connect(listener.localEndpoint);
    ASSERT_NE(client, 0u);

    SocketId server = 0;
    bool connected = false;
    const auto opening = wait_for_events(context, 2);
    ASSERT_EQ(opening.size(), 2u);
    for (const auto& event : opening) {
        if (event.kind == Event::Kind::Accepted) {
            server = event.accepted;
        } else if (event.kind == Event::Kind::Connected) {
            connected = true;
        }
    }
    ASSERT_NE(server, 0u);
    ASSERT_TRUE(connected);

    EXPECT_EQ(context.send(client, {}), SendResult::Ok);
    ASSERT_TRUE(context.stats(client));
    EXPECT_EQ(context.stats(client)->queuedSendBytes, 0u);

    const std::string request = "hello";
    ASSERT_EQ(context.send(client, std::as_bytes(std::span{request})), SendResult::Ok);
    const auto requestEvents = wait_for_events(context, 1);
    ASSERT_EQ(requestEvents.size(), 1u);
    EXPECT_EQ(requestEvents[0].kind, Event::Kind::StreamData);
    EXPECT_EQ(requestEvents[0].id, server);
    EXPECT_EQ(requestEvents[0].message, request);

    const std::string response = "world";
    ASSERT_EQ(context.send(server, std::as_bytes(std::span{response})), SendResult::Ok);
    const auto responseEvents = wait_for_events(context, 1);
    ASSERT_EQ(responseEvents.size(), 1u);
    EXPECT_EQ(responseEvents[0].kind, Event::Kind::StreamData);
    EXPECT_EQ(responseEvents[0].id, client);
    EXPECT_EQ(responseEvents[0].message, response);

    context.close(client);
    context.close(server);
    const auto closed = wait_for_events(context, 2);
    ASSERT_EQ(closed.size(), 2u);
    EXPECT_EQ(closed[0].kind, Event::Kind::Closed);
    EXPECT_EQ(closed[1].kind, Event::Kind::Closed);
    EXPECT_EQ(closed[0].error, Error::None);
    EXPECT_EQ(closed[1].error, Error::None);
    EXPECT_FALSE(context.stats(client));
    EXPECT_FALSE(context.stats(server));

    context.close(listener.id);
    const auto listenerClosed = wait_for_events(context, 1);
    ASSERT_EQ(listenerClosed.size(), 1u);
    EXPECT_EQ(listenerClosed[0].id, listener.id);
}

TEST(NetContextTest, PeerHalfCloseDrainsAcceptedSends) {
    Context serverContext;
    Context clientContext{{.maxQueuedBytes = 4096, .maxQueuedEvents = 1}};
    ListenOptions listenOptions;
    listenOptions.accepted.maxSendQueueBytes = 8 * 1024 * 1024;
    const auto listener = serverContext.listen("tcp://127.0.0.1:0", listenOptions);
    ASSERT_NE(listener.id, 0u) << listener.message;
    StreamOptions clientOptions;
    clientOptions.closeTimeout = 10s;
    const SocketId client = clientContext.connect(listener.localEndpoint, clientOptions);
    ASSERT_NE(client, 0u);

    const auto clientOpen = wait_for_events(clientContext, 1);
    ASSERT_EQ(clientOpen.size(), 1u);
    ASSERT_EQ(clientOpen[0].kind, Event::Kind::Connected);
    const auto serverOpen = wait_for_events(serverContext, 1);
    ASSERT_EQ(serverOpen.size(), 1u);
    ASSERT_EQ(serverOpen[0].kind, Event::Kind::Accepted);
    const SocketId server = serverOpen[0].accepted;

    std::vector<std::byte> payload;
    payload.assign(4 * 1024 * 1024, std::byte{0x5a});
    ASSERT_EQ(serverContext.send(server, payload), SendResult::Ok);
    clientContext.close(client);

    // Let the client's bounded event queue apply backpressure before it starts draining.
    std::this_thread::sleep_for(100ms);

    size_t received = 0;
    bool serverClosed = false;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while ((!serverClosed || received != payload.size()) &&
           std::chrono::steady_clock::now() < deadline)
    {
        Event event;
        while (clientContext.poll(event)) {
            if (event.kind == Event::Kind::StreamData) {
                received += event.data.size();
            }
        }
        while (serverContext.poll(event)) {
            if (event.kind == Event::Kind::Closed && event.id == server) {
                serverClosed = true;
                EXPECT_EQ(event.error, Error::None);
            }
        }
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_EQ(received, payload.size());
    EXPECT_TRUE(serverClosed);
}

TEST(NetContextTest, DatagramRoundTripReportsSource) {
    Context context;
    const auto first = context.open_datagram("udp://127.0.0.1:0");
    const auto second = context.open_datagram("udp://127.0.0.1:0");
    ASSERT_NE(first.id, 0u) << first.message;
    ASSERT_NE(second.id, 0u) << second.message;

    const std::string payload = "datagram";
    ASSERT_EQ(context.send_to(first.id, second.localEndpoint, std::as_bytes(std::span{payload})),
        SendResult::Ok);
    const auto events = wait_for_events(context, 1);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].kind, Event::Kind::Datagram);
    EXPECT_EQ(events[0].id, second.id);
    EXPECT_EQ(events[0].endpoint, first.localEndpoint);
    EXPECT_EQ(events[0].message, payload);

    ASSERT_EQ(context.send_to(first.id, second.localEndpoint, {}), SendResult::Ok);
    const auto empty = wait_for_events(context, 1);
    ASSERT_EQ(empty.size(), 1u);
    EXPECT_EQ(empty[0].kind, Event::Kind::Datagram);
    EXPECT_EQ(empty[0].id, second.id);
    EXPECT_TRUE(empty[0].message.empty());
}

TEST(NetContextTest, ResolvePreservesSchemeAndPort) {
    Context context;
    int marker = 0;
    const SocketId resolver = context.resolve("tcp://localhost:16834", &marker);
    ASSERT_NE(resolver, 0u);
    const auto events = wait_for_events(context, 1);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].kind, Event::Kind::Resolved);
    EXPECT_EQ(events[0].error, Error::None) << events[0].message;
    EXPECT_EQ(events[0].userData, &marker);
    const auto endpoint = parse_endpoint(events[0].endpoint);
    ASSERT_TRUE(endpoint);
    EXPECT_EQ(endpoint->scheme, "tcp");
    EXPECT_EQ(endpoint->port, 16834);
    EXPECT_TRUE(endpoint->literal);
}

TEST(NetContextTest, ResolveFailureAndCancellationAreTerminal) {
    Context context;
    const SocketId invalid = context.resolve("tcp://host.invalid:1234");
    ASSERT_NE(invalid, 0u);
    const auto failed = wait_for_events(context, 1);
    ASSERT_EQ(failed.size(), 1u);
    EXPECT_EQ(failed[0].kind, Event::Kind::Resolved);
    EXPECT_EQ(failed[0].error, Error::Resolve);

    const SocketId canceled = context.resolve("udp://localhost:1234");
    ASSERT_NE(canceled, 0u);
    context.close(canceled);
    const auto result = wait_for_events(context, 1);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].kind, Event::Kind::Resolved);
    EXPECT_TRUE(result[0].error == Error::Canceled || result[0].error == Error::None);
}

TEST(NetContextTest, SupportsIpv6LoopbackWhenAvailable) {
    Context context;
    const auto listener = context.listen("tcp://[::1]:0");
    if (listener.id == 0) {
        GTEST_SKIP() << listener.message;
    }
    const SocketId client = context.connect(listener.localEndpoint);
    ASSERT_NE(client, 0u);
    const auto events = wait_for_events(context, 2);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_TRUE(std::ranges::any_of(
        events, [](const Event& event) { return event.kind == Event::Kind::Connected; }));
    EXPECT_TRUE(std::ranges::any_of(
        events, [](const Event& event) { return event.kind == Event::Kind::Accepted; }));
}

TEST(NetContextTest, RefusedConnectClosesOnce) {
    Context context;
    const auto unused = context.listen("tcp://127.0.0.1:0");
    ASSERT_NE(unused.id, 0u);
    const std::string endpoint = unused.localEndpoint;
    context.close(unused.id);
    ASSERT_EQ(wait_for_events(context, 1).size(), 1u);

    const SocketId stream = context.connect(endpoint);
    ASSERT_NE(stream, 0u);
    const auto events = wait_for_events(context, 1);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].kind, Event::Kind::Closed);
    EXPECT_EQ(events[0].id, stream);
    EXPECT_EQ(events[0].error, Error::Refused) << events[0].message;
    EXPECT_TRUE(wait_for_events(context, 1, 50ms).empty());
}

TEST(NetContextTest, ShutdownFencesAndRestarts) {
    Context context;
    const auto socket = context.open_datagram("udp://127.0.0.1:0");
    ASSERT_NE(socket.id, 0u);
    borealis::shutdown();
    Event event;
    ASSERT_TRUE(context.poll(event));
    EXPECT_EQ(event.kind, Event::Kind::Closed);
    EXPECT_EQ(event.id, socket.id);
    EXPECT_EQ(event.error, Error::Canceled);

    const auto restarted = context.open_datagram("udp://127.0.0.1:0");
    EXPECT_NE(restarted.id, 0u) << restarted.message;
}

TEST(NetContextTest, EnforcesAdmissionLimits) {
    Context context{{
        .maxStreams = 1,
        .maxListeners = 1,
        .maxDatagramSockets = 1,
        .maxResolvers = 1,
    }};

    const auto listener = context.listen("tcp://127.0.0.1:0");
    ASSERT_NE(listener.id, 0u);
    EXPECT_EQ(context.listen("tcp://127.0.0.1:0").id, 0u);

    const auto datagram = context.open_datagram("udp://127.0.0.1:0");
    ASSERT_NE(datagram.id, 0u);
    EXPECT_EQ(context.open_datagram("udp://127.0.0.1:0").id, 0u);

    const SocketId resolver = context.resolve("tcp://localhost:1");
    ASSERT_NE(resolver, 0u);
    EXPECT_EQ(context.resolve("tcp://localhost:2"), 0u);

    const SocketId stream = context.connect(listener.localEndpoint);
    ASSERT_NE(stream, 0u);
    EXPECT_EQ(context.connect(listener.localEndpoint), 0u);
}

TEST(NetContextTest, SamplesUserDataWhenEventIsPolled) {
    Context context;
    int listenerMarker = 1;
    int streamMarker = 2;
    const auto listener = context.listen("tcp://127.0.0.1:0", {}, &listenerMarker);
    ASSERT_NE(listener.id, 0u);
    const SocketId client = context.connect(listener.localEndpoint);
    ASSERT_NE(client, 0u);

    SocketId server = 0;
    for (const auto& event : wait_for_events(context, 2)) {
        if (event.kind == Event::Kind::Accepted) {
            server = event.accepted;
            EXPECT_EQ(event.userData, &listenerMarker);
        }
    }
    ASSERT_NE(server, 0u);

    const std::string payload = "queued";
    ASSERT_EQ(context.send(client, std::as_bytes(std::span{payload})), SendResult::Ok);
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (context.stats(server)->bytesReceived == 0 && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(1ms);
    }
    context.set_user_data(server, &streamMarker);
    const auto events = wait_for_events(context, 1);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].kind, Event::Kind::StreamData);
    EXPECT_EQ(events[0].userData, &streamMarker);
    EXPECT_EQ(events[0].message, payload);
}

TEST(NetContextTest, ReportsDatagramOverflowAfterQueueDrains) {
    Context context{{.maxQueuedBytes = 4, .maxQueuedEvents = 1}};
    const auto source = context.open_datagram("udp://127.0.0.1:0");
    const auto target = context.open_datagram("udp://127.0.0.1:0");
    ASSERT_NE(source.id, 0u);
    ASSERT_NE(target.id, 0u);
    const std::string payload = "data";
    for (int index = 0; index < 8; ++index) {
        ASSERT_EQ(
            context.send_to(source.id, target.localEndpoint, std::as_bytes(std::span{payload})),
            SendResult::Ok);
    }

    const auto first = wait_for_events(context, 1);
    ASSERT_EQ(first.size(), 1u);
    EXPECT_EQ(first[0].kind, Event::Kind::Datagram);
    Event dropped;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (dropped.kind != Event::Kind::Dropped && std::chrono::steady_clock::now() < deadline) {
        const auto events = wait_for_events(context, 1, 100ms);
        if (!events.empty()) {
            dropped = events.front();
        }
    }
    ASSERT_EQ(dropped.kind, Event::Kind::Dropped);
    EXPECT_GT(dropped.dropped, 0u);
    const auto stats = context.stats(target.id);
    ASSERT_TRUE(stats);
    EXPECT_GE(stats->inboundDropped, dropped.dropped);
}

TEST(NetContextTest, CoalescesDroppedMarkersWhileConsumerIsStalled) {
    Context context{{.maxQueuedBytes = 1, .maxQueuedEvents = 0}};
    const auto source = context.open_datagram("udp://127.0.0.1:0");
    const auto target = context.open_datagram("udp://127.0.0.1:0");
    ASSERT_NE(source.id, 0u);
    ASSERT_NE(target.id, 0u);
    const std::string payload = "oversized";
    for (int index = 0; index < 256; ++index) {
        ASSERT_EQ(
            context.send_to(source.id, target.localEndpoint, std::as_bytes(std::span{payload})),
            SendResult::Ok);
    }

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto stats = context.stats(target.id);
        if (stats && stats->inboundDropped >= 64) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    const auto stats = context.stats(target.id);
    ASSERT_TRUE(stats);
    ASSERT_GE(stats->inboundDropped, 64u);
    context.close(target.id);

    size_t droppedMarkers = 0;
    bool sawClosed = false;
    while (!sawClosed && std::chrono::steady_clock::now() < deadline + 2s) {
        for (const auto& event : wait_for_events(context, 1, 100ms)) {
            if (event.id != target.id) {
                continue;
            }
            droppedMarkers += event.kind == Event::Kind::Dropped ? 1u : 0u;
            sawClosed = event.kind == Event::Kind::Closed;
        }
    }
    EXPECT_TRUE(sawClosed);
    EXPECT_LE(droppedMarkers, 1u);
}

TEST(NetContextTest, StreamBackpressurePausesAndResumes) {
    Context context{{.maxQueuedBytes = 4, .maxQueuedEvents = 0}};
    ListenOptions options;
    options.accepted.readChunkBytes = 4;
    options.accepted.inboundStallTimeout = 2s;
    const auto listener = context.listen("tcp://127.0.0.1:0", options);
    ASSERT_NE(listener.id, 0u);
    const SocketId client = context.connect(listener.localEndpoint);
    ASSERT_NE(client, 0u);
    SocketId server = 0;
    for (const auto& event : wait_for_events(context, 2)) {
        if (event.kind == Event::Kind::Accepted) {
            server = event.accepted;
        }
    }
    ASSERT_NE(server, 0u);

    const std::string payload = "12345678";
    ASSERT_EQ(context.send(client, std::as_bytes(std::span{payload})), SendResult::Ok);
    ASSERT_TRUE(wait_for_received(context, server, 4));
    const auto first = wait_for_events(context, 1);
    ASSERT_EQ(first.size(), 1u);
    EXPECT_EQ(first[0].kind, Event::Kind::StreamData);
    EXPECT_EQ(first[0].message, "1234");
    const auto second = wait_for_events(context, 1);
    ASSERT_EQ(second.size(), 1u);
    EXPECT_EQ(second[0].kind, Event::Kind::StreamData);
    EXPECT_EQ(second[0].message, "5678");
}

TEST(NetContextTest, StalledInboundStreamClosesWithTooLarge) {
    Context context{{.maxQueuedBytes = 4, .maxQueuedEvents = 0}};
    ListenOptions options;
    options.accepted.readChunkBytes = 4;
    options.accepted.inboundStallTimeout = 40ms;
    const auto listener = context.listen("tcp://127.0.0.1:0", options);
    ASSERT_NE(listener.id, 0u);
    const SocketId client = context.connect(listener.localEndpoint);
    ASSERT_NE(client, 0u);
    SocketId server = 0;
    for (const auto& event : wait_for_events(context, 2)) {
        if (event.kind == Event::Kind::Accepted) {
            server = event.accepted;
        }
    }
    ASSERT_NE(server, 0u);

    const std::string payload = "12345678";
    ASSERT_EQ(context.send(client, std::as_bytes(std::span{payload})), SendResult::Ok);
    ASSERT_TRUE(wait_for_received(context, server, 4));
    std::this_thread::sleep_for(100ms);
    const auto events = wait_for_events(context, 2);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].kind, Event::Kind::StreamData);
    EXPECT_EQ(events[1].kind, Event::Kind::Closed);
    EXPECT_EQ(events[1].id, server);
    EXPECT_EQ(events[1].error, Error::TooLarge);
}

TEST(NetContextTest, TerminalEventsBypassPayloadLimit) {
    Context context{{.maxQueuedBytes = 4, .maxQueuedEvents = 1}};
    const auto source = context.open_datagram("udp://127.0.0.1:0");
    const auto target = context.open_datagram("udp://127.0.0.1:0");
    ASSERT_NE(source.id, 0u);
    ASSERT_NE(target.id, 0u);
    const std::string payload = "data";
    ASSERT_EQ(context.send_to(source.id, target.localEndpoint, std::as_bytes(std::span{payload})),
        SendResult::Ok);

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (
        context.stats(target.id)->bytesReceived == 0 && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(1ms);
    }
    context.close(target.id);
    const auto events = wait_for_events(context, 2);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].kind, Event::Kind::Datagram);
    EXPECT_EQ(events[1].kind, Event::Kind::Closed);
    EXPECT_EQ(events[1].id, target.id);
}

TEST(NetContextTest, RejectsHostnamesWhereLiteralIsRequired) {
    Context context;
    EXPECT_EQ(context.send(0, {}), SendResult::NotOpen);
    EXPECT_EQ(context.send_to(0, "udp://127.0.0.1:1", {}), SendResult::NotOpen);
    EXPECT_EQ(context.listen("tcp://localhost:0").error, Error::InvalidEndpoint);
    EXPECT_EQ(context.open_datagram("udp://localhost:0").error, Error::InvalidEndpoint);
    const auto socket = context.open_datagram("udp://127.0.0.1:0");
    ASSERT_NE(socket.id, 0u);
    const std::string payload = "x";
    EXPECT_EQ(context.send_to(socket.id, "udp://localhost:1", std::as_bytes(std::span{payload})),
        SendResult::InvalidEndpoint);
    const std::vector<std::byte> oversized(65508);
    EXPECT_EQ(context.send_to(socket.id, "udp://127.0.0.1:1", oversized), SendResult::TooLarge);
}

}  // namespace
}  // namespace borealis::net
