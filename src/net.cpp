#include "borealis/net.hpp"

#include "ascii.hpp"
#include "borealis/log.hpp"
#include "borealis/task.hpp"
#include "borealis/url.hpp"
#include "net/platform.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
constexpr borealis::Log Log{"borealis::net"};
}

namespace borealis::net {
namespace {
using Clock = std::chrono::steady_clock;
using detail::platform::close_socket;
using detail::platform::configure_common_socket;
using detail::platform::connect_in_progress;
using detail::platform::interrupted;
using detail::platform::InvalidSocket;
using detail::platform::last_socket_error;
using detail::platform::map_socket_error;
using detail::platform::NativeSocket;
using detail::platform::PollDescriptor;
using detail::platform::prepare_connect;
using detail::platform::set_nonblocking;
using detail::platform::set_socket_option;
using detail::platform::socket_error_message;
using detail::platform::SockLength;
using detail::platform::would_block;

constexpr size_t MaxReadsPerWake = 16;
constexpr size_t MaxDatagramsPerWake = 16;
constexpr size_t MaxWriteBytesPerWake = 256 * 1024;
constexpr auto MaximumPollInterval = std::chrono::hours{1};
constexpr auto ResolutionPollInterval = std::chrono::milliseconds{25};

struct Address {
    sockaddr_storage storage{};
    SockLength length = 0;
};

struct Resolution {
    std::vector<Address> addresses;
    int error = 0;
    std::string message;
};

Resolution resolve_addresses(const ParsedEndpoint& endpoint, bool passive);

struct ResolutionJob {
    std::mutex mutex;
    std::optional<Resolution> result;
    std::atomic_bool canceled = false;
};

class ResolverExecutor {
public:
    ResolverExecutor() : state{std::make_shared<State>()} {
        try {
            for (size_t index = 0; index < WorkerCount; ++index) {
                workers.emplace_back([shared = state] { worker_main(shared); });
            }
        } catch (...) {
            {
                std::lock_guard lock{state->mutex};
                state->stopping = true;
            }
            state->ready.notify_all();
            for (auto& worker : workers) {
                if (worker.joinable()) {
                    worker.join();
                }
            }
            throw;
        }
    }

    std::shared_ptr<ResolutionJob> submit(ParsedEndpoint endpoint) {
        auto job = std::make_shared<ResolutionJob>();
        {
            std::lock_guard lock{state->mutex};
            if (state->stopping) {
                return nullptr;
            }
            state->queue.push_back({std::move(endpoint), job});
        }
        state->ready.notify_one();
        return job;
    }

private:
    static constexpr size_t WorkerCount = 4;

    struct Work {
        ParsedEndpoint endpoint;
        std::shared_ptr<ResolutionJob> job;
    };

    struct State {
        std::mutex mutex;
        std::condition_variable ready;
        std::deque<Work> queue;
        bool stopping = false;
    };

    static void worker_main(const std::shared_ptr<State>& state) noexcept try {
        for (;;) {
            Work work;
            {
                std::unique_lock lock{state->mutex};
                state->ready.wait(lock, [&] { return state->stopping || !state->queue.empty(); });
                if (state->stopping) {
                    return;
                }
                work = std::move(state->queue.front());
                state->queue.pop_front();
            }
            if (work.job->canceled.load(std::memory_order_acquire)) {
                continue;
            }
            Resolution result;
            if (!detail::platform::initialize()) {
                result = {.error = -1, .message = "Network resolver could not initialize"};
            } else {
                result = resolve_addresses(work.endpoint, false);
                detail::platform::cleanup();
            }
            if (!work.job->canceled.load(std::memory_order_acquire)) {
                std::lock_guard lock{work.job->mutex};
                work.job->result.emplace(std::move(result));
            }
        }
    }
    BOREALIS_CATCH()

