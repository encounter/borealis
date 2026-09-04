#include "../ws_internal.hpp"
#include "../http/url_session_common.hpp"

#include "borealis/log.hpp"

#import <Foundation/Foundation.h>

#include <algorithm>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using borealis::detail::url_session::response_headers;
using borealis::detail::url_session::to_nsstring;
using borealis::detail::url_session::to_string;

constexpr borealis::Log Log{"borealis::ws"};

borealis::ws::Error map_error(NSError* error) {
    return borealis::detail::url_session::map_error<borealis::ws::Error>(error);
}

}  // namespace

@interface BorealisWebSocketDelegate : NSObject <NSURLSessionWebSocketDelegate, NSURLSessionTaskDelegate> {
@public
    std::mutex _mutex;
    std::condition_variable _invalidated;
    std::shared_ptr<borealis::ws::detail::EventSink> _sink;
    BOOL _terminal;
    BOOL _active;
    BOOL _sessionInvalidated;
}
@property(nonatomic, strong) NSURLSession* session;
@property(nonatomic, strong) NSURLSessionWebSocketTask* task;
- (void)prepareWithSink:(std::shared_ptr<borealis::ws::detail::EventSink>)sink;
- (void)receiveNext;
- (void)schedulePing:(std::chrono::milliseconds)interval;
- (void)failAfterCloseOpportunity:(NSError*)error;
- (void)failSendAfterCloseOpportunity:(NSError*)error bytes:(size_t)bytes;
- (void)deactivate;
- (void)waitForInvalidation;
@end

@implementation BorealisWebSocketDelegate

- (void)prepareWithSink:(std::shared_ptr<borealis::ws::detail::EventSink>)sink {
    std::lock_guard lock{_mutex};
    _sink = std::move(sink);
    _terminal = NO;
    _active = YES;
    _sessionInvalidated = NO;
}

- (std::shared_ptr<borealis::ws::detail::EventSink>)sink {
    std::lock_guard lock{_mutex};
    return _active ? _sink : nullptr;
}

- (BOOL)claimTerminal {
    std::lock_guard lock{_mutex};
    if (!_active || _terminal) {
        return NO;
    }
    _terminal = YES;
    return YES;
}

- (void)deactivate {
    std::lock_guard lock{_mutex};
    _active = NO;
    _sink.reset();
}

- (void)waitForInvalidation {
    std::unique_lock lock{_mutex};
    _invalidated.wait_for(
        lock, std::chrono::seconds{1}, [self] { return _sessionInvalidated == YES; });
}

- (void)failAfterCloseOpportunity:(NSError*)error {
    // A peer close can complete an outstanding receive with ENOTCONN before URLSession
    // delivers didCloseWithCode. Give the authoritative close delegate a chance to win.
    __weak BorealisWebSocketDelegate* weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC),
        dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            BorealisWebSocketDelegate* strongSelf = weakSelf;
            if (strongSelf == nil) {
                return;
            }
            auto sink = [strongSelf sink];
            if (sink != nullptr && [strongSelf claimTerminal]) {
                sink->closed(
                    map_error(error), to_string(error.localizedDescription), 0, 0, {});
            }
        });
}

- (void)failSendAfterCloseOpportunity:(NSError*)error bytes:(size_t)bytes {
    __weak BorealisWebSocketDelegate* weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC),
        dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            BorealisWebSocketDelegate* strongSelf = weakSelf;
            if (strongSelf == nil) {
                return;
            }
            auto sink = [strongSelf sink];
            if (sink != nullptr && [strongSelf claimTerminal]) {
                sink->send_complete(
                    bytes, map_error(error), to_string(error.localizedDescription));
            }
        });
}

- (void)receiveNext {
    NSURLSessionWebSocketTask* task = self.task;
    if (task == nil || [self sink] == nullptr) {
        return;
    }
    __weak BorealisWebSocketDelegate* weakSelf = self;
    [task receiveMessageWithCompletionHandler:^(NSURLSessionWebSocketMessage* message,
              NSError* error) {
        BorealisWebSocketDelegate* strongSelf = weakSelf;
        if (strongSelf == nil) {
            return;
        }
        auto sink = [strongSelf sink];
        if (sink == nullptr) {
            return;
        }
        if (error != nil) {
            [strongSelf failAfterCloseOpportunity:error];
            return;
        }
        @try {
            if (message.type == NSURLSessionWebSocketMessageTypeString) {
                sink->message(borealis::ws::MessageKind::Text, to_string(message.string));
            } else {
                NSData* data = message.data;
                sink->message(borealis::ws::MessageKind::Binary,
                    data != nil ? std::string{static_cast<const char*>(data.bytes), data.length} :
                                  std::string{});
            }
        } @catch (NSException* exception) {
            if ([strongSelf claimTerminal]) {
                sink->closed(borealis::ws::Error::Protocol, to_string(exception.reason), 0, 0,
                    {});
            }
            return;
        }
        [strongSelf receiveNext];
    }];
}

