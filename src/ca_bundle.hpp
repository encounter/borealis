#pragma once

#include <cstdlib>
#include <string>

#include <sys/stat.h>

namespace borealis::detail {

inline bool file_exists(const char* path) {
    struct stat info{};
    return path != nullptr && *path != '\0' && ::stat(path, &info) == 0 && S_ISREG(info.st_mode);
}

// https://github.com/golang/go/blob/6446eeb686f1f35c0260ca9b4f6323b809249fbe/src/crypto/x509/root_linux.go#L10
inline constexpr const char* kCaBundlePaths[] = {
    "/etc/ssl/certs/ca-certificates.crt",                 // Debian, Ubuntu, Arch, Gentoo
    "/etc/pki/tls/certs/ca-bundle.crt",                   // Fedora, RHEL
    "/etc/ssl/ca-bundle.pem",                             // openSUSE
    "/etc/pki/tls/cacert.pem",                            // OpenELEC
    "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",  // CentOS, RHEL 7
    "/etc/ssl/cert.pem",                                  // Alpine, BSDs
};

// Locates the system CA bundle using SSL_CERT_FILE and common paths.
inline const std::string& system_ca_bundle() {
    static const std::string bundle = [] {
        if (const char* file = std::getenv("SSL_CERT_FILE"); file_exists(file)) {
            return std::string{file};
        }
        for (const char* candidate : kCaBundlePaths) {
            if (file_exists(candidate)) {
                return std::string{candidate};
            }
        }
        return std::string{};
    }();
    return bundle;
}

}  // namespace borealis::detail
