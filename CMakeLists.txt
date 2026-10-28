cmake_minimum_required(VERSION 3.20)
project(trader LANGUAGES CXX)

find_package(Boost 1.74 REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

add_executable(trader
    src/analyzer.cpp
    src/business_logic.cpp
    src/kite_broker.cpp
    src/main.cpp
    src/paper_broker.cpp
    src/scheduler.cpp
    src/text_protocol.cpp
    src/trader.cpp
    src/websocket_server.cpp
)

target_include_directories(trader PRIVATE include ${Boost_INCLUDE_DIRS})
target_compile_features(trader PRIVATE cxx_std_20)
target_compile_definitions(trader PRIVATE BOOST_ERROR_CODE_HEADER_ONLY)
target_link_libraries(trader PRIVATE Threads::Threads OpenSSL::SSL OpenSSL::Crypto)
target_compile_options(trader PRIVATE -Wall -Wextra -Wpedantic)

include(CTest)
if(BUILD_TESTING)
    add_executable(trader_tests
        src/analyzer.cpp
        tests/test_trader.cpp
        src/business_logic.cpp
        src/text_protocol.cpp
        src/trader.cpp
    )
    target_include_directories(trader_tests PRIVATE include)
    target_compile_features(trader_tests PRIVATE cxx_std_20)
    target_link_libraries(trader_tests PRIVATE Threads::Threads)
    target_compile_options(trader_tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME trader_tests COMMAND trader_tests)
endif()