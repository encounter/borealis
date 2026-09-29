# Builds a static libcurl (OpenSSL, HTTP(S) and WebSockets only) and provides CURL::libcurl.
function(borealis_vendor_curl)
    # curl's cmake_minimum_required predates CMP0126; without it, curl's set(CACHE) calls would
    # discard the plain variables below.
    set(CMAKE_POLICY_DEFAULT_CMP0126 NEW)

    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_STATIC_LIBS ON)
    set(BUILD_CURL_EXE OFF)
    set(BUILD_TESTING OFF)
    set(BUILD_EXAMPLES OFF)
    set(BUILD_LIBCURL_DOCS OFF)
    set(BUILD_MISC_DOCS OFF)
    set(ENABLE_CURL_MANUAL OFF)
    set(CURL_DISABLE_INSTALL ON)
    set(CURL_ENABLE_EXPORT_TARGET OFF)

    set(CURL_USE_OPENSSL ON)
    set(CURL_ZLIB ON)
    set(CURL_BROTLI OFF)
    set(CURL_ZSTD OFF)
    set(CURL_USE_LIBPSL OFF)
    set(CURL_USE_LIBSSH2 OFF)
    set(CURL_USE_LIBSSH OFF)
    set(CURL_USE_GSSAPI OFF)
    set(USE_LIBIDN2 OFF)
    set(USE_NGHTTP2 OFF)
    set(USE_LIBRTMP OFF)
    foreach (_protocol DICT FILE FTP GOPHER IMAP IPFS LDAP LDAPS MQTT POP3 RTSP SMB SMTP TELNET TFTP)
        set(CURL_DISABLE_${_protocol} ON)
    endforeach ()

    # Don't bake in the build machine's CA paths; borealis locates the system bundle at runtime
    # (see src/ca_bundle.hpp), and anything else linking this libcurl falls back to OpenSSL's
    # default paths, which honor SSL_CERT_FILE and SSL_CERT_DIR. These are cache entries because
    # curl's config header reads them after unsetting the cache entry for "none".
    set(CURL_CA_BUNDLE "none" CACHE STRING "")
    set(CURL_CA_PATH "none" CACHE STRING "")
    set(CURL_CA_FALLBACK ON CACHE BOOL "")

    include(FetchContent)
    FetchContent_Declare(borealis_curl
            URL https://github.com/curl/curl/releases/download/curl-8_22_0/curl-8.22.0.tar.xz
            URL_HASH SHA256=f7ef3ae8a22e521f289803fe93543eb64c329b58aa73a9e224dfd915a2a5f4f7
            DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    )
    FetchContent_MakeAvailable(borealis_curl)
    if (NOT TARGET CURL::libcurl)
        message(FATAL_ERROR "borealis: vendored libcurl did not provide target 'CURL::libcurl'")
    endif ()
endfunction()
