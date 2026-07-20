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
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/interface
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

# ── FEC 丢包测试 ──
add_executable(fec_loss_test fec_loss_test.cpp)
target_link_libraries(fec_loss_test PRIVATE ${COMMON_LIBS})

# ── 全方位专业测试 ──
add_executable(game_net_test game_net_test.cpp)
target_link_libraries(game_net_test PRIVATE ${COMMON_LIBS})

# ── FEC book 切换滞回观测 ──
add_executable(loss_transition_test loss_transition_test.cpp)
target_link_libraries(loss_transition_test PRIVATE ${COMMON_LIBS})

# ── 卡头阻塞修复快速验证（S1/S2/L15/L25 定向复现） ──
add_executable(quick_repro_test quick_repro_test.cpp)
target_link_libraries(quick_repro_test PRIVATE ${COMMON_LIBS})

# ── 详细指标矩阵报告（FEC恢复占比/ARQ成功丢弃/boost占比/延迟带宽） ──
add_executable(metrics_matrix_test metrics_matrix_test.cpp)
target_link_libraries(metrics_matrix_test PRIVATE ${COMMON_LIBS})

# ── stream_key 是否污染FEC矩阵起始序号：阻断ARQ反馈路径，隔离出纯FEC恢复能力对比 ──
add_executable(streamkey_fec_isolation_test streamkey_fec_isolation_test.cpp)
target_link_libraries(streamkey_fec_isolation_test PRIVATE ${COMMON_LIBS})

# ── 流量分类占比（DATA首发/ARQ重传/boost/FEC/ACK/NACK）──
add_executable(traffic_breakdown_test traffic_breakdown_test.cpp)
target_link_libraries(traffic_breakdown_test PRIVATE ${COMMON_LIBS})

# ── ARQ per-session节点数硬上限验证（peer完全静默场景）──
add_executable(arq_cap_test arq_cap_test.cpp)
target_link_libraries(arq_cap_test PRIVATE ${COMMON_LIBS})
