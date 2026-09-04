#include "platform.hpp"

#include <mstcpip.h>

#include <algorithm>
#include <array>
#include <climits>
#include <limits>
#include <utility>

namespace borealis::net::detail::platform {

bool available() noexcept {
    return true;
}

bool initialize() noexcept {
    WSADATA data{};
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

void cleanup() noexcept {
    WSACleanup();
}

int last_socket_error() noexcept {
    return WSAGetLastError();
}

bool would_block(int error) noexcept {
    return error == WSAEWOULDBLOCK;
}

bool connect_in_progress(int error) noexcept {
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS || error == WSAEINVAL;
}

bool interrupted(int error) noexcept {
    return error == WSAEINTR;
}

void close_socket(NativeSocket socket) noexcept {
    if (socket != InvalidSocket) {
        closesocket(socket);
    }
}

bool set_nonblocking(NativeSocket socket) noexcept {
    u_long enabled = 1;
    return ioctlsocket(socket, FIONBIO, &enabled) == 0;
}

std::string socket_error_message(int error) {
    char* text = nullptr;
    const DWORD length = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(error), 0, reinterpret_cast<char*>(&text), 0, nullptr);
    std::string result = length != 0 && text != nullptr ? std::string{text, length} :
                                                          "Socket error " + std::to_string(error);
    if (text != nullptr) {
        LocalFree(text);
    }
    while (!result.empty() && (result.back() == '\r' || result.back() == '\n')) {
        result.pop_back();
    }
    return result;
}

std::string resolve_error_message(int error) {
    return gai_strerrorA(error);
}

Error map_socket_error(int error) noexcept {
    switch (error) {
    case WSAECONNREFUSED:
        return Error::Refused;
    case WSAEHOSTUNREACH:
    case WSAENETUNREACH:
    case WSAENETDOWN:
        return Error::Unreachable;
    case WSAECONNRESET:
    case WSAECONNABORTED:
        return Error::Reset;
    case WSAEADDRINUSE:
        return Error::AddressInUse;
    case WSAEACCES:
        return Error::Permission;
    case WSAETIMEDOUT:
        return Error::Timeout;
    default:
        return Error::Network;
    }
}

bool set_socket_option(
    NativeSocket socket, int level, int name, const void* value, size_t size) noexcept {
    return setsockopt(socket, level, name, static_cast<const char*>(value),
               static_cast<SockLength>(size)) == 0;
}

void configure_common_socket(NativeSocket) noexcept {}

void configure_listener_reuse(NativeSocket socket) noexcept {
    const int enabled = 1;
    set_socket_option(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, &enabled, sizeof(enabled));
}

static bool is_loopback(const sockaddr* address) noexcept {
    if (address->sa_family == AF_INET) {
        const auto* in4 = reinterpret_cast<const sockaddr_in*>(address);
        return in4->sin_addr.S_un.S_un_b.s_b1 == 127;
    }
    if (address->sa_family == AF_INET6) {
        const auto* in6 = reinterpret_cast<const sockaddr_in6*>(address);
        return IN6_IS_ADDR_LOOPBACK(&in6->sin6_addr);
    }
    return false;
}

void prepare_connect(NativeSocket socket, const sockaddr* address) noexcept {
    if (!is_loopback(address)) {
        return;
    }
    TCP_INITIAL_RTO_PARAMETERS parameters{};
    parameters.Rtt = TCP_INITIAL_RTO_DEFAULT_RTT;
    parameters.MaxSynRetransmissions = TCP_INITIAL_RTO_NO_SYN_RETRANSMISSIONS;
    DWORD bytes = 0;
    WSAIoctl(socket, SIO_TCP_INITIAL_RTO, &parameters, sizeof(parameters), nullptr, 0, &bytes,
        nullptr, nullptr);
}

void shutdown_send(NativeSocket socket) noexcept {
    ::shutdown(socket, SD_SEND);
}

int socket_error(NativeSocket socket) noexcept {
    int error = 0;
    SockLength length = sizeof(error);
    if (getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) != 0) {
        return last_socket_error();
    }
    return error;
}

std::ptrdiff_t send_bytes(NativeSocket socket, const void* data, size_t size) noexcept {
    const int amount = static_cast<int>(std::min(size, static_cast<size_t>(INT_MAX)));
    return ::send(socket, static_cast<const char*>(data), amount, 0);
}

std::ptrdiff_t receive_bytes(NativeSocket socket, void* data, size_t size) noexcept {
    const int amount = static_cast<int>(std::min(size, static_cast<size_t>(INT_MAX)));
    return ::recv(socket, static_cast<char*>(data), amount, 0);
}

std::ptrdiff_t send_datagram(NativeSocket socket, const void* data, size_t size,
    const sockaddr* destination, SockLength destinationLength) noexcept {
    const int amount = static_cast<int>(std::min(size, static_cast<size_t>(INT_MAX)));
    return ::sendto(
        socket, static_cast<const char*>(data), amount, 0, destination, destinationLength);
}

std::ptrdiff_t receive_datagram(NativeSocket socket, void* data, size_t size, sockaddr* source,
    SockLength* sourceLength) noexcept {
    const int amount = static_cast<int>(std::min(size, static_cast<size_t>(INT_MAX)));
    return ::recvfrom(socket, static_cast<char*>(data), amount, 0, source, sourceLength);
}

int poll(std::span<PollDescriptor> descriptors, std::chrono::milliseconds timeout) noexcept {
    return WSAPoll(descriptors.data(), static_cast<ULONG>(descriptors.size()),
        static_cast<int>(timeout.count()));
}

bool Wakeup::open() noexcept {
    NativeSocket reader = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    NativeSocket writer = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (reader == InvalidSocket || writer == InvalidSocket) {
        close_socket(reader);
        close_socket(writer);
        return false;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(reader, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        close_socket(reader);
        close_socket(writer);
        return false;
    }
    SockLength length = sizeof(address);
    if (getsockname(reader, reinterpret_cast<sockaddr*>(&address), &length) != 0 ||
        connect(writer, reinterpret_cast<const sockaddr*>(&address), length) != 0)
    {
        close_socket(reader);
        close_socket(writer);
        return false;
    }
    set_nonblocking(reader);
    set_nonblocking(writer);
    std::lock_guard lock{m_mutex};
    m_read = reader;
    m_write = writer;
    return true;
}

void Wakeup::close() noexcept {
    std::lock_guard lock{m_mutex};
    close_socket(std::exchange(m_read, InvalidSocket));
    close_socket(std::exchange(m_write, InvalidSocket));
}

void Wakeup::wake() noexcept {
    std::lock_guard lock{m_mutex};
    if (m_write != InvalidSocket) {
        const char byte = 0;
        ::send(m_write, &byte, 1, 0);
    }
}

void Wakeup::drain() noexcept {
    std::lock_guard lock{m_mutex};
    if (m_read == InvalidSocket) {
        return;
    }
    std::array<char, 256> buffer{};
    while (::recv(m_read, buffer.data(), static_cast<int>(buffer.size()), 0) > 0) {
    }
}

NativeSocket Wakeup::descriptor() const noexcept {
    std::lock_guard lock{m_mutex};
    return m_read;
}

}  // namespace borealis::net::detail::platform
