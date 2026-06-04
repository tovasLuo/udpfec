# GoodTP — CLAUDE.md

## 项目一句话定位

GoodTP 是一个 **FEC + ARQ 混合可靠 UDP 传输静态库**（`libfec.a`，C++11），面向游戏加速和视频传输两种场景，通过编译期宏 `APPLICATION_TYPE` 在两套参数集之间切换。

---

## 构建

```bash
cd build && cmake .. && make    # 输出 build/libfec.a
```

- CMake ≥ 3.10，C++11 标准
- 唯一编译目标：静态库 `fec`（即 `libfec.a`）
- Linux 编译时强制 `-include ctime`
- 构建宏：`VBUILDVERSION="1.0.0"`，`APPLICATION_TYPE`（默认 2 = 游戏加速）

---

## 目录结构

```
src/
  utils/           goodtp.h          公开 C API、类型/错误码/回调定义
                   goodtp_macrodefine.h  编译期参数集与枚举（最常改动）
                   goodtp_comstruct.h    包头位域、Session Key、内部结构体
  tranmempool.h    分级传输内存池（头文件即实现）
  interface/       goodtp_interface.cpp/h   对外 C API 入口（唯一对外暴露层）
  mgr/             goodtp_mgr.cpp/h         GoodTp 实例管理器 + 全局单例 g_goodtp_inst_mgr
  session/         goodtp_session.cpp/h     单条链路会话，聚合 ARQ/FEC/滑窗/拥塞控制
                   goodtp_factorcalulation.cpp/h  MIMD+EWMA 拥塞因子
  arq/             goodtp_arq.cpp/h         HARQ/ARQ 重传，双向链表 + 512 槽 O(1) 索引
  fec2/            goodtp_fec2.cpp/h        二维 XOR FEC 编解码
                   goodtp_fec2_buffer.cpp/h  接收端 FEC 矩阵缓冲区管理
  mempool/         goodtp_mem_pool.cpp/h    定长块内存池（ArqNode / GtpSession 对象池）
  slidwin/         slidwin.cpp + slidwin_self.h   位图环形滑动窗口
  slidwin_utils/   slidwin.h              滑动窗口公开 C API
udp_echo/          echo_client/echo_server 集成验证程序（独立 CMake）
lib-demo-doc/      参考二进制与示例代码（只读参考，不编译入库）
```

---

## 分层架构（依赖方向：上 → 下）

```
应用层 (App)
    │  GtpFrameSend() / GtpPacketReceive() / PeriodGtpTimer()
    ▼
interface 层  goodtp_interface.cpp      参数校验、线程检测、转发调用
    │
    ▼
mgr 层        GoodTp / GtpInstMgr       最多 64 个实例，session unordered_map
    │  1:N
    ▼
session 层    GtpSession                单链路聚合体：三大算法组件 + 统计
    ├── arq   GtpArq                    双向链表 ARQ + Boost 重传
    ├── fec2  GtpFec2 / GtpFec2Buffer  二维 XOR 矩阵编解码
    ├── slidwin  slid_win_hdl × 3       发送/接收/过滤窗口
    └── fac   FactorCalculation         拥塞控制
    │
    ▼
内存层        TranMemPool（7 规格分级）+ GtpMemPool（定长块）
```

---

## 关键不变量（严禁破坏）

### 单线程约束
每个 `GoodTp` 实例**只能由一个线程驱动**。`CheckSingleThreadCalling()` 会检测多线程并发调用（警告不强制中断）。多路并发请创建多个实例（上限 64）。

### APPLICATION_TYPE 编译期切换
`goodtp_macrodefine.h` 中 `APPLICATION_TYPE` 决定两套完全不同的参数集：
- `0/1`（视频/通用）：大窗口 16382、默认 FEC 4×1、MAX_SESSION 1024、TTL 600ms
- `2`（游戏加速，当前默认）：小窗口 2048、默认 FEC 2×2 双向、MAX_SESSION 5120（server）、TTL 4s

**不得在运行时混用两套参数**，切换必须重新编译。

### 包头格式兼容性
发包前必须调用 `GtpHeaderNewToOld(pack, peer_version)` 转换为旧格式，发包后调用 `GtpHeaderOldToNew(pack)` 恢复，以兼容 `peer_version == 0x00` 的旧版本端。不得省略这两个宏调用。

### 内存安全
- `TranBufElement` 两端写有魔数（`0x5a` / `0x5aa55aa5`），改动内存分配逻辑时必须保持魔数完整性
- 引用计数 `referenceCounter` 非零时禁止释放
- 超时 3s 强制回收机制（`MAX_USED_BUF_TIME_S = 3000000 µs`）不得关闭，防内存泄漏

### 零拷贝接口约定
`GtpMallocPackMem` 传入 `old_pack_mem + used_size` 可在同一内存块续分配；`GtpFrameSend` 的 `frame` 参数前至少预留 64 字节给协议头，**调用方不得传入栈上临时 buffer 且没有足够头部空间的数据**。

---

## 核心数据流（速查）

### 发送
```
GtpFrameSend
  → GoodTp::GetSession()               // 查找或懒创建 session
  → session.CalcMaxFramePeriod()
  → session.FramePrepHandler()          // 写包头 + pack_sn_++ + FEC 编码
  → GtpHeaderNewToOld()                 // 兼容旧版本
  → cb_.send_pack_cb_()                 // 回调：应用 UDP 发出
  → GtpHeaderOldToNew()
  → session.FramePostHandler()          // 登记发送滑窗 + 入 ARQ 链表
```

