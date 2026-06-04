cmake_minimum_required(VERSION 3.10)
project(udp_echo CXX)

set(CMAKE_CXX_STANDARD 11)

# goodtp 头文件路径（demotest 提供的公开头文件）
set(GOODTP_INC "${CMAKE_CURRENT_SOURCE_DIR}/../lib-demo-doc/lib/inc")

# 使用本地 build 目录编译出的静态库
set(GOODTP_LIB "${CMAKE_CURRENT_SOURCE_DIR}/../build/libfec.a")

# 头文件
include_directories(
    ${GOODTP_INC}
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/utils
)

# 公共链接选项
set(COMMON_LIBS
    ${GOODTP_LIB}
    pthread
    dl
)

# ── 服务端 ──
add_executable(echo_server server.cpp)
target_link_libraries(echo_server PRIVATE ${COMMON_LIBS})

# ── 客户端 ──
add_executable(echo_client client.cpp)
target_link_libraries(echo_client PRIVATE ${COMMON_LIBS})