- (void)schedulePing:(std::chrono::milliseconds)interval {
    if (interval.count() <= 0) {
        return;
    }
    const auto nanoseconds = static_cast<int64_t>(interval.count()) * NSEC_PER_MSEC;
    __weak BorealisWebSocketDelegate* weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, nanoseconds),
        dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            BorealisWebSocketDelegate* strongSelf = weakSelf;
            if (strongSelf == nil || [strongSelf sink] == nullptr) {
                return;
            }
            [strongSelf.task sendPingWithPongReceiveHandler:^(NSError* error) {
                auto sink = [strongSelf sink];
                if (sink == nullptr) {
                    return;
                }
                if (error != nil) {
                    [strongSelf failAfterCloseOpportunity:error];
                    return;
                }
                [strongSelf schedulePing:interval];
            }];
        });
}

- (void)URLSession:(NSURLSession*)session
              task:(NSURLSessionTask*)task
    willPerformHTTPRedirection:(NSHTTPURLResponse*)response
                    newRequest:(NSURLRequest*)request
             completionHandler:(void (^)(NSURLRequest*))completionHandler {
    (void)session;
    (void)task;
    (void)response;
    (void)request;
    completionHandler(nil);
}

- (void)URLSession:(NSURLSession*)session didBecomeInvalidWithError:(NSError*)error {
    (void)session;
    (void)error;
    {
        std::lock_guard lock{_mutex};
        _sessionInvalidated = YES;
    }
    _invalidated.notify_all();
}

- (void)URLSession:(NSURLSession*)session
      webSocketTask:(NSURLSessionWebSocketTask*)webSocketTask
 didOpenWithProtocol:(NSString*)protocol {
    (void)session;
    auto sink = [self sink];
    if (sink == nullptr) {
        return;
    }
    NSHTTPURLResponse* response =
        [webSocketTask.response isKindOfClass:[NSHTTPURLResponse class]] ?
            (NSHTTPURLResponse*)webSocketTask.response :
            nil;
    sink->opened(to_string(protocol), response_headers(response));
    [self receiveNext];
}

- (void)URLSession:(NSURLSession*)session
      webSocketTask:(NSURLSessionWebSocketTask*)webSocketTask
 didCloseWithCode:(NSURLSessionWebSocketCloseCode)closeCode
            reason:(NSData*)reason {
    (void)session;
    (void)webSocketTask;
    auto sink = [self sink];
    if (sink == nullptr || ![self claimTerminal]) {
        return;
    }
    std::string reasonText;
    if (reason != nil) {
        reasonText.assign(static_cast<const char*>(reason.bytes), reason.length);
    }
    sink->closed(borealis::ws::Error::None, {}, 0, static_cast<uint16_t>(closeCode),
        std::move(reasonText));
    [self.session finishTasksAndInvalidate];
}

- (void)URLSession:(NSURLSession*)session
                    task:(NSURLSessionTask*)task
    didCompleteWithError:(NSError*)error {
    (void)session;
    auto sink = [self sink];
    if (sink == nullptr) {
        return;
    }
    NSHTTPURLResponse* response = [task.response isKindOfClass:[NSHTTPURLResponse class]] ?
                                       (NSHTTPURLResponse*)task.response :
                                       nil;
    const NSInteger status = response.statusCode;
    if (error != nil && status != 0 && status != 101 && [self claimTerminal]) {
        sink->closed(borealis::ws::Error::Handshake, to_string(error.localizedDescription),
            static_cast<int>(status), 0, {}, response_headers(response));
        return;
    }
    if ([task isKindOfClass:[NSURLSessionWebSocketTask class]]) {
        NSURLSessionWebSocketTask* webSocketTask = (NSURLSessionWebSocketTask*)task;
        if (webSocketTask.closeCode != NSURLSessionWebSocketCloseCodeInvalid &&
            [self claimTerminal])
        {
            NSData* reason = webSocketTask.closeReason;
            std::string reasonText;
            if (reason != nil) {
                reasonText.assign(static_cast<const char*>(reason.bytes), reason.length);
            }
            sink->closed(borealis::ws::Error::None, {}, 0,
                static_cast<uint16_t>(webSocketTask.closeCode), std::move(reasonText));
            return;
        }
    }
    if (error != nil) {
        [self failAfterCloseOpportunity:error];
    }
}

