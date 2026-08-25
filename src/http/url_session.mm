#include "borealis/http.hpp"

#include "../http_internal.hpp"

#import <Foundation/Foundation.h>

#include <atomic>
#include <span>
#include <string_view>
#include <utility>

namespace {

std::string to_string(NSString* value) {
    if (value == nil) {
        return {};
    }

    const char* utf8 = [value UTF8String];
    return utf8 == nullptr ? std::string{} : std::string{utf8};
}

std::vector<borealis::http::Header> to_headers(NSHTTPURLResponse* response) {
    std::vector<borealis::http::Header> result;
    NSDictionary* headers = response.allHeaderFields;
    for (id key in headers) {
        id value = headers[key];
        result.push_back({
            .name = to_string([key description]),
            .value = to_string([value description]),
        });
    }
    return result;
}

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
    if (self.redirectCount <= 5 && isHttps) {
        completionHandler(request);
    } else {
        self.redirectDowngrade = !isHttps;
        self.tooManyRedirects = self.redirectCount > 5;
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
                static_cast<int>(httpResponse.statusCode), to_headers(httpResponse)) ==
            borealis::http::detail::TransportObserver::Directive::Abort)
        {
            completionHandler(NSURLSessionResponseCancel);
            return;
        }
    } catch (...) {
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
    } catch (...) {
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

NSString* to_nsstring(std::string_view value) {
    return [[NSString alloc] initWithBytes:value.data()
                                    length:value.size()
                                  encoding:NSUTF8StringEncoding];
}

Error map_nsurl_error(NSError* error) {
    if (error == nil || ![error.domain isEqualToString:NSURLErrorDomain]) {
        return Error::Network;
    }

    switch (error.code) {
    case NSURLErrorTimedOut:
        return Error::Timeout;
    case NSURLErrorBadURL:
    case NSURLErrorUnsupportedURL:
        return Error::InvalidUrl;
    default:
        return Error::Network;
    }
}

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
        urlRequest.HTTPMethod = request.method == Method::Post ? @"POST" : @"GET";
        urlRequest.cachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
        if (request.method == Method::Post && !request.body.empty()) {
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
                .error = map_nsurl_error(delegate.error),
                .message = to_string(delegate.error.localizedDescription),
            };
        }
        return {};
    }
}

}  // namespace borealis::http
