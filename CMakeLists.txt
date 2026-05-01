cmake_minimum_required(VERSION 3.22)
project(trade_bot
    VERSION 0.0.1
    DESCRIPTION "Trade Bot — hardcore C++23 implementation"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_TESTS "Build unit tests" ON)
option(ENABLE_AVX2 "Enable AVX2 optimizations" ON)
option(ENABLE_SANITIZERS "Enable AddressSanitizer + UndefinedBehaviorSanitizer in Debug" ON)

# Strict warnings
if(MSVC)
    add_compile_options(/W4 /WX /permissive- /utf-8)
    if(ENABLE_AVX2)
        add_compile_options(/arch:AVX2)
    endif()
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Werror)
    if(ENABLE_AVX2)
        add_compile_options(-mavx2)
    endif()
    if(ENABLE_SANITIZERS AND CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

# Dependencies
find_package(spdlog REQUIRED CONFIG)
find_package(hdr_histogram REQUIRED CONFIG)
find_package(xsimd REQUIRED CONFIG)
find_package(concurrentqueue REQUIRED CONFIG)
find_package(benchmark REQUIRED CONFIG)
find_package(Boost REQUIRED CONFIG)
find_package(tomlplusplus REQUIRED CONFIG)
find_package(CURL REQUIRED CONFIG)
find_package(nlohmann_json REQUIRED CONFIG)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Main executable (T0-SETUP: just prints version and exits)
add_executable(trade_bot 
    src/main.cpp 
    src/logger/Logger.cpp
    src/perf/CpuPinning.cpp
    src/perf/SimdOps.cpp
    src/config/Config.cpp
    src/transport/CurlHttpClient.cpp
    src/transport/MetaScalpDiscovery.cpp
    src/transport/BeastWsClient.cpp
    src/transport/MetaScalpCodec.cpp
    src/transport/MarketDataFeed.cpp
    src/transport/OrderGateway.cpp
)
target_include_directories(trade_bot PRIVATE src)
target_link_libraries(trade_bot PRIVATE 
    spdlog::spdlog 
    xsimd 
    hdr_histogram::hdr_histogram_static 
    boost::boost 
    concurrentqueue::concurrentqueue 
    tomlplusplus::tomlplusplus
    CURL::libcurl
    nlohmann_json::nlohmann_json
)

# Tests
if(BUILD_TESTS)
    enable_testing()
    find_package(GTest REQUIRED CONFIG)
    
    add_executable(smoke_test test/unit/smoke_test.cpp)
    target_link_libraries(smoke_test PRIVATE GTest::gtest GTest::gtest_main)
    add_test(NAME smoke COMMAND smoke_test)

    add_executable(logger_test 
        test/unit/logger_test.cpp 
        src/logger/Logger.cpp
    )
    target_include_directories(logger_test PRIVATE src)
    target_link_libraries(logger_test PRIVATE GTest::gtest GTest::gtest_main spdlog::spdlog)
    add_test(NAME logger COMMAND logger_test)

    add_executable(numeric_test 
        test/unit/numeric_test.cpp
    )
    target_include_directories(numeric_test PRIVATE src)
    target_link_libraries(numeric_test PRIVATE GTest::gtest GTest::gtest_main hdr_histogram::hdr_histogram_static)
    add_test(NAME numeric COMMAND numeric_test)

    add_executable(perf_test 
        test/unit/perf_test.cpp
        src/perf/CpuPinning.cpp
        src/perf/SimdOps.cpp
        src/logger/Logger.cpp
    )
    target_include_directories(perf_test PRIVATE src)
    target_link_libraries(perf_test PRIVATE GTest::gtest GTest::gtest_main spdlog::spdlog xsimd boost::boost concurrentqueue::concurrentqueue)
    add_test(NAME perf COMMAND perf_test)

    add_executable(config_test 
        test/unit/config_test.cpp
        src/config/Config.cpp
        src/logger/Logger.cpp
    )
    target_include_directories(config_test PRIVATE src)
    target_link_libraries(config_test PRIVATE GTest::gtest GTest::gtest_main spdlog::spdlog tomlplusplus::tomlplusplus)
    add_test(NAME config COMMAND config_test)

    add_executable(http_client_test 
        test/unit/http_client_test.cpp
        src/transport/CurlHttpClient.cpp
        src/logger/Logger.cpp
    )
    target_include_directories(http_client_test PRIVATE src)
    target_link_libraries(http_client_test PRIVATE GTest::gtest GTest::gtest_main spdlog::spdlog CURL::libcurl boost::boost)
    add_test(NAME http_client COMMAND http_client_test)

    add_executable(discover_test 
        test/unit/discover_test.cpp
        src/transport/MetaScalpDiscovery.cpp
        src/transport/CurlHttpClient.cpp
        src/logger/Logger.cpp
    )
    target_include_directories(discover_test PRIVATE src)
    target_link_libraries(discover_test PRIVATE GTest::gtest GTest::gtest_main spdlog::spdlog CURL::libcurl nlohmann_json::nlohmann_json boost::boost)
    add_test(NAME discover COMMAND discover_test)

    add_executable(domain_test 
        test/unit/domain_test.cpp
    )
    target_include_directories(domain_test PRIVATE src)
    target_link_libraries(domain_test PRIVATE GTest::gtest GTest::gtest_main)
    add_test(NAME domain COMMAND domain_test)

    add_executable(ws_test 
        test/unit/ws_test.cpp
        src/transport/BeastWsClient.cpp
        src/logger/Logger.cpp
    )
    target_include_directories(ws_test PRIVATE src)
    target_link_libraries(ws_test PRIVATE GTest::gtest GTest::gtest_main spdlog::spdlog boost::boost nlohmann_json::nlohmann_json)
    add_test(NAME ws COMMAND ws_test)

    add_executable(ws_smoke_test 
        test/integration/ws_smoke_test.cpp
        src/transport/BeastWsClient.cpp
        src/logger/Logger.cpp
    )
    target_include_directories(ws_smoke_test PRIVATE src)
    target_link_libraries(ws_smoke_test PRIVATE GTest::gtest GTest::gtest_main spdlog::spdlog boost::boost nlohmann_json::nlohmann_json)
    add_test(NAME ws_smoke COMMAND ws_smoke_test)

    add_executable(codec_test 
        test/unit/codec_test.cpp
        src/transport/MetaScalpCodec.cpp
        src/logger/Logger.cpp
    )
    target_include_directories(codec_test PRIVATE src)
    target_link_libraries(codec_test PRIVATE GTest::gtest GTest::gtest_main spdlog::spdlog nlohmann_json::nlohmann_json)
    add_test(NAME codec COMMAND codec_test)

    add_executable(feed_test 
        test/unit/feed_test.cpp
        src/transport/MarketDataFeed.cpp
        src/transport/MetaScalpCodec.cpp
        src/logger/Logger.cpp
    )
    target_include_directories(feed_test PRIVATE src)
    target_link_libraries(feed_test PRIVATE GTest::gtest GTest::gtest_main spdlog::spdlog nlohmann_json::nlohmann_json boost::boost)
    add_test(NAME feed COMMAND feed_test)

    add_executable(feed_smoke_test 
        test/integration/feed_smoke_test.cpp
        src/transport/MarketDataFeed.cpp
        src/transport/BeastWsClient.cpp
        src/transport/MetaScalpCodec.cpp
        src/logger/Logger.cpp
    )
    target_include_directories(feed_smoke_test PRIVATE src)
    target_link_libraries(feed_smoke_test PRIVATE GTest::gtest GTest::gtest_main spdlog::spdlog boost::boost nlohmann_json::nlohmann_json)
    add_test(NAME feed_smoke COMMAND feed_smoke_test)

    add_executable(order_gateway_test 
        test/unit/order_gateway_test.cpp
        src/transport/OrderGateway.cpp
        src/transport/MetaScalpCodec.cpp
        src/logger/Logger.cpp
    )
    target_include_directories(order_gateway_test PRIVATE src)
    target_link_libraries(order_gateway_test PRIVATE GTest::gtest GTest::gtest_main spdlog::spdlog nlohmann_json::nlohmann_json boost::boost)
    add_test(NAME order_gateway COMMAND order_gateway_test)

    # Benchmarks
    add_executable(bench_perf 
        bench/bench_perf.cpp
        src/perf/SimdOps.cpp
    )
    target_include_directories(bench_perf PRIVATE src)
    target_link_libraries(bench_perf PRIVATE benchmark::benchmark xsimd)
endif()

message(STATUS "Project: ${PROJECT_NAME} v${PROJECT_VERSION}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "Compiler:     ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "Build type:   ${CMAKE_BUILD_TYPE}")
message(STATUS "AVX2:         ${ENABLE_AVX2}")
message(STATUS "Sanitizers:   ${ENABLE_SANITIZERS}")
