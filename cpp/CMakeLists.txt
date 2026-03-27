cmake_minimum_required(VERSION 3.16)
project(polymarket_arb LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

add_executable(arb
    src/main.cpp
    src/polymarket.cpp
    src/kalshi.cpp
    src/matcher.cpp
    src/arb.cpp
    src/display.cpp
)

target_include_directories(arb PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

# Link against system libcurl directly (headers bundled in include/curl/)
target_link_libraries(arb PRIVATE
    /usr/lib64/libcurl.so.4
    Threads::Threads
)
