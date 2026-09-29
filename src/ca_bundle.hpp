#pragma once

#include <cstdlib>
#include <string>

#include <sys/stat.h>

namespace borealis::detail {

inline bool is_readable_file(const char* path) {
    struct stat info{};
    return path != nullptr && *path != '\0' && ::stat(path, &info) == 0 && S_ISREG(info.st_mode);
}

// Well-known CA bundle locations, as searched by Go's crypto/x509.
inline constexpr const char* CaBundlePaths[] = {
    "/etc/ssl/certs/ca-certificates.crt",                 // Debian, Ubuntu, Arch, Gentoo
    "/etc/pki/tls/certs/ca-bundle.crt",                   // Fedora, RHEL
    "/etc/ssl/ca-bundle.pem",                             // openSUSE
    "/etc/pki/tls/cacert.pem",                            // OpenELEC
    "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",  // CentOS, RHEL 7
    "/etc/ssl/cert.pem",                                  // Alpine, BSDs
};

// Locates the system CA bundle. A libcurl/OpenSSL built on one distro (e.g. the Ubuntu libraries
// bundled into an AppImage) hardcodes that distro's path, which does not exist on others.
// Returns an empty string if no bundle was found.
inline const std::string& system_ca_bundle() {
    static const std::string bundle = [] {
        if (const char* file = std::getenv("SSL_CERT_FILE"); is_readable_file(file)) {
            return std::string{file};
        }
        for (const char* candidate : CaBundlePaths) {
            if (is_readable_file(candidate)) {
                return std::string{candidate};
            }
        }
        return std::string{};
    }();
    return bundle;
}

}  // namespace borealis::detail