    std::shared_ptr<State> state;
    std::vector<std::thread> workers;
};

std::shared_ptr<ResolutionJob> start_resolution(ParsedEndpoint endpoint) {
    // System resolver calls cannot be canceled portably. A process-lifetime pool bounds
    // concurrency without making network or task-pool shutdown wait for a stuck lookup.
    static auto* executor = new ResolverExecutor{};
    return executor->submit(std::move(endpoint));
}

bool is_ipv4_mapped(const in6_addr& address) {
    static constexpr std::array<unsigned char, 12> Prefix{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    return std::equal(Prefix.begin(), Prefix.end(), address.s6_addr);
}

std::string address_endpoint(const sockaddr* address, SockLength length, std::string_view scheme) {
    if (address->sa_family == AF_INET6) {
        const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(address);
        if (is_ipv4_mapped(ipv6->sin6_addr)) {
            char host[INET_ADDRSTRLEN]{};
            in_addr ipv4{};
            std::memcpy(&ipv4, &ipv6->sin6_addr.s6_addr[12], sizeof(ipv4));
            if (inet_ntop(AF_INET, &ipv4, host, sizeof(host)) != nullptr) {
                return std::string{scheme} + "://" + host + ":" +
                       std::to_string(ntohs(ipv6->sin6_port));
            }
        }
    }

    char host[NI_MAXHOST]{};
    char service[NI_MAXSERV]{};
    if (getnameinfo(address, length, host, sizeof(host), service, sizeof(service),
            NI_NUMERICHOST | NI_NUMERICSERV) != 0)
    {
        return {};
    }
    if (address->sa_family == AF_INET6) {
        return std::string{scheme} + "://[" + host + "]:" + service;
    }
    return std::string{scheme} + "://" + host + ":" + service;
}

Resolution resolve_addresses(const ParsedEndpoint& endpoint, bool passive) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = endpoint.scheme == "tcp" ? SOCK_STREAM : SOCK_DGRAM;
    hints.ai_protocol = endpoint.scheme == "tcp" ? IPPROTO_TCP : IPPROTO_UDP;
    if (passive) {
        hints.ai_flags |= AI_PASSIVE;
    }
    if (endpoint.literal) {
        hints.ai_flags |= AI_NUMERICHOST;
    }

    addrinfo* head = nullptr;
    const std::string service = std::to_string(endpoint.port);
    const int result = getaddrinfo(endpoint.host.c_str(), service.c_str(), &hints, &head);
    if (result != 0) {
        return {.error = result, .message = detail::platform::resolve_error_message(result)};
    }

    Resolution resolved;
    for (auto* cursor = head; cursor != nullptr; cursor = cursor->ai_next) {
        if (cursor->ai_addrlen > sizeof(sockaddr_storage)) {
            continue;
        }
        Address address;
        std::memcpy(&address.storage, cursor->ai_addr, cursor->ai_addrlen);
        address.length = static_cast<SockLength>(cursor->ai_addrlen);
        resolved.addresses.emplace_back(address);
    }
    freeaddrinfo(head);
    if (resolved.addresses.empty()) {
        resolved.error = -1;
        resolved.message = "Name resolution returned no usable addresses";
    }
    return resolved;
}

std::optional<Address> literal_address(const ParsedEndpoint& endpoint) {
    Address result;
    if (!endpoint.ipv6) {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(endpoint.port);
        if (inet_pton(AF_INET, endpoint.host.c_str(), &address.sin_addr) != 1) {
            return std::nullopt;
        }
        std::memcpy(&result.storage, &address, sizeof(address));
        result.length = sizeof(address);
        return result;
    }
    if (endpoint.host.find('%') != std::string::npos) {
        Resolution resolved = resolve_addresses(endpoint, true);
        return resolved.addresses.empty() ? std::nullopt :
                                            std::optional<Address>{resolved.addresses.front()};
    }
    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    address.sin6_port = htons(endpoint.port);
    if (inet_pton(AF_INET6, endpoint.host.c_str(), &address.sin6_addr) != 1) {
        return std::nullopt;
    }
    std::memcpy(&result.storage, &address, sizeof(address));
    result.length = sizeof(address);
    return result;
}

void configure_stream(NativeSocket socket, const StreamOptions& options) {
    configure_common_socket(socket);
    const int noDelay = options.noDelay ? 1 : 0;
    set_socket_option(socket, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay));
}

struct NativeBindResult {
    NativeSocket socket = InvalidSocket;
    sockaddr_storage local{};
    SockLength localLength = 0;
    Error error = Error::None;
    std::string message;
};

template <typename Configure>
NativeBindResult bind_socket(const ParsedEndpoint& endpoint, int type, int protocol,
    Configure&& configure, std::optional<int> backlog = std::nullopt) {
    const auto address = literal_address(endpoint);
    if (!address) {
        return {.error = Error::InvalidEndpoint, .message = "Invalid bind address"};
    }
    NativeSocket socket = ::socket(address->storage.ss_family, type, protocol);
    if (socket == InvalidSocket) {
        const int error = last_socket_error();
        return {.error = map_socket_error(error), .message = socket_error_message(error)};
    }
    configure(socket);
    if (address->storage.ss_family == AF_INET6) {
        const int dualStack = 0;
        set_socket_option(socket, IPPROTO_IPV6, IPV6_V6ONLY, &dualStack, sizeof(dualStack));
    }
    if (!set_nonblocking(socket) ||
        ::bind(socket, reinterpret_cast<const sockaddr*>(&address->storage), address->length) !=
            0 ||
        (backlog && ::listen(socket, std::max(*backlog, 1)) != 0))
    {
        const int error = last_socket_error();
        close_socket(socket);
        return {.error = map_socket_error(error), .message = socket_error_message(error)};
    }
    NativeBindResult result{.socket = socket, .localLength = sizeof(sockaddr_storage)};
    if (getsockname(socket, reinterpret_cast<sockaddr*>(&result.local), &result.localLength) != 0) {
        const int error = last_socket_error();
        close_socket(socket);
        return {.error = map_socket_error(error), .message = socket_error_message(error)};
    }
    return result;
}

enum class SocketKind {
    Listener,
    Stream,
    Datagram,
    Resolver,
};

enum class SocketPhase {
    Resolving,
    Connecting,
    Open,
    Closing,
    Closed,
};

struct Outbound {
    std::vector<std::byte> data;
    size_t offset = 0;
    std::optional<Address> destination;
};

struct SocketState {
    SocketId id = 0;
    SocketKind kind = SocketKind::Stream;
    SocketPhase phase = SocketPhase::Closed;
    NativeSocket socket = InvalidSocket;
    void* userData = nullptr;
    std::string scheme;
    StreamOptions streamOptions;
    ListenOptions listenOptions;
    DatagramOptions datagramOptions;
    std::deque<Outbound> outbound;
    Stats stats;
    size_t queuedInboundBytes = 0;
    std::shared_ptr<ResolutionJob> resolution;
    std::vector<Address> candidates;
    size_t candidateIndex = 0;
    Clock::time_point createdAt = Clock::now();
    Clock::time_point closeDeadline{};
    std::optional<Clock::time_point> pausedAt;
    bool readClosed = false;
    bool writeShutdown = false;
    uint64_t droppedSinceMarker = 0;
    bool droppedMarkerQueued = false;
};

struct QueuedEvent {
    Event::Kind kind = Event::Kind::Closed;
    SocketId id = 0;
    SocketId accepted = 0;
    std::string endpoint;
    std::vector<std::byte> data;
    uint32_t dropped = 0;
    Error error = Error::None;
    std::string message;
    bool payload = false;
};

struct Runtime {
    explicit Runtime(ContextOptions value) : options{value} {}

    std::mutex mutex;
    std::condition_variable destroyed;
    ContextOptions options;
    std::unordered_map<SocketId, std::shared_ptr<SocketState>> sockets;
    std::deque<QueuedEvent> events;
    std::vector<std::byte> lastPayload;
    std::vector<std::byte> readScratch;
    size_t queuedPayloadBytes = 0;
    size_t queuedPayloadEvents = 0;
    bool registered = false;
    bool destroyRequested = false;
    bool destroyAcknowledged = false;
};

std::atomic<SocketId> g_nextSocketId = 1;

SocketId next_socket_id() {
    SocketId id = g_nextSocketId.fetch_add(1, std::memory_order_relaxed);
    if (id == 0) {
        id = g_nextSocketId.fetch_add(1, std::memory_order_relaxed);
    }
    return id;
}

bool payload_room(const Runtime& runtime, size_t bytes = 1) {
    const bool bytesFit =
        runtime.options.maxQueuedBytes == 0 ||
        bytes <= runtime.options.maxQueuedBytes -
                     std::min(runtime.queuedPayloadBytes, runtime.options.maxQueuedBytes);
    const bool eventFits = runtime.options.maxQueuedEvents == 0 ||
                           runtime.queuedPayloadEvents < runtime.options.maxQueuedEvents;
    return bytesFit && eventFits;
}

void queue_event(Runtime& runtime, QueuedEvent event) {
    if (event.payload) {
        runtime.queuedPayloadBytes += event.data.size();
        ++runtime.queuedPayloadEvents;
    }
    runtime.events.emplace_back(std::move(event));
}

size_t count_kind(const Runtime& runtime, SocketKind kind) {
    return static_cast<size_t>(std::count_if(runtime.sockets.begin(), runtime.sockets.end(),
        [kind](const auto& item) { return item.second->kind == kind; }));
}

size_t stream_count(const Runtime& runtime) {
    return count_kind(runtime, SocketKind::Stream);
}

void close_native(SocketState& state) {
    close_socket(state.socket);
    state.socket = InvalidSocket;
}

void finish_socket(Runtime& runtime, SocketState& state, Error error, std::string message = {}) {
    if (state.phase == SocketPhase::Closed) {
        return;
    }
    close_native(state);
    state.phase = SocketPhase::Closed;
    state.outbound.clear();
    state.stats.queuedSendBytes = 0;
    state.resolution.reset();
    queue_event(runtime, {
                             .kind = Event::Kind::Closed,
                             .id = state.id,
                             .error = error,
                             .message = std::move(message),
                         });
}

void finish_resolver(Runtime& runtime, SocketState& state, Error error, std::string endpoint = {},
    std::string message = {}) {
    if (state.phase == SocketPhase::Closed) {
        return;
    }
    state.phase = SocketPhase::Closed;
    state.resolution.reset();
    queue_event(runtime, {
                             .kind = Event::Kind::Resolved,
                             .id = state.id,
                             .endpoint = std::move(endpoint),
                             .error = error,
                             .message = std::move(message),
                         });
}

class NetworkManager {
public:
    ~NetworkManager() { shutdown(); }

