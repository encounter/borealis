#include "borealis/url.hpp"

#include <gtest/gtest.h>

namespace borealis::url {
namespace {

TEST(UrlTest, ParsesAndNormalizesNetworkUrls) {
    const auto https = parse("HTTPS://Example.COM:443/path?query");
    ASSERT_TRUE(https);
    EXPECT_EQ(https->scheme, "https");
    EXPECT_EQ(https->host, "example.com");
    EXPECT_EQ(https->port, 443);
    EXPECT_FALSE(https->ipv6);
    EXPECT_TRUE(https->hasResource);

    const auto ipv6 = parse("wss://[::1]:8443/socket");
    ASSERT_TRUE(ipv6);
    EXPECT_EQ(ipv6->host, "::1");
    EXPECT_EQ(ipv6->port, 8443);
    EXPECT_TRUE(ipv6->ipv6);
    EXPECT_TRUE(ipv6->hasResource);

    const auto endpoint = parse("tcp://localhost:80");
    ASSERT_TRUE(endpoint);
    EXPECT_FALSE(endpoint->hasResource);
}

TEST(UrlTest, RejectsMalformedOrCredentialBearingUrls) {
    constexpr std::string_view Invalid[]{
        "",
        "https://",
        "1https://example.com",
        "https://user@example.com",
        "https://example.com:",
        "https://example.com:not-a-port",
        "https://example.com:65536",
        "https://::1/path",
        "https://example.com/path#fragment",
        "https://example.com\\path",
    };
    for (const auto value : Invalid) {
        EXPECT_FALSE(parse(value)) << value;
    }
}

}  // namespace
}  // namespace borealis::url