@end

namespace borealis::ws::detail {
namespace {

class UrlSessionTransport final : public Transport {
public:
    ~UrlSessionTransport() override { abort(); }

    void start(const Options& options, std::shared_ptr<EventSink> sink) override {
        @autoreleasepool {
            delegate = [[BorealisWebSocketDelegate alloc] init];
            [delegate prepareWithSink:std::move(sink)];

            NSURLSessionConfiguration* configuration =
                [NSURLSessionConfiguration ephemeralSessionConfiguration];
            configuration.timeoutIntervalForRequest = 7 * 24 * 60 * 60;
            configuration.timeoutIntervalForResource = 7 * 24 * 60 * 60;
            session = [NSURLSession sessionWithConfiguration:configuration
                                                    delegate:delegate
                                               delegateQueue:nil];
            delegate.session = session;

            NSString* urlText = to_nsstring(options.url);
            NSURL* url = urlText != nil ? [NSURL URLWithString:urlText] : nil;
            if (url == nil) {
                auto eventSink = [delegate sink];
                if (eventSink != nullptr && [delegate claimTerminal]) {
                    eventSink->closed(Error::InvalidUrl, "Failed to parse WebSocket URL", 0, 0,
                        {});
                }
                return;
            }
            NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:url];
            request.cachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
            request.timeoutInterval = configuration.timeoutIntervalForRequest;
            for (const auto& header : options.headers) {
                NSString* name = to_nsstring(header.name);
                NSString* value = to_nsstring(header.value);
                if (name != nil && value != nil) {
                    [request setValue:value forHTTPHeaderField:name];
                }
            }
            task = [session webSocketTaskWithRequest:request];
            task.maximumMessageSize = options.maxMessageBytes;
            delegate.task = task;
            [task resume];
            if (options.keepaliveInterval.count() != 0) {
                [delegate schedulePing:options.keepaliveInterval];
            }
        }
    }

    bool send(MessageKind kind, std::string data) override {
        NSURLSessionWebSocketTask* currentTask = task;
        BorealisWebSocketDelegate* currentDelegate = delegate;
        if (currentTask == nil || currentDelegate == nil || [currentDelegate sink] == nullptr) {
            return false;
        }
        NSURLSessionWebSocketMessage* message = nil;
        if (kind == MessageKind::Text) {
            NSString* text = to_nsstring(data);
            if (text == nil) {
                return false;
            }
            message = [[NSURLSessionWebSocketMessage alloc] initWithString:text];
        } else {
            NSData* bytes = [NSData dataWithBytes:data.data() length:data.size()];
            message = [[NSURLSessionWebSocketMessage alloc] initWithData:bytes];
        }
        const size_t size = data.size();
        [currentTask sendMessage:message completionHandler:^(NSError* error) {
            auto sink = [currentDelegate sink];
            if (sink == nullptr) {
                return;
            }
            if (error != nil) {
                [currentDelegate failSendAfterCloseOpportunity:error bytes:size];
            } else {
                sink->send_complete(size, borealis::ws::Error::None, {});
            }
        }];
        return true;
    }

    void close(uint16_t code, std::string reason) override {
        NSData* reasonData = [NSData dataWithBytes:reason.data() length:reason.size()];
        [task cancelWithCloseCode:static_cast<NSURLSessionWebSocketCloseCode>(code)
                          reason:reasonData];
    }

    void abort() noexcept override try {
        BorealisWebSocketDelegate* currentDelegate = delegate;
        if (currentDelegate == nil) {
            return;
        }
        [currentDelegate deactivate];
        [task cancel];
        [session invalidateAndCancel];
        [currentDelegate waitForInvalidation];
        currentDelegate.task = nil;
        currentDelegate.session = nil;
        task = nil;
        session = nil;
        delegate = nil;
    }
    BOREALIS_CATCH()

private:
    BorealisWebSocketDelegate* __strong delegate = nil;
    NSURLSession* __strong session = nil;
    NSURLSessionWebSocketTask* __strong task = nil;
};

}  // namespace

std::unique_ptr<Transport> make_transport() {
    return std::make_unique<UrlSessionTransport>();
}

bool backend_available() noexcept {
    return true;
}

}  // namespace borealis::ws::detail