    static void start_stream_close(SocketState& state) { begin_write_shutdown(state); }

    bool add(const std::shared_ptr<Runtime>& runtime) {
        std::unique_lock managerLock{m_mutex};
        m_shutdownComplete.wait(managerLock, [this] { return !m_shuttingDown; });
        if (!ensure_started_locked()) {
            return false;
        }
        {
            std::lock_guard runtimeLock{runtime->mutex};
            if (runtime->registered) {
                wake();
                return true;
            }
            runtime->registered = true;
            runtime->destroyAcknowledged = false;
        }
        m_runtimes.emplace_back(runtime);
        wake();
        return true;
    }

    void wake() noexcept { m_wakeup.wake(); }

    void shutdown() noexcept {
        std::thread thread;
        {
            std::unique_lock lock{m_mutex};
            m_shutdownComplete.wait(lock, [this] { return !m_shuttingDown; });
            if (!m_running) {
                return;
            }
            m_shuttingDown = true;
            m_stopping.store(true, std::memory_order_release);
            thread = std::move(m_thread);
        }
        wake();
        if (thread.joinable()) {
            try {
                thread.join();
            }
            BOREALIS_CATCH_FATAL()
        }

        std::vector<std::shared_ptr<Runtime>> runtimes;
        {
            std::lock_guard lock{m_mutex};
            for (const auto& weak : m_runtimes) {
                if (auto runtime = weak.lock()) {
                    runtimes.emplace_back(std::move(runtime));
                }
            }
            m_runtimes.clear();
            m_running = false;
        }
        for (const auto& runtime : runtimes) {
            std::lock_guard lock{runtime->mutex};
            abort_runtime(*runtime, runtime->destroyRequested);
            runtime->registered = false;
            if (runtime->destroyRequested) {
                runtime->destroyAcknowledged = true;
                runtime->destroyed.notify_all();
            }
        }
        m_wakeup.close();
        detail::platform::cleanup();
        {
            std::lock_guard lock{m_mutex};
            m_shuttingDown = false;
        }
        m_shutdownComplete.notify_all();
    }

private:
    struct ReadySocket {
        std::shared_ptr<Runtime> runtime;
        std::shared_ptr<SocketState> state;
        NativeSocket descriptor = InvalidSocket;
    };

    bool ensure_started_locked() {
        if (m_running) {
            return true;
        }
        if (!detail::platform::initialize()) {
            return false;
        }
        if (!m_wakeup.open()) {
            detail::platform::cleanup();
            return false;
        }
        m_stopping.store(false, std::memory_order_release);
        try {
            m_thread = std::thread{[this] { run(); }};
            m_running = true;
        } catch (const std::system_error& exception) {
            ::Log.error("Could not start network I/O thread: {}", exception.what());
            m_wakeup.close();
            detail::platform::cleanup();
        }
        return m_running;
    }

    std::vector<std::shared_ptr<Runtime>> runtimes() {
        std::vector<std::shared_ptr<Runtime>> result;
        std::lock_guard lock{m_mutex};
        std::erase_if(m_runtimes, [](const auto& runtime) { return runtime.expired(); });
        result.reserve(m_runtimes.size());
        for (const auto& weak : m_runtimes) {
            if (auto runtime = weak.lock()) {
                result.emplace_back(std::move(runtime));
            }
        }
        return result;
    }

    static void abort_runtime(Runtime& runtime, bool discardEvents) {
        for (auto& [id, state] : runtime.sockets) {
            (void)id;
            if (state->resolution) {
                state->resolution->canceled.store(true, std::memory_order_release);
            }
            if (discardEvents) {
                close_native(*state);
            } else if (state->kind == SocketKind::Resolver) {
                finish_resolver(runtime, *state, Error::Canceled, {}, "Networking shut down");
            } else {
                finish_socket(runtime, *state, Error::Canceled, "Networking shut down");
            }
        }
        if (!discardEvents) {
            return;
        }
        runtime.sockets.clear();
        runtime.events.clear();
        runtime.lastPayload.clear();
        runtime.queuedPayloadBytes = 0;
        runtime.queuedPayloadEvents = 0;
    }

    static bool start_next_candidate(Runtime& runtime, SocketState& state, int lastError = 0) {
        while (state.candidateIndex < state.candidates.size()) {
            close_native(state);
            const Address& address = state.candidates[state.candidateIndex++];
            state.socket = socket(address.storage.ss_family, SOCK_STREAM, IPPROTO_TCP);
            if (state.socket == InvalidSocket) {
                lastError = last_socket_error();
                continue;
            }
            configure_stream(state.socket, state.streamOptions);
            if (!set_nonblocking(state.socket)) {
                lastError = last_socket_error();
                continue;
            }
            prepare_connect(state.socket, reinterpret_cast<const sockaddr*>(&address.storage));
            const int result = ::connect(
                state.socket, reinterpret_cast<const sockaddr*>(&address.storage), address.length);
            if (result == 0) {
                state.phase = SocketPhase::Open;
                const std::string endpoint = address_endpoint(
                    reinterpret_cast<const sockaddr*>(&address.storage), address.length, "tcp");
                queue_event(runtime, {
                                         .kind = Event::Kind::Connected,
                                         .id = state.id,
                                         .endpoint = endpoint,
                                     });
                return true;
            }
            lastError = last_socket_error();
            if (connect_in_progress(lastError)) {
                state.phase = SocketPhase::Connecting;
                return true;
            }
        }
        finish_socket(runtime, state, map_socket_error(lastError),
            lastError != 0 ? socket_error_message(lastError) : "Connection failed");
        return false;
    }

