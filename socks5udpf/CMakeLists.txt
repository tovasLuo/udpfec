cmake_minimum_required(VERSION 3.16)

project(socks5udps LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

set(SOCKS5UDPS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(SOCKS5UDPS_LIB_DIR "${SOCKS5UDPS_ROOT}/lib")
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(SOCKS5UDPS_PLATFORM_DIR "x64")
else()
    set(SOCKS5UDPS_PLATFORM_DIR "Win32")
endif()

add_executable(socks5udps
    src/fec.cpp
    src/main.cpp
    src/pair_session_mgr.cpp
    src/sync_auth_info.cpp
    src/slide_win.cpp
    utils/cli_business.cpp
    utils/epoll_wrap.cpp
    utils/file_rwlock.cpp
    utils/libcli.c
    utils/socket_helper.cpp
)

target_include_directories(socks5udps PRIVATE
    "${SOCKS5UDPS_ROOT}/utils"
    "${SOCKS5UDPS_ROOT}/inc"
    "${SOCKS5UDPS_ROOT}/src"
)

target_compile_definitions(socks5udps PRIVATE
    _GNU_SOURCE
    LIBCLI_USE_POLL
    UDP_NONBLOCK
    UDP_RECV_SEND_MSG
)

target_compile_options(socks5udps PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:-fpermissive>"
    -Wall
    -Wno-sign-conversion
    -Wno-conversion
)

find_package(Threads REQUIRED)

add_library(bitlinker_debug STATIC IMPORTED GLOBAL)
set_target_properties(bitlinker_debug PROPERTIES
    IMPORTED_LOCATION "${SOCKS5UDPS_LIB_DIR}/libbitlinker64d.a"
)

add_library(bitlinker_release STATIC IMPORTED GLOBAL)
set_target_properties(bitlinker_release PROPERTIES
    IMPORTED_LOCATION "${SOCKS5UDPS_LIB_DIR}/libbitlinker64r.a"
)

target_link_libraries(socks5udps PRIVATE
    Threads::Threads
    crypt
    "$<$<CONFIG:Debug>:bitlinker_debug>"
    "$<$<NOT:$<CONFIG:Debug>>:bitlinker_release>"
)

set_target_properties(socks5udps PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${SOCKS5UDPS_ROOT}/bin/${SOCKS5UDPS_PLATFORM_DIR}/$<CONFIG>"
)
