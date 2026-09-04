#pragma once

#include "ascii.hpp"
#include "borealis/ws.hpp"

#include <algorithm>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace borealis::ws::detail {

class EventSink {
public:
    virtual ~EventSink() = default;
    virtual void opened(std::string protocol, std::vector<http::Header> headers) = 0;
    virtual void message(MessageKind kind, std::string data) = 0;
    virtual void send_complete(size_t bytes, Error error, std::string message) = 0;
    virtual void closed(Error error, std::string message, int status, uint16_t code,
        std::string reason, std::vector<http::Header> headers = {}) = 0;
};

class Transport {
public:
    virtual ~Transport() = default;
    virtual void start(const Options& options, std::shared_ptr<EventSink> sink) = 0;
    virtual bool send(MessageKind kind, std::string data) = 0;
    virtual void close(uint16_t code, std::string reason) = 0;
    virtual void abort() noexcept = 0;
};

inline std::string selected_protocol(std::span<const http::Header> headers) {
    const auto found = std::ranges::find_if(headers, [](const auto& header) {
        return borealis::detail::ascii_iequals(header.name, "Sec-WebSocket-Protocol");
    });
    return found != headers.end() ? found->value : std::string{};
}

std::unique_ptr<Transport> make_transport();
bool backend_available() noexcept;

using TransportFactory = std::function<std::unique_ptr<Transport>()>;
Connection connect_for_testing(Options options, TransportFactory factory);

}  // namespace borealis::ws::detail