    static void process_resolutions(Runtime& runtime, SocketState& state) {
        if (!state.resolution) {
            return;
        }
        Resolution resolution;
        {
            std::lock_guard lock{state.resolution->mutex};
            if (!state.resolution->result) {
                return;
            }
            resolution = std::move(*state.resolution->result);
        }
        state.resolution.reset();
        if (state.phase == SocketPhase::Closed) {
            return;
        }
        if (resolution.error != 0 || resolution.addresses.empty()) {
            if (state.kind == SocketKind::Resolver) {
                finish_resolver(runtime, state, Error::Resolve, {}, resolution.message);
            } else {
                finish_socket(runtime, state, Error::Resolve, resolution.message);
            }
            return;
        }
        if (state.kind == SocketKind::Resolver) {
            const Address& address = resolution.addresses.front();
            finish_resolver(runtime, state, Error::None,
                address_endpoint(reinterpret_cast<const sockaddr*>(&address.storage),
                    address.length, state.scheme));
            return;
        }
        state.candidates = std::move(resolution.addresses);
        state.candidateIndex = 0;
        start_next_candidate(runtime, state);
    }

    static bool listener_can_accept(const Runtime& runtime) {
        return payload_room(runtime) && (runtime.options.maxStreams == 0 ||
                                            stream_count(runtime) < runtime.options.maxStreams);
    }

    static short requested_events(Runtime& runtime, SocketState& state) {
        switch (state.kind) {
        case SocketKind::Listener:
            return state.phase == SocketPhase::Open && listener_can_accept(runtime) ? POLLIN : 0;
        case SocketKind::Stream: {
            if (state.phase == SocketPhase::Connecting) {
                return POLLOUT;
            }
            if (state.phase != SocketPhase::Open && state.phase != SocketPhase::Closing) {
                return 0;
            }
            short events = 0;
            if (!state.readClosed && payload_room(runtime)) {
                events |= POLLIN;
                state.pausedAt.reset();
            } else if (!state.readClosed && state.queuedInboundBytes != 0 && !state.pausedAt) {
                state.pausedAt = Clock::now();
            }
            if (!state.outbound.empty()) {
                events |= POLLOUT;
            }
            return events;
        }
        case SocketKind::Datagram:
            return state.phase == SocketPhase::Open ?
                       static_cast<short>(POLLIN | (!state.outbound.empty() ? POLLOUT : 0)) :
                       0;
        case SocketKind::Resolver:
            return 0;
        }
        return 0;
    }

    static void process_deadlines(Runtime& runtime, SocketState& state, Clock::time_point now) {
        if ((state.phase == SocketPhase::Resolving || state.phase == SocketPhase::Connecting) &&
            state.kind == SocketKind::Stream &&
            now - state.createdAt >= state.streamOptions.connectTimeout)
        {
            if (state.resolution) {
                state.resolution->canceled.store(true, std::memory_order_release);
            }
            finish_socket(runtime, state, Error::Timeout, "Connection timed out");
            return;
        }
        if (state.phase == SocketPhase::Closing && now >= state.closeDeadline) {
            finish_socket(runtime, state, Error::Timeout, "Close timed out");
            return;
        }
        if (state.pausedAt && now - *state.pausedAt >= state.streamOptions.inboundStallTimeout) {
            finish_socket(runtime, state, Error::TooLarge, "Inbound stream remained stalled");
        }
    }

    static std::optional<Clock::time_point> next_deadline(
        const SocketState& state, Clock::time_point now) {
        std::optional<Clock::time_point> result;
        if (state.resolution) {
            result = now + ResolutionPollInterval;
        }
        if (state.kind == SocketKind::Stream &&
            (state.phase == SocketPhase::Resolving || state.phase == SocketPhase::Connecting))
        {
            const auto connectDeadline = state.createdAt + state.streamOptions.connectTimeout;
            if (!result || connectDeadline < *result) {
                result = connectDeadline;
            }
        } else if (state.phase == SocketPhase::Closing) {
            result = state.closeDeadline;
        }
        if (state.pausedAt) {
            const auto stallDeadline = *state.pausedAt + state.streamOptions.inboundStallTimeout;
            if (!result || stallDeadline < *result) {
                result = stallDeadline;
            }
        }
        return result;
    }

    static void process_dropped_marker(Runtime& runtime, SocketState& state) {
        if (state.kind != SocketKind::Datagram || state.phase != SocketPhase::Open ||
            state.droppedMarkerQueued || state.droppedSinceMarker == 0 || !payload_room(runtime))
        {
            return;
        }
        const auto count = static_cast<uint32_t>(
            std::min<uint64_t>(state.droppedSinceMarker, std::numeric_limits<uint32_t>::max()));
        state.droppedSinceMarker -= count;
        state.droppedMarkerQueued = true;
        queue_event(runtime, {
                                 .kind = Event::Kind::Dropped,
                                 .id = state.id,
                                 .dropped = count,
                             });
    }

    static void handle_connect(Runtime& runtime, SocketState& state) {
        const int error = detail::platform::socket_error(state.socket);
        if (error != 0) {
            start_next_candidate(runtime, state, error);
            return;
        }
        sockaddr_storage peer{};
        SockLength peerLength = sizeof(peer);
        state.phase = SocketPhase::Open;
        std::string endpoint;
        if (getpeername(state.socket, reinterpret_cast<sockaddr*>(&peer), &peerLength) == 0) {
            endpoint =
                address_endpoint(reinterpret_cast<const sockaddr*>(&peer), peerLength, "tcp");
        }
        queue_event(runtime, {
                                 .kind = Event::Kind::Connected,
                                 .id = state.id,
                                 .endpoint = std::move(endpoint),
                             });
    }

    static void handle_accept(Runtime& runtime, SocketState& listener) {
        for (size_t acceptedCount = 0;
            acceptedCount < MaxReadsPerWake && listener_can_accept(runtime); ++acceptedCount)
        {
            sockaddr_storage peer{};
            SockLength peerLength = sizeof(peer);
            NativeSocket socket =
                accept(listener.socket, reinterpret_cast<sockaddr*>(&peer), &peerLength);
            if (socket == InvalidSocket) {
                const int error = last_socket_error();
                if (!would_block(error) && !interrupted(error)) {
                    finish_socket(
                        runtime, listener, map_socket_error(error), socket_error_message(error));
                }
                return;
            }
            if (!set_nonblocking(socket)) {
                close_socket(socket);
                continue;
            }
            configure_stream(socket, listener.listenOptions.accepted);
            auto stream = std::make_shared<SocketState>();
            stream->id = next_socket_id();
            stream->kind = SocketKind::Stream;
            stream->phase = SocketPhase::Open;
            stream->socket = socket;
            stream->userData = listener.userData;
            stream->scheme = "tcp";
            stream->streamOptions = listener.listenOptions.accepted;
            runtime.sockets.emplace(stream->id, stream);
            queue_event(
                runtime, {
                             .kind = Event::Kind::Accepted,
                             .id = listener.id,
                             .accepted = stream->id,
                             .endpoint = address_endpoint(
                                 reinterpret_cast<const sockaddr*>(&peer), peerLength, "tcp"),
                         });
        }
    }

