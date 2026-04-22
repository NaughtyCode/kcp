cmake_minimum_required(VERSION 3.10)
project(kcp_demo LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# KCP library
add_library(kcp_library STATIC ${CMAKE_CURRENT_SOURCE_DIR}/../ikcp.c)
target_include_directories(kcp_library PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Server
add_executable(kcp_server
    server.cpp
    kcp_endpoint.cpp
    network.cpp
)
target_link_libraries(kcp_server PRIVATE kcp_library)

# Client
add_executable(kcp_client
    client.cpp
    kcp_endpoint.cpp
    network.cpp
)
target_link_libraries(kcp_client PRIVATE kcp_library)

# On Windows, link Winsock
if(WIN32)
    target_link_libraries(kcp_server PRIVATE ws2_32)
    target_link_libraries(kcp_client PRIVATE ws2_32)
endif()

# Compiler warnings
if(MSVC)
    target_compile_options(kcp_server PRIVATE /W4)
    target_compile_options(kcp_client PRIVATE /W4)
else()
    target_compile_options(kcp_server PRIVATE -Wall -Wextra -pedantic)
    target_compile_options(kcp_client PRIVATE -Wall -Wextra -pedantic)
endif()
