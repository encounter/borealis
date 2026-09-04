#pragma once

#include "borealis/net.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WS2tcpip.h>
#include <WinSock2.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#endif

namespace borealis::net::detail::platform {

#if defined(_WIN32)
using NativeSocket = SOCKET;
using PollDescriptor = WSAPOLLFD;
using SockLength = int;
constexpr NativeSocket InvalidSocket = INVALID_SOCKET;
#else
using NativeSocket = int;
using PollDescriptor = pollfd;
using SockLength = socklen_t;
constexpr NativeSocket InvalidSocket = -1;
#endif

bool available() noexcept;
bool initialize() noexcept;
void cleanup() noexcept;

int last_socket_error() noexcept;
bool would_block(int error) noexcept;
bool connect_in_progress(int error) noexcept;
bool interrupted(int error) noexcept;
void close_socket(NativeSocket socket) noexcept;
bool set_nonblocking(NativeSocket socket) noexcept;
std::string socket_error_message(int error);
std::string resolve_error_message(int error);
Error map_socket_error(int error) noexcept;

bool set_socket_option(
    NativeSocket socket, int level, int name, const void* value, size_t size) noexcept;
void configure_common_socket(NativeSocket socket) noexcept;
void configure_listener_reuse(NativeSocket socket) noexcept;
void shutdown_send(NativeSocket socket) noexcept;
int socket_error(NativeSocket socket) noexcept;

std::ptrdiff_t send_bytes(NativeSocket socket, const void* data, size_t size) noexcept;
std::ptrdiff_t receive_bytes(NativeSocket socket, void* data, size_t size) noexcept;
std::ptrdiff_t send_datagram(NativeSocket socket, const void* data, size_t size,
    const sockaddr* destination, SockLength destinationLength) noexcept;
std::ptrdiff_t receive_datagram(NativeSocket socket, void* data, size_t size, sockaddr* source,
    SockLength* sourceLength) noexcept;
int poll(std::span<PollDescriptor> descriptors, std::chrono::milliseconds timeout) noexcept;

class Wakeup {
public:
    Wakeup() = default;
    Wakeup(const Wakeup&) = delete;
    Wakeup& operator=(const Wakeup&) = delete;
    ~Wakeup() { close(); }

    bool open() noexcept;
    void close() noexcept;
    void wake() noexcept;
    void drain() noexcept;
    NativeSocket descriptor() const noexcept;

private:
    mutable std::mutex m_mutex;
    NativeSocket m_read = InvalidSocket;
    NativeSocket m_write = InvalidSocket;
};

}  // namespace borealis::net::detail::platform