    static void begin_write_shutdown(SocketState& state) {
        if (state.writeShutdown || !state.outbound.empty() || state.socket == InvalidSocket) {
            return;
        }
        detail::platform::shutdown_send(state.socket);
        state.writeShutdown = true;
    }

    static void handle_stream_write(Runtime& runtime, SocketState& state) {
        size_t writtenThisWake = 0;
        while (!state.outbound.empty() && writtenThisWake < MaxWriteBytesPerWake) {
            auto& item = state.outbound.front();
            const size_t remaining = item.data.size() - item.offset;
            const size_t amount = std::min({remaining, MaxWriteBytesPerWake - writtenThisWake,
                static_cast<size_t>(std::numeric_limits<int>::max())});
            const auto written =
                detail::platform::send_bytes(state.socket, item.data.data() + item.offset, amount);
            if (written < 0) {
                const int error = last_socket_error();
                if (would_block(error) || interrupted(error)) {
                    return;
                }
                finish_socket(runtime, state, map_socket_error(error), socket_error_message(error));
                return;
            }
            if (written == 0) {
                return;
            }
            item.offset += static_cast<size_t>(written);
            writtenThisWake += static_cast<size_t>(written);
            state.stats.queuedSendBytes -= static_cast<size_t>(written);
            state.stats.bytesSent += static_cast<uint64_t>(written);
            if (item.offset == item.data.size()) {
                state.outbound.pop_front();
            }
        }
        if (state.phase == SocketPhase::Closing) {
            begin_write_shutdown(state);
        }
        if (state.readClosed && state.outbound.empty()) {
            finish_socket(runtime, state, Error::None);
        }
    }

    static void handle_stream_read(Runtime& runtime, SocketState& state) {
        for (size_t readCount = 0; readCount < MaxReadsPerWake; ++readCount) {
            if (!payload_room(runtime)) {
                if (state.queuedInboundBytes != 0 && !state.pausedAt) {
                    state.pausedAt = Clock::now();
                }
                return;
            }
            size_t remainingCapacity = state.streamOptions.readChunkBytes;
            if (runtime.options.maxQueuedBytes != 0) {
                remainingCapacity = std::min(
                    remainingCapacity, runtime.options.maxQueuedBytes - runtime.queuedPayloadBytes);
            }
            if (remainingCapacity == 0) {
                return;
            }
            if (runtime.readScratch.size() < remainingCapacity) {
                runtime.readScratch.resize(remainingCapacity);
            }
            const auto read = detail::platform::receive_bytes(
                state.socket, runtime.readScratch.data(), remainingCapacity);
            if (read < 0) {
                const int error = last_socket_error();
                if (would_block(error) || interrupted(error)) {
                    return;
                }
                finish_socket(runtime, state, map_socket_error(error), socket_error_message(error));
                return;
            }
            if (read == 0) {
                state.readClosed = true;
                if (state.outbound.empty()) {
                    finish_socket(runtime, state, Error::None);
                }
                return;
            }
            std::vector<std::byte> data{
                runtime.readScratch.begin(), runtime.readScratch.begin() + read};
            state.stats.bytesReceived += static_cast<uint64_t>(read);
            state.queuedInboundBytes += static_cast<size_t>(read);
            queue_event(runtime, {
                                     .kind = Event::Kind::StreamData,
                                     .id = state.id,
                                     .data = std::move(data),
                                     .payload = true,
                                 });
        }
    }

    static void handle_datagram_read(Runtime& runtime, SocketState& state) {
        constexpr size_t MaxDatagramBytes = 65535;
        if (runtime.readScratch.size() < MaxDatagramBytes) {
            runtime.readScratch.resize(MaxDatagramBytes);
        }
        for (size_t readCount = 0; readCount < MaxReadsPerWake; ++readCount) {
            sockaddr_storage source{};
            SockLength sourceLength = sizeof(source);
            const auto read =
                detail::platform::receive_datagram(state.socket, runtime.readScratch.data(),
                    MaxDatagramBytes, reinterpret_cast<sockaddr*>(&source), &sourceLength);
            if (read < 0) {
                const int error = last_socket_error();
                if (would_block(error) || interrupted(error)) {
                    return;
                }
                finish_socket(runtime, state, map_socket_error(error), socket_error_message(error));
                return;
            }
            state.stats.bytesReceived += static_cast<uint64_t>(read);
            if (!payload_room(runtime, static_cast<size_t>(read))) {
                ++state.stats.inboundDropped;
                ++state.droppedSinceMarker;
                continue;
            }
            std::vector<std::byte> data{
                runtime.readScratch.begin(), runtime.readScratch.begin() + read};
            queue_event(
                runtime, {
                             .kind = Event::Kind::Datagram,
                             .id = state.id,
                             .endpoint = address_endpoint(
                                 reinterpret_cast<const sockaddr*>(&source), sourceLength, "udp"),
                             .data = std::move(data),
                             .payload = true,
                         });
        }
    }

    static void handle_datagram_write(Runtime& runtime, SocketState& state) {
        for (size_t count = 0; count < MaxDatagramsPerWake && !state.outbound.empty(); ++count) {
            auto& item = state.outbound.front();
            const Address& destination = *item.destination;
            static constexpr std::byte EmptyPayload{};
            const std::byte* data = item.data.empty() ? &EmptyPayload : item.data.data();
            const auto written =
                detail::platform::send_datagram(state.socket, data, item.data.size(),
                    reinterpret_cast<const sockaddr*>(&destination.storage), destination.length);
            if (written < 0) {
                const int error = last_socket_error();
                if (would_block(error) || interrupted(error)) {
                    return;
                }
                ++state.stats.sendFailures;
                state.stats.queuedSendBytes -= item.data.size();
                state.outbound.pop_front();
                continue;
            }
            state.stats.bytesSent += static_cast<uint64_t>(written);
            state.stats.queuedSendBytes -= item.data.size();
            state.outbound.pop_front();
        }
        (void)runtime;
    }

