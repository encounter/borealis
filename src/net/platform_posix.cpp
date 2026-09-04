#include "platform.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <unistd.h>
#include <utility>

#if defined(__linux__)
#include <sys/eventfd.h>
#endif

namespace borealis::net::detail::platform {

bool available() noexcept {
#if defined(__SWITCH__)
    return false;
#else
    return true;
#endif
}

bool initialize() noexcept {
    return true;
}

void cleanup() noexcept {}

int last_socket_error() noexcept {
    return errno;
}

bool would_block(int error) noexcept {
    return error == EAGAIN || error == EWOULDBLOCK;
}

bool connect_in_progress(int error) noexcept {
    return error == EINPROGRESS || would_block(error);
}

bool interrupted(int error) noexcept {
    return error == EINTR;
}

void close_socket(NativeSocket socket) noexcept {
    if (socket != InvalidSocket) {
        ::close(socket);
    }
}

bool set_nonblocking(NativeSocket socket) noexcept {
    const int flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
}

std::string socket_error_message(int error) {
    return std::strerror(error);
}

std::string resolve_error_message(int error) {
    return gai_strerror(error);
}

Error map_socket_error(int error) noexcept {
    switch (error) {
    case ECONNREFUSED:
        return Error::Refused;
    case EHOSTUNREACH:
    case ENETUNREACH:
    case ENETDOWN:
        return Error::Unreachable;
    case ECONNRESET:
    case ECONNABORTED:
    case EPIPE:
        return Error::Reset;
    case EADDRINUSE:
        return Error::AddressInUse;
    case EACCES:
    case EPERM:
        return Error::Permission;
    case ETIMEDOUT:
        return Error::Timeout;
    default:
        return Error::Network;
    }
}

bool set_socket_option(
    NativeSocket socket, int level, int name, const void* value, size_t size) noexcept {
    return setsockopt(socket, level, name, value, static_cast<SockLength>(size)) == 0;
}

void configure_common_socket(NativeSocket socket) noexcept {
#if defined(__APPLE__)
    const int enabled = 1;
    set_socket_option(socket, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#else
    (void)socket;
#endif
}

void configure_listener_reuse(NativeSocket socket) noexcept {
    const int enabled = 1;
    set_socket_option(socket, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
}

void shutdown_send(NativeSocket socket) noexcept {
    ::shutdown(socket, SHUT_WR);
}

int socket_error(NativeSocket socket) noexcept {
    int error = 0;
    SockLength length = sizeof(error);
    if (getsockopt(socket, SOL_SOCKET, SO_ERROR, &error, &length) != 0) {
        return last_socket_error();
    }
    return error;
}

std::ptrdiff_t send_bytes(NativeSocket socket, const void* data, size_t size) noexcept {
#if defined(MSG_NOSIGNAL)
    constexpr int Flags = MSG_NOSIGNAL;
#else
    constexpr int Flags = 0;
#endif
    return ::send(socket, data, size, Flags);
}

std::ptrdiff_t receive_bytes(NativeSocket socket, void* data, size_t size) noexcept {
    return ::recv(socket, data, size, 0);
}

std::ptrdiff_t send_datagram(NativeSocket socket, const void* data, size_t size,
    const sockaddr* destination, SockLength destinationLength) noexcept {
#if defined(MSG_NOSIGNAL)
    constexpr int Flags = MSG_NOSIGNAL;
#else
    constexpr int Flags = 0;
#endif
    return ::sendto(socket, data, size, Flags, destination, destinationLength);
}

std::ptrdiff_t receive_datagram(NativeSocket socket, void* data, size_t size, sockaddr* source,
    SockLength* sourceLength) noexcept {
    return ::recvfrom(socket, data, size, 0, source, sourceLength);
}

int poll(std::span<PollDescriptor> descriptors, std::chrono::milliseconds timeout) noexcept {
    return ::poll(descriptors.data(), descriptors.size(), static_cast<int>(timeout.count()));
}

bool Wakeup::open() noexcept {
#if defined(__linux__)
    const int descriptor = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (descriptor < 0) {
        return false;
    }
    std::lock_guard lock{m_mutex};
    m_read = descriptor;
    m_write = descriptor;
#else
    int descriptors[2]{};
    if (pipe(descriptors) != 0) {
        return false;
    }
    set_nonblocking(descriptors[0]);
    set_nonblocking(descriptors[1]);
    std::lock_guard lock{m_mutex};
    m_read = descriptors[0];
    m_write = descriptors[1];
#endif
    return true;
}

void Wakeup::close() noexcept {
    std::lock_guard lock{m_mutex};
    const NativeSocket reader = std::exchange(m_read, InvalidSocket);
    const NativeSocket writer = std::exchange(m_write, InvalidSocket);
    close_socket(reader);
    if (writer != reader) {
        close_socket(writer);
    }
}

void Wakeup::wake() noexcept {
    std::lock_guard lock{m_mutex};
    if (m_write == InvalidSocket) {
        return;
    }
#if defined(__linux__)
    const uint64_t value = 1;
    ::write(m_write, &value, sizeof(value));
#else
    const char byte = 0;
    ::write(m_write, &byte, 1);
#endif
}

void Wakeup::drain() noexcept {
    std::lock_guard lock{m_mutex};
    if (m_read == InvalidSocket) {
        return;
    }
#if defined(__linux__)
    uint64_t value = 0;
    while (::read(m_read, &value, sizeof(value)) > 0) {
    }
#else
    std::array<char, 256> buffer{};
    while (::read(m_read, buffer.data(), buffer.size()) > 0) {
    }
#endif
}

NativeSocket Wakeup::descriptor() const noexcept {
    std::lock_guard lock{m_mutex};
    return m_read;
}

}  // namespace borealis::net::detail::platform
