include(FetchContent)
include(GoogleTest)

if (NOT TARGET gtest)
    FetchContent_Declare(googletest
            URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
            URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
            DOWNLOAD_EXTRACT_TIMESTAMP FALSE
            EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(googletest)
endif ()

function(borealis_add_gtest target)
    target_link_libraries(${target} PRIVATE gtest gtest_main)
    gtest_discover_tests(${target} DISCOVERY_MODE PRE_TEST)
endfunction()

add_executable(borealis_version_test version_test.cpp)
target_link_libraries(borealis_version_test PRIVATE borealis::core)
borealis_add_gtest(borealis_version_test)

add_executable(borealis_task_test task_test.cpp)
target_link_libraries(borealis_task_test PRIVATE borealis::task)
borealis_add_gtest(borealis_task_test)

add_executable(borealis_url_test url_test.cpp)
target_link_libraries(borealis_url_test PRIVATE borealis::url)
borealis_add_gtest(borealis_url_test)

add_executable(borealis_presentation_test presentation_test.cpp)
target_link_libraries(borealis_presentation_test PRIVATE borealis::presentation)
borealis_add_gtest(borealis_presentation_test)

add_executable(borealis_log_test log_test.cpp)
target_link_libraries(borealis_log_test PRIVATE borealis::log)
borealis_add_gtest(borealis_log_test)

if (TARGET borealis_io)
    add_executable(borealis_io_test io_test.cpp)
    target_link_libraries(borealis_io_test PRIVATE borealis::io)
    borealis_add_gtest(borealis_io_test)
endif ()

if (TARGET borealis_file_select)
    add_executable(borealis_file_select_test file_select_test.cpp)
    target_include_directories(borealis_file_select_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(borealis_file_select_test PRIVATE borealis::file_select)
    borealis_add_gtest(borealis_file_select_test)
endif ()

if (TARGET borealis_data)
    add_executable(borealis_data_test data_test.cpp)
    target_include_directories(borealis_data_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(borealis_data_test PRIVATE
            borealis::data nlohmann_json::nlohmann_json)
    borealis_add_gtest(borealis_data_test)
endif ()

if (TARGET borealis_disc)
    add_executable(borealis_disc_test disc_test.cpp)
    target_include_directories(borealis_disc_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(borealis_disc_test PRIVATE
            borealis::disc ${_borealis_nod_target} ${_borealis_sdl_target})
    borealis_add_gtest(borealis_disc_test)
endif ()

if (TARGET borealis_crash)
    add_executable(borealis_crash_test crash_test.cpp)
    target_include_directories(borealis_crash_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(borealis_crash_test PRIVATE borealis::crash)
    borealis_add_gtest(borealis_crash_test)
endif ()

add_executable(borealis_sentry_test sentry_test.cpp)
target_include_directories(borealis_sentry_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(borealis_sentry_test PRIVATE borealis::sentry)
borealis_add_gtest(borealis_sentry_test)

if (BOREALIS_ENABLE_DISCORD)
    add_executable(borealis_discord_test discord_test.cpp)
    target_include_directories(borealis_discord_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(borealis_discord_test PRIVATE
            borealis::discord nlohmann_json::nlohmann_json)
    borealis_add_gtest(borealis_discord_test)
endif ()

if (TARGET borealis_http)
    add_executable(borealis_http_test http_test.cpp)
    target_include_directories(borealis_http_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(borealis_http_test PRIVATE borealis::http borealis_json_dep)
    borealis_add_gtest(borealis_http_test)
endif ()

if (TARGET borealis_net)
    add_executable(borealis_net_test net_test.cpp)
    target_link_libraries(borealis_net_test PRIVATE borealis::net)
    borealis_add_gtest(borealis_net_test)
endif ()

if (TARGET borealis_ws)
    add_executable(borealis_ws_test ws_test.cpp)
    target_include_directories(borealis_ws_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(borealis_ws_test PRIVATE borealis::ws borealis::task)
    borealis_add_gtest(borealis_ws_test)

    add_executable(borealis_ws_backend_test ws_backend_test.cpp)
    target_link_libraries(borealis_ws_backend_test PRIVATE borealis::ws)
    borealis_add_gtest(borealis_ws_backend_test)
endif ()

if (TARGET borealis_update)
    add_executable(borealis_update_test update_test.cpp)
    target_include_directories(borealis_update_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(borealis_update_test PRIVATE borealis::update)
    target_compile_definitions(borealis_update_test PRIVATE
            BOREALIS_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
    borealis_add_gtest(borealis_update_test)
endif ()

if (BOREALIS_ENABLE_CLI)
    add_executable(borealis_cli_test cli_test.cpp)
    target_link_libraries(borealis_cli_test PRIVATE borealis::cli)
    borealis_add_gtest(borealis_cli_test)
endif ()