    static void handle_ready(Runtime& runtime, SocketState& state, short events) {
        if (state.phase == SocketPhase::Connecting && (events & (POLLOUT | POLLERR | POLLHUP)) != 0)
        {
            handle_connect(runtime, state);
            return;
        }
        if ((events & (POLLERR | POLLNVAL)) != 0) {
            int error = detail::platform::socket_error(state.socket);
            if (error == 0) {
                error = last_socket_error();
            }
            finish_socket(runtime, state, map_socket_error(error), socket_error_message(error));
            return;
        }
        switch (state.kind) {
        case SocketKind::Listener:
            if ((events & POLLIN) != 0) {
                handle_accept(runtime, state);
            }
            break;
        case SocketKind::Stream:
            if ((events & POLLOUT) != 0) {
                handle_stream_write(runtime, state);
            }
            if (state.phase != SocketPhase::Closed && (events & (POLLIN | POLLHUP)) != 0) {
                handle_stream_read(runtime, state);
            }
            break;
        case SocketKind::Datagram:
            if ((events & POLLOUT) != 0) {
                handle_datagram_write(runtime, state);
            }
            if (state.phase != SocketPhase::Closed && (events & POLLIN) != 0) {
                handle_datagram_read(runtime, state);
            }
            break;
        case SocketKind::Resolver:
            break;
        }
    }

    void run() noexcept try {
        while (!m_stopping.load(std::memory_order_acquire)) {
            std::vector<PollDescriptor> descriptors;
            std::vector<ReadySocket> ready;
            descriptors.push_back({m_wakeup.descriptor(), POLLIN, 0});
            ready.push_back({});
            const auto now = Clock::now();
            auto pollInterval =
                std::chrono::duration_cast<std::chrono::milliseconds>(MaximumPollInterval);

            for (const auto& runtime : runtimes()) {
                std::lock_guard lock{runtime->mutex};
                if (runtime->destroyRequested) {
                    abort_runtime(*runtime, true);
                    runtime->registered = false;
                    runtime->destroyAcknowledged = true;
                    runtime->destroyed.notify_all();
                    continue;
                }
                std::vector<std::shared_ptr<SocketState>> states;
                states.reserve(runtime->sockets.size());
                for (const auto& [id, state] : runtime->sockets) {
                    (void)id;
                    states.emplace_back(state);
                }
                for (const auto& state : states) {
                    process_resolutions(*runtime, *state);
                    process_deadlines(*runtime, *state, now);
                    process_dropped_marker(*runtime, *state);
                    if (const auto deadline = next_deadline(*state, now)) {
                        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
                            std::max(*deadline - now, Clock::duration::zero()));
                        pollInterval = std::min(pollInterval, remaining);
                    }
                    if (state->socket == InvalidSocket || state->phase == SocketPhase::Closed) {
                        continue;
                    }
                    const short events = requested_events(*runtime, *state);
                    if (events == 0) {
                        continue;
                    }
                    descriptors.push_back({state->socket, events, 0});
                    ready.push_back({runtime, state, state->socket});
                }
            }

            const int result = detail::platform::poll(descriptors, pollInterval);
            if (result <= 0) {
                continue;
            }
            if ((descriptors.front().revents & POLLIN) != 0) {
                m_wakeup.drain();
            }
            for (size_t index = 1; index < descriptors.size(); ++index) {
                if (descriptors[index].revents == 0) {
                    continue;
                }
                auto& item = ready[index];
                std::lock_guard lock{item.runtime->mutex};
                const auto found = item.runtime->sockets.find(item.state->id);
                if (found == item.runtime->sockets.end() ||
                    found->second.get() != item.state.get() ||
                    item.state->socket != item.descriptor)
                {
                    continue;
                }
                handle_ready(*item.runtime, *item.state, descriptors[index].revents);
            }
        }
    }
    BOREALIS_CATCH()

    std::mutex m_mutex;
    std::condition_variable m_shutdownComplete;
    std::vector<std::weak_ptr<Runtime>> m_runtimes;
    std::thread m_thread;
    std::atomic_bool m_stopping = false;
    bool m_running = false;
    bool m_shuttingDown = false;
    detail::platform::Wakeup m_wakeup;
};

NetworkManager& manager() {
    static NetworkManager value;
    static std::once_flag registered;
    std::call_once(
        registered, [] { borealis::detail::register_shutdown_hook([] { manager().shutdown(); }); });
    return value;
}

std::shared_ptr<SocketState> find_state(Runtime& runtime, SocketId id) {
    const auto found = runtime.sockets.find(id);
    return found != runtime.sockets.end() ? found->second : nullptr;
}

std::optional<ParsedEndpoint> parse_endpoint_impl(std::string_view text) {
    const auto parsed = borealis::url::parse(text);
    if (!parsed || (parsed->scheme != "tcp" && parsed->scheme != "udp") || !parsed->port ||
        parsed->hasResource)
    {
        return std::nullopt;
    }

    ParsedEndpoint endpoint{
        .scheme = parsed->scheme,
        .host = parsed->host,
        .port = *parsed->port,
    };
    in_addr ipv4{};
    if (inet_pton(AF_INET, endpoint.host.c_str(), &ipv4) == 1) {
        char canonical[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &ipv4, canonical, sizeof(canonical));
        endpoint.host = canonical;
        endpoint.literal = true;
        return endpoint;
    }

    std::string_view ipv6Text = endpoint.host;
    const size_t scope = ipv6Text.find('%');
    if (scope != std::string_view::npos) {
        if (scope == 0 || scope + 1 == ipv6Text.size()) {
            return std::nullopt;
        }
        ipv6Text = ipv6Text.substr(0, scope);
    }
    in6_addr ipv6{};
    const std::string ipv6Address{ipv6Text};
    if (inet_pton(AF_INET6, ipv6Address.c_str(), &ipv6) == 1) {
        if (!parsed->ipv6) {
            return std::nullopt;
        }
        if (is_ipv4_mapped(ipv6)) {
            char canonical[INET_ADDRSTRLEN]{};
            in_addr mapped{};
            std::memcpy(&mapped, &ipv6.s6_addr[12], sizeof(mapped));
            inet_ntop(AF_INET, &mapped, canonical, sizeof(canonical));
            endpoint.host = canonical;
            endpoint.literal = true;
            endpoint.ipv6 = false;
            return endpoint;
        }
        char canonical[INET6_ADDRSTRLEN]{};
        inet_ntop(AF_INET6, &ipv6, canonical, sizeof(canonical));
        endpoint.host = canonical + (scope == std::string_view::npos ? std::string{} :
                                                                       endpoint.host.substr(scope));
        endpoint.literal = true;
        endpoint.ipv6 = true;
        return endpoint;
    }
    if (parsed->ipv6) {
        return std::nullopt;
    }
    std::transform(endpoint.host.begin(), endpoint.host.end(), endpoint.host.begin(),
        borealis::detail::ascii_lower);
    return endpoint;
}

}  // namespace

