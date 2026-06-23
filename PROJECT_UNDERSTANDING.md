# GoodTP / udpfec 工程理解

本文档基于当前仓库源码整理，用于快速理解工程边界、核心数据流和后续维护注意点。仓库内已有 `CLAUDE.md`、`DESIGN.md`，但当前文件内容呈现为编码乱码，因此本文档以源码为准。

## 1. 工程定位

本工程核心是一个 C++11 静态库 `fec`，对外暴露 C 风格 API，提供基于 UDP 的可靠/半可靠传输能力。核心机制是：

- FEC2：二维 XOR 前向纠错，发送端生成冗余包，接收端用矩阵尝试恢复丢包。
- ARQ/HARQ：ACK/NACK 与 RTO 定时重传，支持 boost 预重传。
- 滑动窗口：记录发送/接收序号，生成 ACK/NACK bitmap，统计 RTT、抖动、丢包率、PPS、预拥塞等级。
- 内存池：固定块池和传输包池，减少热路径动态分配，并支持包数据与 `GtpAddr` 共存。

主构建目标在根目录 `CMakeLists.txt`：

```text
add_library(fec STATIC ...)
```

生成静态库通常为 `build/libfec.a`。

## 2. 目录结构

```text
src/
  interface/       对外 API 实现与公开头 bitlinker.h
  mgr/             GoodTp 实例管理、session map、全局模块状态
  session/         单条 UDP 链路会话，聚合 ARQ/FEC/滑窗/质量控制
  arq/             ARQ/HARQ 缓存、ACK/NACK 处理、RTO/boost 重传
  fec2/            二维 XOR FEC 编码、解码、FEC 缓冲管理
  mempool/         固定大小对象池，用于 ArqNode/GtpSession
  slidwin/         滑动窗口实现
  slidwin_utils/   滑动窗口 C API 头
  utils/           协议结构、宏、错误码、类型定义
  win/             旧版/Windows 相关 FEC 实现文件，当前根 CMake 未编入
  tranmempool.h    分级传输包内存池，头文件内实现

udp_echo/          Linux UDP echo 和 FEC 丢包测试程序
socks5udpf/        独立 socks5 UDP 转发/代理项目
lib-demo-doc/      外部 demo、预编译库和示例工程资料
```

## 3. 对外使用模型

公开 API 主要在 `src/interface/bitlinker.h`，实现集中在 `src/interface/goodtp_interface.cpp`。

典型调用顺序：

```text
InsLoadGtpModule()
CreateGtpInstance(system_id, session_ttl_us, callbacks, mem_pool_cfg)

发送应用帧：
  GtpMallocPackMem()
  填充 payload 与 GtpAddr
  GtpFrameSend()
  库内通过 send_pack_cb_ 把实际 UDP 包交给应用发送

接收 UDP 包：
  GtpMallocPackMem()
  拷贝 UDP 包与来源地址到 GtpAddr
  GtpPacketReceive()
  库内解包/恢复/排序后通过 receive_frame_cb_ 上交应用帧

周期调度：
  PeriodGtpTimer()

退出：
  DeleteGtpInstance()
  RmLoadGtpModule()
```

必须注册的回调：

- `send_pack_cb_`：库需要发送任何数据包、ACK、NACK、FEC、RTT 包时调用。
- `receive_frame_cb_`：库恢复出应用帧后调用。

可选回调：

- `report_link_quality_cb_`：报告链路质量。
- `write_log_cb_` / `cur_log_level_cb_`：日志输出。
- `close_session_cb_`：session 超时销毁时通知应用。

## 4. 核心对象关系

```text
应用层
  |
  | C API: GtpFrameSend / GtpPacketReceive / PeriodGtpTimer
  v
interface
  |
  v
GoodTp
  - 全局最多 MAX_GTP_INST_NUM=64 个实例
  - 每实例一个 session_map
  - 每实例持有 ARQ 节点池、Session 池、传输包池、FEC codebook
  |
  v
GtpSession
  - data_win_s_：发送滑窗
  - data_win_r_：接收滑窗
  - filter_win_：重复包过滤窗口
  - arq_：未确认包缓存与重传
  - fec2_obj_：FEC 编解码
  - fac_：反馈频率/拥塞控制因子
```

`GtpSessionKey` 支持两种会话识别方式：

- 默认：由 socket、对端 IP/端口、本端 IP/端口构造。
- `enable_key_=1`：使用应用传入的 `stream_key_` 作为 session key，适合五元组不稳定或多路复用场景。

## 5. 协议包结构

所有 GoodTP 包以 `GtpPacket` 位域头开始。头部字段包括：

- `goodtp_ver_`：当前 `GTP_VERSION=0x02`。
- `header_offset_`：payload 起始偏移。
- `pack_type_`：包类型。
- `pack_size_`：整包长度。
- `pack_sn_`：发送序号。
- `has_check_flag_`、`has_ts_flag_`、`has_rtt_flag_`、`repeat_counter_`、`stream_type_` 等扩展字段。