### 接收
```
GtpPacketReceive
  → GoodTp::GetSession()
  → session.PackPrepHandler()
      ├── 验头 → 去重过滤
      ├── [数据包] → recv_frame_cb_() 上报 / 可靠流写 sort_buf_
      ├── [FEC包]  → fec2.Decode() → 若能恢复 → PackPrepHandler(恢复包)
      └── [ACK/NACK] → arq.ProcAck() / ProcNack() → 触发重传
```

### 定时器（需应用层周期调用 `PeriodGtpTimer`）
```
TimerHandler
  ├── arq.CheckRtoRetran()              // RTO 超时扫链表
  ├── 生成 ACK/NACK bitmap 发对端
  ├── fac_.AdjustFactor()               // 拥塞因子 MIMD 更新
  └── fec2.ClearResource()             // 清理过期 FEC 矩阵缓存
```

---

## 编码规范（现有代码风格）

- **类型别名**：内部使用 `u8/u16/u32/u64/i8/i16/i32/i64/f32/f64`（定义于 `goodtp_comstruct.h`）
- **命名**：成员变量后缀 `_`，枚举末尾值带 `Butt` 后缀（哨兵），宏全大写
- **访问控制**：`PRIVATE`/`PROTECTED` 宏（单元测试时展开为 `public`，正常编译为 `private`/`protected`）
- **断言**：`goodtp_asserta(expr)` / `goodtp_assertb(expr, retval)` 宏，仅在 `_SELFDEBUG` 下生效
- **日志**：通过 `GtpLog(wrt_log_cb, module_id, log_level, fmt, ...)` 宏，不直接调用 `printf`
- **包头访问**：始终通过位域结构体，不得直接偏移指针访问包头字段
- **错误返回**：返回 `GTP_OK(0)` 或 `RETURN_ERR(module_id, error_code)` 宏生成的复合错误码，不抛异常

---

## 关键常量速查（游戏加速模式 APPLICATION_TYPE=2）

| 常量 | 值 | 含义 |
|------|-----|------|
| `MAX_GTP_INST_NUM` | 64 | 全局实例上限 |
| `GTP_VERSION` | 0x02 | 当前协议版本 |
| `SLID_WIN_SIZE` | 2048 | 滑动窗口包数 |
| `MAX_FEC2_CACHE_CAPACITY` | 32 | FEC 接收缓冲矩阵数 |
| `DEFAULT_FEC2_BOOK_ID` | 4 | 默认 FEC 模式（2×2 H+V） |
| `MAX_SESSION_NUM` (server) | 5120 | 最大并发 session 数 |
| `MIN_SESSION_TTL_US` | 4,000,000 | session 超时（4s） |
| `DEFAULT_RTO_TIMEOUT_US` | 500,000 | 默认 RTO（500ms） |
| `MIN_RTO_US` / `MAX_RTO_US` | 25,000 / 500,000 | RTO 范围 |
| `MAX_RETRAN_PACKET_TIMES` | 3 | 最大重传次数 |
| `ARQ_NODE_ARRAY_SIZE` | 512 | ARQ 二级索引槽数 |
| `BUF_OFFSET_SIZE` | 256 | 内存块头部预留（128B 协议头 + 128B 地址） |
| `MAX_USED_BUF_TIME_S` | 3,000,000 µs | 内存块强制回收超时 |

---

## FEC Codebook（`fec_mode_book_[6]`）

| ID | 矩阵 | 方向 | 场景 |
|----|------|------|------|
| 0 | 4×4 | H+V+UH+DH | 极高冗余 |
| 1 | 4×4 | H+V | 高冗余 |
| 2 | 4×1 | H only | 通用默认（TYPE=0/1） |
| 3 | 2×2 | H+V+UH+DH | 小矩阵全向 |
| 4 | 2×2 | H+V | 游戏默认（TYPE=2） |
| 5 | 2×1 | H only | 极低冗余 |

---

## Session Key 哈希说明

`GtpSessionKey` 用 `(sfd, 对端IP, 对端端口, 本端IP, 本端端口)` 五元组构造，`key_` 字段是通过位逆序哈希 sfd 低 16 位后与 IP+端口累加合成的 64 位值。当 `enable_key_=1` 时使用应用层传入的 `stream_key_` 代替（`key_3rd_flag_=1`），此时比较只看 `key_`，不做五元组匹配。

---

## 注意事项

1. **不要随意修改 `goodtp_macrodefine.h` 中的参数**，会同时影响内存预分配、窗口大小、FEC 缓冲区数量，可能导致越界或OOM。
2. **滑动窗口缓存** `win_cache_[GTP_INST_COM_CACHE_SIZE=3072]` 是三个 `slid_win_hdl` 共享的物理内存，分配逻辑在 `GtpSession::Init()` 中，改动需确保三段不重叠。
3. **ARQ 链表遍历**发生在 `CheckRtoRetran()` 热路径中，不要在此路径增加动态内存分配。
4. **FEC 恢复包**会重新走 `PackPrepHandler()` 路径（递归调用风险），FEC 解码逻辑确保不会二次触发 FEC 恢复，修改时须保持此保护。
5. `udp_echo/` 是独立的集成测试程序，有自己的 CMakeLists.txt，用于验证库接口，不属于库本身。
