#include "borealis/http.hpp"

#include "url_session_common.hpp"
#include "../http_internal.hpp"
#include "borealis/log.hpp"

#import <Foundation/Foundation.h>

#include <atomic>
#include <span>
#include <string_view>
#include <utility>

namespace {

using borealis::detail::url_session::response_headers;
using borealis::detail::url_session::to_string;

constexpr borealis::Log Log{"borealis::http"};

}  // namespace

@interface BorealisHttpRequestDelegate : NSObject <NSURLSessionDataDelegate, NSURLSessionTaskDelegate> {
@public
    borealis::http::detail::TransportObserver* _observer;
    borealis::http::detail::Deadline::Tracker _activity;
}
@property(nonatomic) dispatch_semaphore_t semaphore;
@property(nonatomic) borealis::detail::TaskSignals* signals;
@property(nonatomic, strong) NSError* error;
@property(nonatomic) NSUInteger redirectCount;
@property(nonatomic) BOOL redirectDowngrade;
@property(nonatomic) BOOL tooManyRedirects;
@property(nonatomic) BOOL callbackFailed;
@property(nonatomic) BOOL timedOut;
- (void)prepareWithObserver:(borealis::http::detail::TransportObserver*)observer
                    signals:(borealis::detail::TaskSignals*)signals;
@end

@implementation BorealisHttpRequestDelegate

- (void)prepareWithObserver:(borealis::http::detail::TransportObserver*)observer
                    signals:(borealis::detail::TaskSignals*)signals {
    self.semaphore = dispatch_semaphore_create(0);
    self.signals = signals;
    self.error = nil;
    self.redirectCount = 0;
    self.redirectDowngrade = NO;
    self.tooManyRedirects = NO;
    self.callbackFailed = NO;
    self.timedOut = NO;
    _observer = observer;
    _activity.reset();
}

- (void)URLSession:(NSURLSession*)session
              task:(NSURLSessionTask*)task
    willPerformHTTPRedirection:(NSHTTPURLResponse*)response
                    newRequest:(NSURLRequest*)request
             completionHandler:(void (^)(NSURLRequest*))completionHandler {
    ++self.redirectCount;
    const BOOL isHttps = [[request.URL.scheme lowercaseString] isEqualToString:@"https"];
    if (self.redirectCount <= 20 && isHttps) {
        completionHandler(request);
    } else {
        self.redirectDowngrade = !isHttps;
        self.tooManyRedirects = self.redirectCount > 20;
        completionHandler(nil);
    }
}

- (void)URLSession:(NSURLSession*)session
          dataTask:(NSURLSessionDataTask*)dataTask
didReceiveResponse:(NSURLResponse*)response
 completionHandler:(void (^)(NSURLSessionResponseDisposition disposition))completionHandler {
    _activity.start_response();
    if (![response isKindOfClass:[NSHTTPURLResponse class]]) {
        self.callbackFailed = YES;
        completionHandler(NSURLSessionResponseCancel);
        return;
    }

    try {
        NSHTTPURLResponse* httpResponse = (NSHTTPURLResponse*)response;
        if (_observer->on_response(
                static_cast<int>(httpResponse.statusCode), response_headers(httpResponse)) ==
            borealis::http::detail::TransportObserver::Directive::Abort)
        {
            completionHandler(NSURLSessionResponseCancel);
            return;
        }
    } catch (const std::exception& exception) {
        Log.error("{}: {}", __func__, exception.what());
        self.callbackFailed = YES;
        completionHandler(NSURLSessionResponseCancel);
        return;
    } catch (...) {
        Log.error("{}: unknown exception", __func__);
        self.callbackFailed = YES;
        completionHandler(NSURLSessionResponseCancel);
        return;
    }
    completionHandler(NSURLSessionResponseAllow);
}

- (void)URLSession:(NSURLSession*)session
          dataTask:(NSURLSessionDataTask*)dataTask
    didReceiveData:(NSData*)data {
    if (self.signals->cancelRequested.load(std::memory_order_relaxed)) {
        [dataTask cancel];
        return;
    }

    try {
        const auto chunk = std::span{
            static_cast<const std::byte*>(data.bytes), static_cast<size_t>(data.length)};
        if (_observer->on_data(chunk) ==
            borealis::http::detail::TransportObserver::Directive::Abort)
        {
            [dataTask cancel];
            return;
        }
    } catch (const std::exception& exception) {
        Log.error("{}: {}", __func__, exception.what());
        self.callbackFailed = YES;
        [dataTask cancel];
        return;
    } catch (...) {
        Log.error("{}: unknown exception", __func__);
        self.callbackFailed = YES;
        [dataTask cancel];
        return;
    }
    _activity.touch();
}