包类型定义：

```text
1 DATA
2 ACK
3 FEC
4 RTT request
5 RTT response
6 NACK
```

兼容逻辑：

- `GtpHeaderNewToOld(pack, peer_version)`：发给老版本 peer 前转换头格式。
- `GtpHeaderOldToNew(pack)`：发送回调后恢复本地格式。
- `GtpPacketReceive()` 遇到 `goodtp_ver_ == 0x00` 会认为 peer 版本过旧并报错。

## 6. 发送路径

```text
GtpFrameSend()
  参数校验、单线程检查、更新时间戳
  GoodTp::GetSession()
    不存在则 BuildNewSession()
  GtpSession::CalcMaxFramePeriod()
  GtpSession::FramePrepHandler()
    计算头长
    写入 GtpPacket 头
    分配/递增 pack_sn_
    FEC2 Encode()
  GtpHeaderNewToOld()
  send_pack_cb_()
  GtpHeaderOldToNew()
  GtpSession::FramePostHandler()
    发送滑窗登记
    ARQ PacketEntryList()
```

调用方需要注意：`GtpFrameSend()` 的 `frame` 必须来自 `GtpMallocPackMem()` 或至少预留足够头部空间。注释中要求前面至少有 64 字节供协议头使用。

## 7. 接收路径

```text
GtpPacketReceive()
  参数校验、单线程检查、版本/包长校验
  GoodTp::GetSession()
  GtpSession::PackPrepHandler()
    重复包过滤
    接收滑窗登记
    DATA：得到应用 frame
    FEC：进入 FEC2 Decode()，可能恢复 DATA 包
    ACK/NACK：进入 ARQ ProcAck()/ProcNack()
    RTT：更新 RTT/RTO 或回包
  GtpSession::PackPostHandler()
  DATA 包进入 ReorderEnqueue()
  连续可交付时 receive_frame_cb_()
```

当前 `GtpSession` 内有一个小型接收重排缓冲：

```text
REORDER_BUF_SIZE = 16
```

用于降低乱序和重传包对上层交付顺序的影响。FEC、ACK、NACK、RTT 不进入该排序逻辑。

## 8. 定时器路径

应用必须周期调用 `PeriodGtpTimer()`，示例中通常 10ms 一次。其内部调用：

```text
GoodTp::CheckResourceActiveStatus()
  session TTL 检查与销毁
  GtpSession::TimerHandler()
    ARQ RTO/boost 重传扫描
    ACK/NACK 反馈生成
    FEC 过期资源清理
    链路质量与算法参数更新
```

如果定时器调用过慢，`GoodTp::CheckResourceActiveStatus()` 会输出 `wheel run too slow` 日志。

## 9. ARQ/HARQ

`GtpArq` 维护一个双向链表 `ArqList`，保存已发送但未确认的 `ArqNode`。

关键行为：

- 发送成功后 `PacketEntryList()` 将包放入 ARQ 链表，并增加传输包内存引用计数。
- 收到 ACK bitmap 后，确认位为 1 的包释放；确认位为 0 且落入 RTO 范围的包触发快速重传。
- 收到 NACK 后，NACK 指定的 SN 触发快速重传。
- 定时器调用 `CheckRtoRetran()`，对超时包做 RTO 重传或 boost 重传。
- 超过 `MAX_RETRAN_PACKET_TIMES=3` 后，实时流一般释放；可靠流在部分路径下会再尝试。

Boost 机制：

- 网络质量变差时 `boost_switch_` 打开。
- `CloneBoostNode()` 克隆一份包作为更短周期的预重传候选。
- `boost_period_us_` 由 RTT 推导并限制在 `MIN_BOOST_PERIOD_US..MAX_BOOST_PERIOD_US`。

## 10. FEC2

`GtpFec2` 使用二维矩阵 XOR。发送端将数据包放入矩阵，达到对应行/列/斜线条件后生成 FEC 包。接收端缓存数据包和 FEC 包，当某条编码线只缺一个包时，通过 XOR 恢复。

当前 codebook：

```text
0: 4x4 H+V+UH+DH
1: 4x4 H+V
2: 4x1 H
3: 2x2 H+V+UH+DH
4: 2x2 H+V        默认，APPLICATION_TYPE=2
5: 2x1 H
```

实现特点：

- `XorEncode()` 针对 SSE2 / ARM NEON / 64-bit / 32-bit 有不同路径。
- `Fec2Buffer` 管理环形包缓存。
- `RecvFec2CodeMatrix` 同时维护多个接收矩阵，数量为 `MAX_RECV_FEC2_MATRIX_NUM = MAX_FEC2_CACHE_CAPACITY / 2`。
- `ClearReceiveUnUsedResource()` 用于处理缓存绕回后旧矩阵残留，避免 stale matrix 错误恢复。
- `GtpCheckPacketInvalid()` 可在 `has_check_flag_` 时从包中提取 `stream_key_`，FEC 包也支持 key 提取。