struct Context::Impl {
    explicit Impl(ContextOptions options) : runtime{std::make_shared<Runtime>(options)} {
        if (available()) {
            (void)manager().add(runtime);
        }
    }

    ~Impl() {
        std::unique_lock lock{runtime->mutex};
        if (!runtime->registered) {
            return;
        }
        runtime->destroyRequested = true;
        lock.unlock();
        manager().wake();
        lock.lock();
        runtime->destroyed.wait(
            lock, [this] { return runtime->destroyAcknowledged || !runtime->registered; });
    }

    std::shared_ptr<Runtime> runtime;
};

Context::Context(ContextOptions options) : m_impl{std::make_unique<Impl>(options)} {}

Context::~Context() = default;

SocketId Context::connect(std::string_view endpointText, StreamOptions options, void* userData) {
    const auto endpoint = parse_endpoint(endpointText);
    if (!available() || !endpoint || endpoint->scheme != "tcp") {
        return 0;
    }
    if (!manager().add(m_impl->runtime)) {
        return 0;
    }
    options.readChunkBytes = std::clamp<size_t>(options.readChunkBytes, 1, INT_MAX);
    auto& runtime = *m_impl->runtime;
    std::lock_guard lock{runtime.mutex};
    if (runtime.destroyRequested ||
        (runtime.options.maxStreams != 0 && stream_count(runtime) >= runtime.options.maxStreams))
    {
        return 0;
    }
    auto state = std::make_shared<SocketState>();
    state->id = next_socket_id();
    state->kind = SocketKind::Stream;
    state->phase = SocketPhase::Resolving;
    state->userData = userData;
    state->scheme = "tcp";
    state->streamOptions = options;
    state->createdAt = Clock::now();
    state->resolution = start_resolution(*endpoint);
    if (!state->resolution) {
        return 0;
    }
    runtime.sockets.emplace(state->id, state);
    manager().wake();
    return state->id;
}

Context::BindResult Context::listen(
    std::string_view bindText, ListenOptions options, void* userData) {
    const auto endpoint = parse_endpoint(bindText);
    if (!endpoint || endpoint->scheme != "tcp" || !endpoint->literal) {
        return {.error = Error::InvalidEndpoint, .message = "A TCP literal endpoint is required"};
    }
    if (!available()) {
        return {.error = Error::Network, .message = "Networking is unavailable"};
    }
    if (!manager().add(m_impl->runtime)) {
        return {.error = Error::Network, .message = "Network I/O thread could not start"};
    }
    options.accepted.readChunkBytes =
        std::clamp<size_t>(options.accepted.readChunkBytes, 1, INT_MAX);
    auto& runtime = *m_impl->runtime;
    std::lock_guard lock{runtime.mutex};
    if (runtime.destroyRequested ||
        (runtime.options.maxListeners != 0 &&
            count_kind(runtime, SocketKind::Listener) >= runtime.options.maxListeners))
    {
        return {.error = Error::Network, .message = "Listener limit reached"};
    }
    const auto bound = bind_socket(
        *endpoint, SOCK_STREAM, IPPROTO_TCP,
        [&](NativeSocket socket) {
            configure_stream(socket, options.accepted);
            if (options.reuseAddress) {
                detail::platform::configure_listener_reuse(socket);
            }
        },
        options.backlog);
    if (bound.socket == InvalidSocket) {
        return {.error = bound.error, .message = bound.message};
    }
    auto state = std::make_shared<SocketState>();
    state->id = next_socket_id();
    state->kind = SocketKind::Listener;
    state->phase = SocketPhase::Open;
    state->socket = bound.socket;
    state->userData = userData;
    state->scheme = "tcp";
    state->listenOptions = options;
    runtime.sockets.emplace(state->id, state);
    manager().wake();
    return {
        .id = state->id,
        .localEndpoint = address_endpoint(
            reinterpret_cast<const sockaddr*>(&bound.local), bound.localLength, "tcp"),
    };
}

Context::BindResult Context::open_datagram(
    std::string_view bindText, DatagramOptions options, void* userData) {
    const auto endpoint = parse_endpoint(bindText);
    if (!endpoint || endpoint->scheme != "udp" || !endpoint->literal) {
        return {.error = Error::InvalidEndpoint, .message = "A UDP literal endpoint is required"};
    }
    if (!available()) {
        return {.error = Error::Network, .message = "Networking is unavailable"};
    }
    if (!manager().add(m_impl->runtime)) {
        return {.error = Error::Network, .message = "Network I/O thread could not start"};
    }
    auto& runtime = *m_impl->runtime;
    std::lock_guard lock{runtime.mutex};
    if (runtime.destroyRequested ||
        (runtime.options.maxDatagramSockets != 0 &&
            count_kind(runtime, SocketKind::Datagram) >= runtime.options.maxDatagramSockets))
    {
        return {.error = Error::Network, .message = "Datagram socket limit reached"};
    }
    const auto bound = bind_socket(*endpoint, SOCK_DGRAM, IPPROTO_UDP, [&](NativeSocket socket) {
        configure_common_socket(socket);
        const int receiveBytes =
            static_cast<int>(std::min<size_t>(options.recvBufferBytes, INT_MAX));
        const int sendBytes = static_cast<int>(std::min<size_t>(options.sendBufferBytes, INT_MAX));
        set_socket_option(socket, SOL_SOCKET, SO_RCVBUF, &receiveBytes, sizeof(receiveBytes));
        set_socket_option(socket, SOL_SOCKET, SO_SNDBUF, &sendBytes, sizeof(sendBytes));
    });
    if (bound.socket == InvalidSocket) {
        return {.error = bound.error, .message = bound.message};
    }
    auto state = std::make_shared<SocketState>();
    state->id = next_socket_id();
    state->kind = SocketKind::Datagram;
    state->phase = SocketPhase::Open;
    state->socket = bound.socket;
    state->userData = userData;
    state->scheme = "udp";
    state->datagramOptions = options;
    runtime.sockets.emplace(state->id, state);
    manager().wake();
    return {
        .id = state->id,
        .localEndpoint = address_endpoint(
            reinterpret_cast<const sockaddr*>(&bound.local), bound.localLength, "udp"),
    };
}

