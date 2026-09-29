# Checks whether the system libcurl is usable by borealis. This runs in its own directory so the
# imported CURL::libcurl target from FindCURL doesn't leak into borealis if we vendor libcurl instead.
find_package(CURL 7.47 QUIET OPTIONAL_COMPONENTS HTTPS SSL WSS)
set(_usable FALSE)
if (CURL_FOUND AND CURL_HTTPS_FOUND AND CURL_SSL_FOUND)
    if (NOT BOREALIS_ENABLE_WEBSOCKET OR CURL_WSS_FOUND)
        set(_usable TRUE)
    endif ()
endif ()
set(BOREALIS_SYSTEM_CURL_USABLE ${_usable} PARENT_SCOPE)
set(BOREALIS_SYSTEM_CURL_VERSION "${CURL_VERSION_STRING}" PARENT_SCOPE)
