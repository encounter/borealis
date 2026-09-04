#pragma once

#include "borealis/http.hpp"

#import <Foundation/Foundation.h>

#include <string>
#include <string_view>
#include <vector>

namespace borealis::detail::url_session {

inline std::string to_string(NSString* value) {
    if (value == nil) {
        return {};
    }
    const char* utf8 = value.UTF8String;
    return utf8 != nullptr ? std::string{utf8} : std::string{};
}

inline NSString* to_nsstring(std::string_view value) {
    return [[NSString alloc] initWithBytes:value.data()
                                    length:value.size()
                                  encoding:NSUTF8StringEncoding];
}

inline std::vector<http::Header> response_headers(NSHTTPURLResponse* response) {
    std::vector<http::Header> result;
    if (response == nil) {
        return result;
    }
    for (id key in response.allHeaderFields) {
        id value = response.allHeaderFields[key];
        result.push_back({
            .name = to_string([key description]),
            .value = to_string([value description]),
        });
    }
    return result;
}

template <typename Error>
Error map_error(NSError* error) {
    if (error == nil) {
        return Error::None;
    }
    if (![error.domain isEqualToString:NSURLErrorDomain]) {
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

}  // namespace borealis::detail::url_session