SocketId Context::resolve(std::string_view endpointText, void* userData) {
    const auto endpoint = parse_endpoint(endpointText);
    if (!available() || !endpoint) {
        return 0;
    }
    if (!manager().add(m_impl->runtime)) {
        return 0;
    }
    auto& runtime = *m_impl->runtime;
    std::lock_guard lock{runtime.mutex};
    if (runtime.destroyRequested ||
        (runtime.options.maxResolvers != 0 &&
            count_kind(runtime, SocketKind::Resolver) >= runtime.options.maxResolvers))
    {
        return 0;
    }
    auto state = std::make_shared<SocketState>();
    state->id = next_socket_id();
    state->kind = SocketKind::Resolver;
    state->phase = SocketPhase::Resolving;
    state->userData = userData;
    state->scheme = endpoint->scheme;
    state->resolution = start_resolution(*endpoint);
    if (!state->resolution) {
        return 0;
    }
    runtime.sockets.emplace(state->id, state);
    manager().wake();
    return state->id;
}

SendResult Context::send(SocketId id, std::span<const std::byte> bytes) {
    auto& runtime = *m_impl->runtime;
    std::lock_guard lock{runtime.mutex};
    const auto state = find_state(runtime, id);
    if (!state || state->kind != SocketKind::Stream || state->phase != SocketPhase::Open) {
        return SendResult::NotOpen;
    }
    if (bytes.size() >
        state->streamOptions.maxSendQueueBytes -
            std::min(state->stats.queuedSendBytes, state->streamOptions.maxSendQueueBytes))
    {
        return SendResult::QueueFull;
    }
    if (bytes.empty()) {
        return SendResult::Ok;
    }
    state->outbound.push_back({.data = std::vector<std::byte>{bytes.begin(), bytes.end()}});
    state->stats.queuedSendBytes += bytes.size();
    manager().wake();
    return SendResult::Ok;
}

SendResult Context::send_to(
    SocketId id, std::string_view endpointText, std::span<const std::byte> bytes) {
    const auto endpoint = parse_endpoint(endpointText);
    if (!endpoint || endpoint->scheme != "udp" || !endpoint->literal) {
        return SendResult::InvalidEndpoint;
    }
    if (bytes.size() > 65507) {
        return SendResult::TooLarge;
    }
    const auto address = literal_address(*endpoint);
    if (!address) {
        return SendResult::InvalidEndpoint;
    }
    auto& runtime = *m_impl->runtime;
    std::lock_guard lock{runtime.mutex};
    const auto state = find_state(runtime, id);
    if (!state || state->kind != SocketKind::Datagram || state->phase != SocketPhase::Open) {
        return SendResult::NotOpen;
    }
    if (bytes.size() >
        state->datagramOptions.maxSendQueueBytes -
            std::min(state->stats.queuedSendBytes, state->datagramOptions.maxSendQueueBytes))
    {
        return SendResult::QueueFull;
    }
    state->outbound.push_back({
        .data = std::vector<std::byte>{bytes.begin(), bytes.end()},
        .destination = *address,
    });
    state->stats.queuedSendBytes += bytes.size();
    manager().wake();
    return SendResult::Ok;
}

void Context::set_user_data(SocketId id, void* userData) {
    auto& runtime = *m_impl->runtime;
    std::lock_guard lock{runtime.mutex};
    if (const auto state = find_state(runtime, id)) {
        state->userData = userData;
    }
}

std::optional<Stats> Context::stats(SocketId id) const {
    auto& runtime = *m_impl->runtime;
    std::lock_guard lock{runtime.mutex};
    if (const auto state = find_state(runtime, id)) {
        return state->stats;
    }
    return std::nullopt;
}

void Context::close(SocketId id) {
    auto& runtime = *m_impl->runtime;
    std::lock_guard lock{runtime.mutex};
    const auto state = find_state(runtime, id);
    if (!state || state->phase == SocketPhase::Closed || state->phase == SocketPhase::Closing) {
        return;
    }
    if (state->kind == SocketKind::Resolver) {
        if (state->resolution) {
            state->resolution->canceled.store(true, std::memory_order_release);
        }
        finish_resolver(runtime, *state, Error::Canceled, {}, "Resolution canceled");
    } else if (state->kind == SocketKind::Stream && state->phase == SocketPhase::Open) {
        state->phase = SocketPhase::Closing;
        state->closeDeadline = Clock::now() + state->streamOptions.closeTimeout;
        NetworkManager::start_stream_close(*state);
    } else if (state->kind == SocketKind::Stream) {
        if (state->resolution) {
            state->resolution->canceled.store(true, std::memory_order_release);
        }
        finish_socket(runtime, *state, Error::Canceled, "Connection canceled");
    } else {
        finish_socket(runtime, *state, Error::None);
    }
    manager().wake();
}

bool Context::poll(Event& out) {
    auto& runtime = *m_impl->runtime;
    std::lock_guard lock{runtime.mutex};
    runtime.lastPayload.clear();
    out = {};
    if (runtime.events.empty()) {
        return false;
    }
    const bool payloadWasFull = !payload_room(runtime);
    QueuedEvent event = std::move(runtime.events.front());
    runtime.events.pop_front();
    if (event.payload) {
        runtime.queuedPayloadBytes -= event.data.size();
        --runtime.queuedPayloadEvents;
        if (const auto state = find_state(runtime, event.id)) {
            state->queuedInboundBytes -= std::min(state->queuedInboundBytes, event.data.size());
        }
        runtime.lastPayload = std::move(event.data);
    }
    out.kind = event.kind;
    out.id = event.id;
    out.accepted = event.accepted;
    out.endpoint = std::move(event.endpoint);
    out.data = runtime.lastPayload;
    out.dropped = event.dropped;
    out.error = event.error;
    out.message = std::move(event.message);
    const auto state = find_state(runtime, event.id);
    if (state) {
        out.userData = state->userData;
    }
    bool shouldWake = event.payload && payloadWasFull && payload_room(runtime);
    if (event.kind == Event::Kind::Dropped && state) {
        state->droppedMarkerQueued = false;
        shouldWake = state->phase == SocketPhase::Open && state->droppedSinceMarker != 0;
    }
    if (event.kind == Event::Kind::Closed || event.kind == Event::Kind::Resolved) {
        runtime.sockets.erase(event.id);
        shouldWake = true;
    }
    if (shouldWake) {
        manager().wake();
    }
    return true;
}

bool available() noexcept {
    return detail::platform::available();
}

std::optional<ParsedEndpoint> parse_endpoint(std::string_view text) {
    return parse_endpoint_impl(text);
}

std::string format_endpoint(const ParsedEndpoint& endpoint) {
    if ((endpoint.scheme != "tcp" && endpoint.scheme != "udp") || endpoint.host.empty()) {
        return {};
    }
    const bool bracketHost = endpoint.ipv6 || endpoint.host.find(':') != std::string::npos;
    return endpoint.scheme + "://" + (bracketHost ? "[" + endpoint.host + "]" : endpoint.host) +
           ":" + std::to_string(endpoint.port);
}

}  // namespace borealis::net
