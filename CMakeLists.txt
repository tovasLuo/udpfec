cmake_minimum_required(VERSION 3.10)
project(fec C CXX)

set(CMAKE_CXX_STANDARD 11)

add_library(fec STATIC
    src/slidwin/slidwin.cpp
    src/arq/goodtp_arq.cpp
    src/fec2/goodtp_fec2_buffer.cpp
    src/fec2/goodtp_fec2.cpp
    src/interface/goodtp_interface.cpp
    src/mempool/goodtp_mem_pool.cpp
    src/mgr/goodtp_mgr.cpp
    src/session/goodtp_factorcalulation.cpp
    src/session/goodtp_session.cpp
)

target_include_directories(fec PUBLIC
    src/slidwin
    src/slidwin_utils
    src/utils
    src/arq
    src/fec2
    src/interface
    src/mempool
    src/mgr
    src/session
    src
)

target_compile_definitions(fec PRIVATE
    VBUILDVERSION="1.0.0"
)

if(UNIX)
    target_compile_options(fec PRIVATE -include ctime)
endif()