## 11. 滑动窗口

滑动窗口模块在 `src/slidwin` 和 `src/slidwin_utils`。`GtpSession` 创建三个窗口：

- 发送窗口：记录发出包和 ACK bitmap。
- 接收窗口：记录收到包，生成 ACK/NACK。
- 过滤窗口：判断重复包。

滑窗负责：

- SN 有效性判断和窗口推进。
- 重复包过滤。
- ACK/NACK bitmap 生成。
- 丢包率、RTT、jitter、PPS、预拥塞等级统计。
- 通过 `LinkQualityCallback()` 反馈给 `GtpSession`，进而调整 ARQ/FEC 参数。

## 12. 内存管理

### TranMemPool

`src/tranmempool.h` 是传输包内存池，按规格分级：

```text
256B, 512B, 1K, 1.5K, 4K, 8K, 64K
```

每块内存预留：

- `HEADER_RSV_SIZE=128`
- `TP_ADDR_RSV_SIZE=160`
- `BUF_OFFSET_SIZE=288`

这使一块传输内存可同时容纳协议头、payload 以及 `GtpAddr`。每个块有引用计数、使用时间、首尾魔数和尾部 overlay 检查。

### GtpMemPool

`src/mempool` 是固定大小对象池，用于：

- `ArqNode`
- `GtpSession`

通过 bitmap 找空闲块，块头含 overlay 与引用计数。

## 13. 编译期配置

主要配置位于 `src/utils/goodtp_macrodefine.h`。

当前：

```text
APPLICATION_TYPE = 2
ENABLE_ARQ       = 1
ENABLE_FEC       = 1
SUPPORT_RELIABLE_TRAN = 0
```

`APPLICATION_TYPE=2` 是游戏加速参数集：

```text
MIN_SESSION_TTL_US       4,000,000 us
SLID_WIN_SIZE            2048
DEFAULT_FEC2_BOOK_ID     4
MAX_FEC2_CACHE_CAPACITY  32
MAX_RECV_FEC2_MATRIX_NUM 16
MAX_SESSION_NUM          Linux server: 5120
MAX_SUPPORT_PPS          20000
MAX_FEEDBACK_NACK_PERIOD_US 50000
```

`APPLICATION_TYPE=0/1` 是通用/视频参数集，窗口和 FEC 缓存更大，默认 FEC book 为 2。

注意：这些参数是编译期宏，不应运行时混用。

## 14. 示例与测试

### udp_echo

`udp_echo` 是集成验证程序，依赖根工程构建出的 `build/libfec.a`。

包含：

- `echo_server`：UDP server，收到 GoodTP frame 后 echo 回客户端。
- `echo_client`：周期发送 EchoFrame，统计 RTT 和 GoodTP 控制包数量。
- `fec_loss_test`：单进程模拟双端通信、可控丢包、FEC book 测试、stream key 测试。

注意：`udp_echo/CMakeLists.txt` 里还引用了 `game_net_test.cpp`，但当前仓库文件列表中没有这个源文件，直接配置该子工程可能失败。

### socks5udpf

`socks5udpf` 是一个独立的 socks5 UDP 转发/代理项目，有自己的 CMake、源码、工具类和 VS 工程。它不是根目录 `fec` 静态库的构建输入。

### lib-demo-doc

包含 demo tester、配置文件、预编译 goodtp 库和 Windows/Linux 工程资料，主要是参考或历史集成材料。

## 15. 重要约束

- 单个 `GoodTp` 实例预期由单线程驱动。代码会通过 `CheckSingleThreadCalling()` 检查并告警，但不是强制互斥。
- 应用必须周期调用 `PeriodGtpTimer()`，否则 RTO 重传、ACK/NACK、FEC 清理、session TTL 都会异常。
- `GtpMallocPackMem()` / `GtpFreePackMem()` 应成对使用；发送成功后通常由库接管，发送失败路径需要应用释放。
- 使用 `enable_key_=1` 时，收包侧可先调用 `GtpCheckPacketInvalid()` 提取 `stream_key_` 再填入 `GtpAddr`。
- 修改 `goodtp_macrodefine.h` 会影响窗口大小、FEC 缓存、内存池容量、session 上限，风险较大。
- 包头访问依赖位域布局和平台分支，跨编译器/ABI 改动要谨慎。

## 16. 当前观察到的问题或风险

- 文档文件存在编码乱码，建议后续统一为 UTF-8。
- `udp_echo/CMakeLists.txt` 引用缺失的 `game_net_test.cpp`。
- 根 CMake 只构建核心库，不构建 `udp_echo` 或 `socks5udpf`。
- `SUPPORT_RELIABLE_TRAN` 当前为 0，但示例里使用 `kReliableStream`；可靠流相关行为需要结合实际测试确认。
- 代码大量使用宏、位域、手工内存池和 `goto`，维护时应优先补小范围回归测试，尤其是 FEC 恢复、ARQ 重传和 stream key 路径。

