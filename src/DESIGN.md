# GoodTP 库设计文档

**版本**：GTP_VERSION = 0x02  
**版权**：2021–2031, Free Albort.Feng Studio  
**作者**：Albert.Feng / zn.ye  
**构建产物**：静态库 `libfec.a`（C++11）

---

## 目录

1. [概述](#1-概述)
2. [整体架构](#2-整体架构)
3. [模块详解](#3-模块详解)
4. [关键算法](#4-关键算法)
5. [内存管理](#5-内存管理)
6. [Session 生命周期](#6-session-生命周期)
7. [数据流](#7-数据流)
8. [配置参数](#8-配置参数)
9. [公共 API](#9-公共-api)
10. [设计原则与约束](#10-设计原则与约束)

---

## 1. 概述

GoodTP 是一个面向**实时/可靠 UDP 传输**的前向纠错（FEC）与自动重传请求（ARQ）混合传输库。主要应用场景：

- **游戏加速**（APPLICATION_TYPE=2）：低延迟优先，小窗口，快速响应
- **视频传输**（APPLICATION_TYPE=0/1）：高吞吐优先，大窗口，高冗余

核心能力：
- **FEC2**：二维 XOR 矩阵前向纠错，支持水平/垂直/斜向多方向编码
- **ARQ/HARQ**：混合自动重传，支持 RTO 超时、ACK bitmap、NACK 驱动及 Boost 加速重传
- **滑动窗口**：位图环形结构，负责乱序处理、去重、ACK/NACK 生成
- **拥塞控制**：MIMD + EWMA 自适应速率因子
- **零拷贝友好**：分级内存池，支持多包共存同一内存块

---

## 2. 整体架构

```
┌─────────────────────────────────────────────────────┐
│                    应用层 (App)                       │
│         GtpFrameSend() / GtpPacketReceive()          │
└──────────────────────┬──────────────────────────────┘
                       │ C API
┌──────────────────────▼──────────────────────────────┐
│              Interface 接口层                        │
│              goodtp_interface.cpp                    │
└──────────────────────┬──────────────────────────────┘
                       │
┌──────────────────────▼──────────────────────────────┐
│               GoodTp 管理器 (GtpInstMgr)             │
│  · 最多 64 个实例 (MAX_GTP_INST_NUM)                 │
│  · session 哈希表 (unordered_map<Key, GtpSession*>)  │
│  · ARQ节点池 / 传输内存池 / Session对象池             │
└──────────────────────┬──────────────────────────────┘
                       │ 1:N
┌──────────────────────▼──────────────────────────────┐
│               GtpSession 会话                        │
│  ┌──────────┐ ┌──────────┐ ┌──────────────────────┐ │
│  │  GtpArq  │ │ GtpFec2  │ │    SlidWin × 3       │ │
│  │  重传管理 │ │ 前向纠错 │ │ 发送/接收/过滤窗口   │ │
│  └──────────┘ └──────────┘ └──────────────────────┘ │
│  ┌──────────────────────────────────────────────┐   │
│  │         FactorCalculation  拥塞控制           │   │
│  └──────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────┘
                       │ 共用
┌──────────────────────▼──────────────────────────────┐
│                    内存池层                          │
│  TranMemPool（分级传输包）+ GtpMemPool（定长块）      │
└─────────────────────────────────────────────────────┘
```

### 模块清单

| 模块 | 文件 | 职责 |
|------|------|------|
| utils | `goodtp.h`, `goodtp_comstruct.h`, `goodtp_macrodefine.h` | 类型定义、宏、包结构、错误码 |
| interface | `goodtp_interface.cpp/h` | 对外 C API 入口 |
| mgr | `goodtp_mgr.cpp/h` | `GoodTp` 实例管理器，session 哈希表 |
| session | `goodtp_session.cpp/h` | 单条链路会话，聚合 ARQ/FEC/滑窗 |
| arq | `goodtp_arq.cpp/h` | HARQ/ARQ 重传逻辑 |
| fec2 | `goodtp_fec2.cpp/h`, `goodtp_fec2_buffer.cpp/h` | 二维 XOR FEC 编解码 |
| slidwin | `slidwin.cpp`, `slidwin_self.h`, `slidwin_utils/slidwin.h` | 滑动窗口（发送/接收/过滤） |
| mempool | `goodtp_mem_pool.cpp/h` | 定长块内存池（ARQ 节点、Session 对象） |
| tranmempool | `tranmempool.h` | 分级传输内存池（数据包载体） |
| session/factor | `goodtp_factorcalulation.cpp/h` | EWMA 拥塞控制因子计算 |

---

## 3. 模块详解

### 3.1 协议包头（`goodtp_comstruct.h`）

所有传输包共用 10 字节固定头（位域结构）：

```
Bit Layout (10 bytes):
 [7:0]   goodtp_ver_       协议版本 (0x02)
 [13:8]  header_offset_    头部总长度，用于定位 payload
 [14]    cache_us_flag_    是否携带缓存时间戳
 [15]    has_loss_flag_    是否携带丢包率字段
 [18:16] pack_type_        包类型（数据/ACK/NACK/FEC/RTT等）
 [31:19] pack_size_        整包字节数（最大 8192 字节）
 [63:32] pack_sn_          包序列号（32位，循环使用）
 [64]    has_check_flag_
 [65]    has_ts_flag_      是否携带发送时间戳
 [66]    has_rtt_flag_     是否携带 RTT 字段
 [67]    init_flag_        会话初始包标志
 [70:68] repeat_counter_   重传计数
 [71]    is_qos_flg_
 [77:72] share_zone_       共享扩展区编号
 [78]    has_chg_zone_     是否携带 ChangeZone（排序SN + 时间戳）
 [79]    stream_type_      0=实时流 1=可靠流
```

**ACK 包**额外携带：SN 位图大小、头尾 SN 偏移、接收端丢包率（×128 定点整数）、已收到包的 bitmap。

**FEC 包**额外携带：矩阵行列索引、方向类型（水平/垂直/上斜/下斜）。

---

### 3.2 GoodTp 管理器（`goodtp_mgr.h`）

```cpp
// 全局单例管理器
struct GtpInstMgr {
    pthread_spinlock_t *spin_;          // 实例创建/销毁锁
    volatile u32 inst_num_;             // 当前实例数
    volatile u64 runing_flag_;          // 64位位图，标记哪些槽位在用
    GoodTp *goodtp_inst_[MAX_GTP_INST_NUM]; // 最多 64 个实例
};

// 单个 GoodTp 实例关键成员
class GoodTp {
    GtpCallBackParam cb_;               // 回调集合（发包/收帧/日志等）
    GtpMemPool arq_node_mem_pool_;      // ARQ 节点内存池
    TranMemPool packet_mem_pool_;       // 传输包内存池
    GtpMemPool session_mem_pool_;       // Session 对象内存池
    unordered_map<GtpSessionKey,
                  GtpSession*,
                  GtpSessionKeyHash> session_map_;
    GtpFec2Mode fec_mode_book_[MAX_FEC2_MODE_BOOK_ID]; // FEC codebook
    u8 win_cache_[GTP_INST_COM_CACHE_SIZE];  // 滑动窗口共用缓存
    u64 session_ttl_us_;                // Session 超时时间
    goodtp_tid init_tid_;               // 初始化线程 ID（单线程检测）
};
```

**Session 键**：`GtpSessionKey = (sfd, 对端IP, 对端端口, 本端IP, 本端端口)`，使用位逆序哈希减少碰撞。

---

### 3.3 Session（`goodtp_session.h`）

每条端到端 UDP 链路对应一个 `GtpSession`，聚合全部算法组件：

```cpp
class GtpSession {
    slid_win_hdl data_win_s_;     // 发送滑动窗口
    slid_win_hdl data_win_r_;     // 接收滑动窗口
    slid_win_hdl filter_win_;     // 重复包过滤窗口
    FactorCalculation fac_;       // 拥塞控制
    SessionPublicData pb_dt_;     // IP/端口/统计/时间戳
    GtpArq arq_;                  // ARQ 对象
    GtpFec2 fec2_obj_;            // FEC2 对象
    u32 pack_sn_;                 // 当前发送序号
    u32 sort_sn_;                 // 可靠流排序序号
    u32 ack_sn_;                  // 最新 ACK 的 SN
    u32 send_session_stat_:4;     // 发送侧状态机
    u32 recv_session_stat_:4;     // 接收侧状态机
    u32 mode_:2;                  // kSender / kReceiver
    u32 stream_type_:1;           // kRealTimeStream / kReliableStream
};
```

Session 提供三个核心入口：
- `FramePrepHandler()`：发包前处理（写头部、FEC 编码）
- `FramePostHandler()`：发包后处理（登记发送窗口、加入 ARQ 链表）
- `PackPrepHandler()`：收包处理（去重、滑窗登记、分发给上层/ARQ/FEC）

---

### 3.4 ARQ（`goodtp_arq.h`）

使用双向链表 `ArqList` 维护所有已发出但未确认的包节点 `ArqNode`：

```cpp
struct ArqNode {
    ArqNode *pre_node_, *nxt_node_;  // 链表指针
    u64 last_send_ts_us_;            // 最近一次发送时刻
    u8 *pack_;                       // 包数据（指向 TranMemPool）
    u16 pack_len_;
    u8  retran_counter_;             // 已重传次数
    u8  boost_threshold_;            // Boost 重传阈值
    u8  boost_node_flg_:1;          // 是否为 Boost 副本节点
    u32 pack_sn_;
};
```

**二级索引数组**（512 槽，O(1) 按 SN 查找）：
```
index = (sn >> 7) & 0x01FF    → 槽位
slot  存储该 SN 对应的 ArqNode*
```

---

### 3.5 FEC2（`goodtp_fec2.h`）

二维 XOR 矩阵编解码，支持四个方向的冗余包：

```
方向        描述                冗余包数
Horizontal  行内 XOR            每行 1 个
Vertical    列内 XOR            每列 1 个
Uphill      反斜线方向 XOR      每条斜线 1 个
Downhill    正斜线方向 XOR      每条斜线 1 个
```

**编码矩阵**：`encode_matrix_[h_size × v_size]`，按行优先顺序填入数据包，满行/列触发 XOR 计算并发出冗余包。

**解码矩阵池**：维护 `MAX_RECV_FEC2_MATRIX_NUM` 个并发矩阵（游戏模式=16，视频模式更大），每个矩阵记录已收位图，当某行/列仅缺 1 包时执行 XOR 恢复。

**FEC Codebook**：

| ID | 矩阵 | H | V | UH | DH | 适用场景 |
|----|------|---|---|----|----|---------|
| 0 | 4×4 | ✓ | ✓ | ✓  | ✓  | 极高冗余 |
| 1 | 4×4 | ✓ | ✓ | ✗  | ✗  | 高冗余 |
| 2 | 4×1 | ✓ | ✗ | ✗  | ✗  | 通用默认 |
| 3 | 2×2 | ✓ | ✓ | ✓  | ✓  | 小矩阵全向 |
| 4 | 2×2 | ✓ | ✓ | ✗  | ✗  | 游戏加速默认 |
| 5 | 2×1 | ✓ | ✗ | ✗  | ✗  | 极低冗余 |

**自适应 FEC**：接收端丢包率超阈值时保持 FEC 开启；网络质量评估为 Good 且 PPS 足够高时关闭 FEC 节省带宽。

---

### 3.6 滑动窗口（`slidwin.h`）

位图环形结构，3 个实例分别用于发送记账、接收记账、重复包过滤：

```
游戏加速模式:  WIN_BUF_SIZE = 1024 B = 8192 bits → 2048 包窗口
视频/通用模式: WIN_BUF_SIZE = 64 KB → 16382 包窗口

滑动条件: 头部连续 MIN_MOVE_STEP=64 位全为1 → 窗口向前推进
ACK 生成: GenAckSnBitMap() 输出 [head_sn, tail_sn) 范围的已收位图
NACK 生成: 反向扫描0位 → 最多输出 MAX_FEEDBACK_NACK_SN_NUM=12 个
```

---

### 3.7 拥塞控制（`goodtp_factorcalulation.h`）

基于 MIMD（乘性增加/加法减少）策略，辅以 EWMA 平滑：

```
factorA ∈ [0.526, 10.0]    控制发包速率反馈频率

无丢包:  factorA *= mi_f   (mi_f = 4.0，乘性增加)
有丢包:  factorA -= ad_f   (ad_f = 1.0，加法减少)

EWMA:   beta = 0.82，环形采样数组大小 SF_MAX_NUM = 16
        采样超时阈值 SD_TIMEOUT_US = 5,000,000 µs
```

丢包率阈值判定（来自 `GetStreamQos`）：
- 实时流：丢包率 > `MAX_REALTIME_LOSS_THRESHLD`（游戏50%，视频70%）→ 维持 FEC
- 可靠流：丢包率 > `MAX_RELIABLE_LOSS_THRESHLD`（游戏80%，视频90%）→ 维持 FEC

---

## 4. 关键算法

### 4.1 ARQ 重传决策

```
JudgeCanRtoReSend(node, now):
  1. if retran_counter_ >= max_retran_times_  → 放弃（丢包超限）
  2. if now - last_send_ts_us_ > rto_timeout_us_
       → kNormalRtran（正常 RTO 重传）
  3. if boost_switch_ && now > bst_rto_ts_us_
       → kBoostRtran（提前 Boost 重传）
  4. else → kNoRtran
```

**Boost 重传**：对高优先级包克隆副本节点（`boost_node_flg_=1`），以更短的超时提前触发，适用于游戏加速场景对极低延迟的需求。

**ACK 处理**（`ProcAck`）：解析收到的 SN bitmap，从 ARQ 链表中移除已确认节点；对 bitmap 中为0的 SN 触发选择性重传。

### 4.2 FEC2 恢复逻辑

```
收到数据包 / FEC 包:
  1. 定位对应的 decode_matrix_ (按矩阵起始SN索引)
  2. 更新矩阵的已收位图
  3. 扫描所有行/列/斜线:
     若某行/列 (missing_count == 1):
       XOR 所有已收到的数据 → 还原丢失包
       → Fec2RestoreFrameReceive() → 重新走接收路径
  4. 若矩阵全部到齐或超时 → 释放矩阵
```

### 4.3 包排序缓冲（可靠流）

可靠流（`kReliableStream`）维护 `sort_sn_` 预期序号和 `sort_buf_[]` 排序缓冲（大小 `MAX_SORT_BUF_SIZE`），乱序包暂存缓冲，等收齐连续序号后按序上报给应用。

---

## 5. 内存管理

### 5.1 传输内存池（TranMemPool）

支持 7 个规格的内存块：

| 规格 | 大小 |
|------|------|
| kBuf256Bytes | 256 B |
| kBuf512Bytes | 512 B |
| kBuf1K | 1024 B |
| kBuf1dot5K | 1536 B |
| kBuf4K | 4096 B |
| kBuf8K | 8192 B |
| kBuf64K | 65536 B |

每个内存块（`TranbufElement`）的关键设计：
- `referenceCounter`：引用计数，非零禁止回收
- 三指针（`current_pack_pos` / `current_malloc_pos` / `sub_last_malloc_pos`）支持**多包共存同一内存块**，减少碎片
- `used_time_us`：超时 3,000,000 µs 自动强制回收
- `mem_check_overlay_0/1`：内存越界魔数校验（`0x5a` / `0x5aa55aa5`）
- 头部预留：`BUF_OFFSET_SIZE = 256 B`（128B 协议头 + 128B 传输地址），数据区 64 字节对齐

**包大小到规格映射**（O(1) 查表宏）：
```c
GtpPackSizeToMemSpec(size)
// 256 → kMemSpec256Bytes
// ...
// >8192 → kMemSpec64k
```

### 5.2 定长块内存池（GtpMemPool）

用于管理 `ArqNode` 和 `GtpSession` 对象：

```cpp
struct PoolMgr {
    u32 pool_size_;          // 总块数
    u32 using_num_;          // 当前使用数
    u64 *used_bitmap_;       // 位图快速查找空闲块
    u32 last_free_pos_;      // 上次释放位置（加速下次分配）
    PoolNode *mem_pool_[0];
};
```

分配复杂度：O(1) 均摊（`last_free_pos_` 提示 + 位图扫描）。

---

## 6. Session 生命周期

```
创建时机: GtpFrameSend / GtpPacketReceive 时若不存在则创建
标识键:   GtpSessionKey = hash(sfd + 对端IP:端口 + 本端IP:端口)

活跃检测:
  last_active_ts_us_ 每次收发均更新
  CheckResourceActiveStatus() 周期扫描：
    now - last_active_ts_us_ > session_ttl_us_ → CalcSessionQualityBfDead() → 销毁

TTL:
  游戏模式: MIN_SESSION_TTL_US = 4,000,000 µs (4 秒)
  视频模式: MIN_SESSION_TTL_US = 600,000 µs (0.6 秒)

RTT 测量:
  发端周期性发 RTT 测试包 (SendSetRecvRttPacket)
  收端回复 RTT 响应包 (SendRttTestResPacket)
  发端收到响应 → RttHandler() → 调整 rto_timeout_us_
```

---

## 7. 数据流

### 7.1 发送路径

```
App: GtpFrameSend(frame, size, tran_addr)
  │
  ├─ GoodTp::GetSession() ──── 查找或创建 GtpSession
  │
  ├─ session.CalcMaxFramePeriod() ── 更新帧间隔统计
  │
  ├─ session.FramePrepHandler()
  │     ├─ CalcHeaderSize()      计算可变头长度
  │     ├─ 写包头（ver/sn/type/size/ts...）
  │     ├─ pack_sn_++            分配发送序号
  │     └─ fec2.Encode(pack)     填 FEC 矩阵，满行/列发 FEC 冗余包
  │
  ├─ cb.send_pack_cb_() ──────── 回调：应用层通过 UDP 发出
  │
  └─ session.FramePostHandler()
        ├─ SnEntrySlidWin(data_win_s_, sn)  登记发送窗口
        └─ arq.PacketEntryList(pack)        加入 ARQ 等待确认链表
```

### 7.2 接收路径

```
App: GtpPacketReceive(pack, size, tran_addr)
  │
  ├─ GoodTp::GetSession()
  │
  └─ session.PackPrepHandler()
        ├─ 验证包头（版本/长度/类型/padding）
        ├─ RepeatPacketFilter(filter_win_, sn) ── 去重
        ├─ SnEntrySlidWin(data_win_r_, sn) ────── 登记接收窗口
        │
        ├─ [数据包] → cb.recv_frame_cb_() → 上报应用
        │             可靠流额外写入 sort_buf_ 按序上报
        │
        ├─ [FEC包]  → fec2.Decode()
        │               └─ TryRecoveryPackByFecPack()
        │                   恢复丢失包 → PackPrepHandler(恢复包)
        │
        └─ [ACK/NACK] → arq.ProcAck() / arq.ProcNack() → 触发重传

定时器 (session.TimerHandler):
  ├─ arq.CheckRtoRetran()          RTO 超时重传扫描
  ├─ 生成 ACK/NACK bitmap 发对端
  ├─ fac_.AdjustFactor()           拥塞因子更新
  └─ fec2.ClearResource()          清理过期 FEC 缓存
```

---

## 8. 配置参数

通过 `APPLICATION_TYPE` 宏在编译时选择参数集（`goodtp_macrodefine.h`）：

| 参数 | 游戏加速 (TYPE=2) | 视频/通用 (TYPE=0/1) |
|------|:-----------------:|:-------------------:|
| `SLID_WIN_SIZE` | 2048 包 | 16382 包 |
| `MAX_FEC2_CACHE_CAPACITY` | 32 | 256 |
| `MAX_RECV_FEC2_MATRIX_NUM` | 16 | — |
| `MAX_SESSION_NUM`（服务端 Linux） | 1024 | 1024 |
| `MAX_SESSION_NUM`（客户端） | 128 | 128 |
| `MIN_SESSION_TTL_US` | 4,000,000 µs | 600,000 µs |
| `DEFAULT_FEC2_BOOK_ID` | 4（2×2 双向） | 2（4×1 水平） |
| `MAX_SUPPORT_PPS` | 20,000 | 60,000 |
| `MAX_REALTIME_LOSS_THRESHLD` | 50% | 70% |
| `MAX_RELIABLE_LOSS_THRESHLD` | 80% | 90% |
| `ARQ_SPECS`（服务端） | 11 | 11 |
| `WIN_SPECS` | 3 | 6 |

---

## 9. 公共 API

```c
/* ── 实例管理 ── */
GtpHandler_p GtpCreate(GtpInitParam *param);
void         GtpDestroy(GtpHandler_p gtp_hdl);

/* ── 内存管理（零拷贝接口）── */
u8*  GtpMallocPackMem(GtpHandler_p gtp_hdl,
                      u8* old_pack_mem, u32 used_size,
                      u32 *new_mem_usable_size,
                      void **tran_addr_mem, u32 pack_len);
void GtpFreePackMem(GtpHandler_p gtp_hdl, u8 *pack_mem);

/* ── 发送 ── */
u32  GtpFrameSend(GtpHandler_p gtp_hdl,
                  void *frame, u32 size,
                  GtpAddr *tran_addr, u32 token, u32 token_id);

/* ── 接收 ── */
u32  GtpPacketReceive(GtpHandler_p gtp_hdl,
                      u8 *pack, u32 pack_size,
                      GtpAddr *tran_addr);

/* ── 诊断 ── */
u32  GtpShowBitMap(GtpHandler_p gtp_hdl, ...);
u32  GtpShowLinker(GtpHandler_p gtp_hdl, ...);
u32  GtpShowAlgorithmParam(GtpHandler_p gtp_hdl, ...);
```

**回调函数集（`GtpCallBackParam`）**：

| 回调 | 触发时机 |
|------|---------|
| `send_pack_cb_` | 有包需要通过 UDP 发出时 |
| `recv_frame_cb_` | 成功还原一帧用户数据时 |
| `write_log_cb_` | 库内部日志输出 |
| `get_cur_ts_us_cb_` | 获取当前时间戳（µs） |

---

## 10. 设计原则与约束

### 零拷贝
`GtpMallocPackMem` 支持传入 `old_pack_mem + used_size`，在同一内存块剩余空间中连续分配，应用层可直接向内存池申请缓冲区写入数据后发送，无需额外拷贝。

### 单线程约束
`CheckSingleThreadCalling()` 检测同一实例被多线程调用（警告不强制），库的设计预期每个 `GoodTp` 实例由单一线程驱动，线程安全性由调用方保证。多路并发请创建多个实例（最多 64 个）。

### 版本兼容
当前 wire 协议只接受 `GTP_VERSION`。发包版本由 `GtpMakePacketVersion()` 统一生成，收包由 `GtpIsSupportedPacketVersion()` 统一校验。后续需要兼容新版本时，只扩展版本 helper，不在收发热路径散落兼容分支。

### 混合可靠性模式
- **实时流（kRealTimeStream）**：FEC + ARQ 并行，容忍少量丢包换取低延迟
- **可靠流（kReliableStream）**：严格 ARQ + 排序缓冲，等收齐连续序号后按序交付，不容忍丢包

### 内存安全
- 每个内存块两端设置魔数（`0x5a` / `0x5aa55aa5`），定期检测越界
- 引用计数防止提前释放
- 超时强制回收防止内存泄漏（3s）