- (void)URLSession:(NSURLSession*)session
                    task:(NSURLSessionTask*)task
    didCompleteWithError:(NSError*)error {
    if (error != nil) {
        self.error = error;
    }
    dispatch_semaphore_signal(self.semaphore);
}

@end

namespace borealis::http {
namespace {

using borealis::detail::url_session::to_nsstring;
using borealis::detail::url_session::to_string;

struct SessionState {
    BorealisHttpRequestDelegate* __strong delegate;
    NSURLSession* __strong session;

    SessionState() {
        delegate = [[BorealisHttpRequestDelegate alloc] init];
        NSURLSessionConfiguration* configuration =
            [NSURLSessionConfiguration ephemeralSessionConfiguration];
        configuration.timeoutIntervalForRequest = 7 * 24 * 60 * 60;
        configuration.timeoutIntervalForResource = 7 * 24 * 60 * 60;
        session = [NSURLSession sessionWithConfiguration:configuration
                                                delegate:delegate
                                           delegateQueue:nil];
    }

    ~SessionState() {
        [session invalidateAndCancel];
    }
};

SessionState& session_state() {
    static thread_local SessionState state;
    return state;
}

void wait_for_request(NSURLSessionTask* task, BorealisHttpRequestDelegate* delegate,
    detail::Deadline& deadline) {
    for (;;) {
        const dispatch_time_t waitDeadline = dispatch_time(DISPATCH_TIME_NOW,
            static_cast<int64_t>(detail::Deadline::PollInterval.count()) *
                static_cast<int64_t>(NSEC_PER_MSEC));
        if (dispatch_semaphore_wait(delegate.semaphore, waitDeadline) == 0) {
            return;
        }
        if (delegate.signals->cancelRequested.load(std::memory_order_relaxed)) {
            [task cancel];
            continue;
        }

        if (deadline.expired(delegate->_activity)) {
            delegate.timedOut = YES;
            [task cancel];
        }
    }
}

}  // namespace

bool available() noexcept {
    return true;
}

Backend backend() noexcept {
    return Backend::UrlSession;
}

const char* backend_name() noexcept {
    return "NSURLSession";
}

detail::TransportResult detail::send_request(const TransportRequest& request) {
    @autoreleasepool {
        NSString* urlString = to_nsstring(request.url);
        if (urlString == nil) {
            return {
                .error = Error::InvalidUrl,
                .message = "URL is not valid UTF-8",
            };
        }

        NSURL* url = [NSURL URLWithString:urlString];
        if (url == nil || ![[url.scheme lowercaseString] isEqualToString:@"https"]) {
            return {
                .error = Error::InvalidUrl,
                .message = "Failed to parse URL",
            };
        }

        NSMutableURLRequest* urlRequest = [NSMutableURLRequest requestWithURL:url];
        urlRequest.HTTPMethod = to_nsstring(detail::method_name(request.method));
        urlRequest.cachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
        if (detail::method_has_request_body(request.method) && !request.body.empty()) {
            urlRequest.HTTPBody = [NSData dataWithBytes:request.body.data()
                                               length:request.body.size()];
        }
        for (const Header& header : request.headers) {
            NSString* name = to_nsstring(header.name);
            NSString* value = to_nsstring(header.value);
            if (name == nil || value == nil) {
                return {
                    .error = Error::InvalidUrl,
                    .message = "Request header is not valid UTF-8",
                };
            }
            [urlRequest setValue:value forHTTPHeaderField:name];
        }

        SessionState& sessionState = session_state();
        BorealisHttpRequestDelegate* delegate = sessionState.delegate;
        [delegate prepareWithObserver:request.observer signals:request.signals];
        NSURLSessionDataTask* task = [sessionState.session dataTaskWithRequest:urlRequest];
        [task resume];
        wait_for_request(task, delegate, *request.deadline);

        if (request.signals->cancelRequested.load(std::memory_order_relaxed)) {
            return {
                .error = Error::Canceled,
                .message = "Request canceled",
            };
        }
        if (delegate.timedOut) {
            return {
                .error = Error::Timeout,
                .message = "Request timed out",
            };
        }
        if (delegate.redirectDowngrade) {
            return {
                .error = Error::UnsupportedScheme,
                .message = "Only https:// redirects are supported",
            };
        }
        if (delegate.tooManyRedirects) {
            return {
                .error = Error::Network,
                .message = "Too many redirects",
            };
        }
        if (delegate.callbackFailed) {
            return {
                .error = Error::Network,
                .message = "HTTP response callback failed",
            };
        }
        if (delegate.error != nil) {
            return {
                .error = borealis::detail::url_session::map_error<Error>(delegate.error),
                .message = to_string(delegate.error.localizedDescription),
            };
        }
        return {};
    }
}

}  // namespace borealis::http
