cmake_minimum_required(VERSION 3.15)
project(claude_chokidar LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SOURCES
    src/safety.cpp
    src/config.cpp
    src/chrome_finder.cpp
    src/net.cpp
    src/cdp.cpp
    src/health_server.cpp
    src/bot.cpp
)

set(HEADERS
    src/common.hpp
    src/json.hpp
    src/safety.hpp
    src/config.hpp
    src/chrome_finder.hpp
    src/net.hpp
    src/cdp.hpp
    src/health_server.hpp
    src/bot.hpp
)

add_executable(claude-chokidar src/main.cpp ${SOURCES} ${HEADERS})
target_include_directories(claude-chokidar PRIVATE src)

find_package(Threads REQUIRED)
target_link_libraries(claude-chokidar PRIVATE Threads::Threads)

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR NOT CMAKE_BUILD_TYPE)
    target_compile_options(claude-chokidar PRIVATE -O3)
endif()

enable_testing()
add_executable(test_runner tests/test_main.cpp ${SOURCES} ${HEADERS})
target_include_directories(test_runner PRIVATE src)
target_link_libraries(test_runner PRIVATE Threads::Threads)
add_test(NAME AllTests COMMAND test_runner)
