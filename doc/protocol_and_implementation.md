# KCP 协议原理与实现详解

## 1. KCP 协议概述

KCP (Kinetic Communication Protocol) 是一种基于 UDP 的快速可靠传输协议，实现了增强型的 ARQ (Automatic Repeat reQuest) 协议。

### 1.1 协议定位

```
KCP 在协议栈中的位置:

  应用数据
     │
     ▼
  ┌─────────────────────────────┐
  │         KCP 层               │
  │  ┌───────────────────────┐  │
  │  │  可靠传输 (ARQ)        │  │
  │  │  • 分段/重组           │  │
  │  │  • 序列号管理           │  │
  │  │  • ACK 确认             │  │
  │  │  • 超时重传             │  │
  │  │  • 快速重传             │  │
  │  │  • 拥塞控制             │  │
  │  │  • 流控                 │  │
  │  └───────────────────────┘  │
  └─────────────────────────────┘
     │
     ▼ (编码为二进制数据包)
  UDP 数据报
     │
     ▼
  IP 网络
```

### 1.2 与传统 TCP 的本质区别

| 维度 | TCP | KCP |
|------|-----|-----|
| **设计哲学** | 最大化带宽利用 | 最小化传输延迟 |
| **拥塞响应** | 保守退让 | 快速恢复 |
| **RTO 计算** | RTO × 2 | 快速模式下 RTO × 1.5 |
| **重传策略** | SACK 选择性重传 | 选择性重传 + 快速重传 |
| **ACK** | 延迟 ACK (最大 200ms) | 即时 ACK |
| **流控** | 标准退让 | 可关闭 |

---

## 2. 协议报文格式

### 2.1 报文结构

KCP 只有一种报文类型，数据报和控制报文字段相同：

```
 0                   4                   8                  12
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                        Conversation ID (conv)                 |
|                                                               |
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      Command (cmd)    |  Fragment (frg)   |   Window Size(wnd)|
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                          Timestamp (ts)                       |
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                       Serial Number (sn)                      |
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                 Unacknowledged Serial Number (una)            |
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                            Data Length (len)                  |
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                                               |
|                   Data (variable length)                      |
|                                                               |
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

**头部固定 24 字节**:

| 字段 | 偏移 | 大小 | 说明 |
|------|------|------|------|
| conv | 0 | 4 bytes | 会话 ID，标识一个 KCP 连接 |
| cmd | 4 | 1 byte | 命令类型 (81-84) |
| frg | 5 | 1 byte | 分片数 (0=单段或未分片) |
| wnd | 6 | 2 bytes | 接收窗口大小 |
| ts | 8 | 4 bytes | 时间戳 (毫秒) |
| sn | 12 | 4 bytes | 序列号 |
| una | 16 | 4 bytes | 远端期望接收的下一个序列号 |
| len | 20 | 4 bytes | 数据长度 |

### 2.2 命令类型 (cmd)

| 命令值 | 常量名 | 说明 |
|--------|--------|------|
| 81 | IKCP_CMD_PUSH | 推送数据段 (携带业务数据) |
| 82 | IKCP_CMD_ACK | 确认段 (不携带数据) |
| 83 | IKCP_CMD_WASK | 窗口探测请求 |
| 84 | IKCP_CMD_WINS | 窗口通知 |

**cmd 编码**: 使用 1 字节，高 4 位固定为 5 (0x5x)，低 4 位为命令号。实际上代码中直接使用 81-84。

### 2.3 分片机制 (frg)

当发送数据超过 MSS 时，数据被分成多个片段：

```
原始数据: [========== 3000 bytes ============]

发送端分片:
  片段0: [==== 1376 bytes ===] frg=2  → 需要等待片段1,2 才能交付
  片段1: [==== 1376 bytes ===] frg=1  → 需要等待片段2 才能交付
  片段2: [==== 248 bytes ===]  frg=0  → 最后一个片段

接收端重组:
  按 sn 排序，只有当 frg=0 的片段到达且之前所有片段都已收到，
  才将重组后的数据放入接收队列交付给应用层。
```

### 2.4 序列化/反序列化

KCP 使用小端序 (LSB first) 序列化：

```c
// 32 位整数编码 (小端序)
char* ikcp_encode32u(char* p, IUINT32 l) {
#if IWORDS_BIG_ENDIAN || IWORDS_MUST_ALIGN
    *(unsigned char*)(p + 0) = (l >>  0) & 0xff;
    *(unsigned char*)(p + 1) = (l >>  8) & 0xff;
    *(unsigned char*)(p + 2) = (l >> 16) & 0xff;
    *(unsigned char*)(p + 3) = (l >> 24) & 0xff;
#else
    memcpy(p, &l, 4);  // 小端序直接拷贝
#endif
    return p + 4;
}
```

---

## 3. 核心算法

### 3.1 RTT 估算与 RTO 计算

KCP 使用改进的 TCP RTT 估算算法 (类似 Jacobson/Karels 算法):

```c
void ikcp_update_ack(ikcpcb *kcp, IINT32 rtt)
{
    if (kcp->rx_srtt == 0) {
        // 第一次测量
        kcp->rx_srtt = rtt;
        kcp->rx_rttval = rtt / 2;
    } else {
        // alpha = 7/8, beta = 1/4 (接近 TCP 标准值)
        long delta = rtt - kcp->rx_srtt;
        if (delta < 0) delta = -delta;  // |rtt - srtt|
        
        kcp->rx_rttval = (3 * kcp->rx_rttval + delta) / 4;  // 1/4 alpha
        kcp->rx_srtt = (7 * kcp->rx_srtt + rtt) / 8;        // 7/8 alpha
        
        if (kcp->rx_srtt < 1) kcp->rx_srtt = 1;
    }
    
    // RTO = srtt + max(interval, 4 * rttval)
    IINT32 rto = kcp->rx_srtt + _imax_(kcp->interval, 4 * kcp->rx_rttval);
    kcp->rx_rto = _ibound_(kcp->rx_minrto, rto, IKCP_RTO_MAX);
}
```

**参数默认值**:

| 参数 | 默认值 | 说明 |
|------|--------|------|
| IKCP_RTO_NDL | 30ms | 快速模式最小 RTO |
| IKCP_RTO_MIN | 100ms | 普通模式最小 RTO |
| IKCP_RTO_DEF | 200ms | 默认 RTO |
| IKCP_RTO_MAX | 60000ms | 最大 RTO (60秒) |
| rx_minrto | 100ms | 用户可调整的最小 RTO |

**nodelay 模式对 RTO 的影响**:

```
模式 0 (默认):
  rx_minrto = 100ms
  RTO 增长: RTO = RTO × 2 (每次超时翻倍)

模式 1/2 (快速):
  rx_minrto = 30ms
  RTO 增长: RTO = RTO + RTO/2 (每次超时 ×1.5)
```

### 3.2 拥塞控制算法

KCP 实现了类似 TCP Reno 的拥塞控制，包含慢启动和拥塞避免:

```
                    有 ACK 到达
                       │
                       ▼
              snd_una > prev_una?
               │          │
              YES         NO
               │          │
               ▼          │
         cwnd < rmt_wnd? │
          │      │       │
         YES     NO      │
          │      │       │
          ▼      │       │
     cwnd < ssthresh?   │
      │        │        │
     YES       NO       │
      │        │        │
      ▼        ▼        │
  慢启动    拥塞避免     │
  cwnd++   cwnd +=     │
  incr+=   mss²/incr   │
  mss     + mss/16     │
      │        │        │
      └───┬────┘        │
           │             │
           ▼             │
     cwnd > rmt_wnd?     │
      │        │         │
     YES       NO        │
      │        │         │
      ▼        ▼         │
  cwnd=rmt_wnd  继续     │
  incr=rmt×mss 增长      │
                       │
```

#### 慢启动 (Congestion < ssthresh)

```c
if (kcp->cwnd < kcp->ssthresh) {
    kcp->cwnd++;        // 每 RTT 增加 1
    kcp->incr += mss;   // 可增加的数据量
}
```

每次 ACK 确认一个新的数据包，cwnd 就加 1，是**指数增长**。

#### 拥塞避免 (Congestion >= ssthresh)

```c
else {
    if (kcp->incr < mss) kcp->incr = mss;
    kcp->incr += (mss * mss) / kcp->incr + (mss / 16);
    if ((kcp->cwnd + 1) * mss <= kcp->incr) {
        kcp->cwnd = (kcp->incr + mss - 1) / mss;
    }
}
```

每 RTT 增长约 `1/cwnd` 个段，是**线性增长**。

#### 快速恢复 (Fast Recovery)

**快速重传触发时 (change != 0)**:

```c
if (change) {
    IUINT32 inflight = kcp->snd_nxt - kcp->snd_una;
    kcp->ssthresh = inflight / 2;
    if (kcp->ssthresh < IKCP_THRESH_MIN)
        kcp->ssthresh = IKCP_THRESH_MIN;  // 最小值 2
    kcp->cwnd = kcp->ssthresh + resend;
    kcp->incr = kcp->cwnd * kcp->mss;
}
```

**超时重传触发时 (lost != 0)**:

```c
if (lost) {
    kcp->ssthresh = cwnd / 2;
    if (kcp->ssthresh < IKCP_THRESH_MIN)
        kcp->ssthresh = IKCP_THRESH_MIN;
    kcp->cwnd = 1;       // 回退到慢启动
    kcp->incr = kcp->mss;
}
```

### 3.3 快速重传 (Fast Retransmit)

KCP 的快速重传机制：

```
发送端发送: [1] [2] [3] [4] [5]
                    ↓
接收端收到: [1] [3] [4] [5]  ← 跳过 [2]
    │        │   │   │   │
    │        ▼   ▼   ▼   ▼
    │      ACK1 ACK3 ACK4 ACK5
    │               ↑    ↑
    │            收到3: 2被跳过1次 → fastack=1
    │            收到4: 2被跳过2次 → fastack=2
    │                         ↓
    │                    fastack >= resend(2)
    │                         ↓
    └────────── 重传 [2] ← 不等超时!
```

**代码实现**:

```c
static void ikcp_parse_fastack(ikcpcb *kcp, IUINT32 sn, IUINT32 ts)
{
    for (p = kcp->snd_buf.next; p != &kcp->snd_buf; p = next) {
        seg = iqueue_entry(p, IKCPSEG, node);
        if (_itimediff(sn, seg->sn) < 0) break;
        else if (sn != seg->sn) {
            #ifdef IKCP_FASTACK_CONSERVE
                // 保守模式: 只统计时间戳比当前段旧的
                if (_itimediff(ts, seg->ts) >= 0)
                    seg->fastack++;
            #else
                seg->fastack++;  // 非保守模式: 每次 ACK 都计数
            #endif
        }
    }
}
```

**在 ikcp_flush 中判断**:

```c
resent = (kcp->fastresend > 0) ? (IUINT32)kcp->fastresend : 0xffffffff;

if (segment->fastack >= resent) {
    // 快速重传
    needsend = 1;
    segment->xmit++;
    segment->fastack = 0;
    segment->resendts = current + segment->rto;
    change++;  // 标记拥塞窗口需要更新
}
```

### 3.4 窗口探测 (Window Probe)

当远端接收窗口为 0 时，KCP 会启动窗口探测：

```
初始探测: 7000ms (IKCP_PROBE_INIT)
后续探测: 每次翻倍，但不超过 120000ms (IKCP_PROBE_LIMIT)

ts_probe = current + probe_wait
    ↓ 等待 probe_wait 毫秒
发送 WASK 命令
    ↓ 等待 probe_wait 毫秒 (翻倍)
再发送 WASK...
    ↓
直到收到 WINS 回复，probe_wait 重置
```

### 3.5 死连接检测

```c
if (segment->xmit >= kcp->dead_link) {
    kcp->state = (IUINT32)-1;  // 标记连接断开
}
```

默认 `dead_link = 20`，即一个包重传 20 次仍未确认，认为连接已断。

---

## 4. 工作流程详解

### 4.1 发送流程

```
ikcp_send(buffer, len)
    │
    ├─ [流模式优化] 尝试合并到前一段
    │   if (stream != 0 && snd_queue 非空) {
    │       old = snd_queue 最后一个段
    │       if (old->len < mss) {
    │           追加数据到 old
    │           删除 old (内存重用)
    │       }
    │   }
    │
    ├─ [分段判断]
    │   count = ceil(len / mss)
    │   if (count >= rcv_wnd) return -2;  // 窗口太小
    │
    └─ [创建段]
        for (i = 0; i < count; i++) {
            seg = 新段 (size = min(mss, 剩余长度))
            memcpy(seg->data, buffer, seg->len)
            seg->frg = (count - i - 1)  // 剩余片数
            iqueue_add_tail(seg, &snd_queue)
            nsnd_que++
        }
        返回实际发送的字节数
```

### 4.2 刷新流程 (ikcp_flush)

这是 KCP 最核心的函数，负责所有发送相关操作：

```
ikcp_flush()
    │
    │  ┌─── 阶段 1: 发送 ACK ───┐
    │   for (每个待发的 ACK):
    │       seg.sn = ack[i].sn
    │       seg.ts = ack[i].ts
    │       如果 + 包头 > MTU → 先输出
    │       编码 seg 到 buffer
    │   清空 ackcount
    │  └─────────────────────────┘
    │
    │  ┌─── 阶段 2: 窗口探测 ───┐
    │   if (rmt_wnd == 0) {
    │       if (到达探测时间) {
    │           probe_wait *= 1.5 (翻倍)
    │           ts_probe = current + probe_wait
    │           probe |= ASK_SEND  // 准备发 WASK
    │       }
    │   }
    │  └─────────────────────────┘
    │
    │  ┌─── 阶段 3: 窗口通知 ───┐
    │   if (probe & ASK_TELL) {
    │       seg.cmd = WINS
    │       编码并输出
    │   }
    │   probe = 0
    │  └─────────────────────────┘
    │
    │  ┌─── 阶段 4: 移动数据 ───┐
    │   cwnd = min(snd_wnd, rmt_wnd, [拥塞控制? cwnd : ∞])
    │   while (snd_nxt < snd_una + cwnd && snd_queue 非空) {
    │       从 snd_queue 移一段到 snd_buf
    │       初始化:
    │         seg->cmd = PUSH
    │         seg->ts = current
    │         seg->sn = snd_nxt++
    │         seg->resendts = current + rto
    │         seg->rto = rx_rto
    │         seg->xmit = 0
    │         seg->fastack = 0
    │   }
    │  └─────────────────────────┘
    │
    │  ┌─── 阶段 5: 数据发送/重传 ──┐
    │   for (每个在 snd_buf 的段):
    │       needsend = false
    │
    │       if (xmit == 0) {
    │           needsend = true  // 首次发送
    │           xmit = 1
    │           resendts = current + rto + rtomin
    │       }
    │       else if (current >= resendts) {
    │           needsend = true  // 超时重传
    │           xmit++
    │           if (nodelay) rto += rto/2   // ×1.5
    │           else         rto += rto/3   // ×1.33 (近似×2)
    │           resendts = current + rto
    │           lost = 1
    │       }
    │       else if (fastack >= resent) {
    │           needsend = true  // 快速重传
    │           xmit++
    │           fastack = 0
    │           resendts = current + rto
    │           change = 1
    │       }
    │
    │       if (needsend) {
    │           编码段 + 数据到 buffer
    │           if (+ 新段 > MTU → 先输出
    │           if (xmit >= dead_link) state = -1
    │       }
    │   }
    │  └─────────────────────────┘
    │
    │  ┌─── 阶段 6: 输出剩余数据 ──┐
    │   if (buffer 有数据) 输出
    │  └─────────────────────────┘
    │
    │  ┌─── 阶段 7: 更新拥塞窗口 ──┐
    │   if (change) {
    │       ssthresh = inflight / 2
    │       cwnd = ssthresh + resend
    │   }
    │   if (lost) {
    │       ssthresh = cwnd / 2
    │       cwnd = 1  // 慢启动
    │   }
    │   if (cwnd < 1) cwnd = 1
    │  └─────────────────────────┘
```

### 4.3 接收流程 (ikcp_input)

```
ikcp_input(data, size)
    │
    ├─ 验证 conv (不匹配直接丢弃)
    │
    └─ 循环解析多个段:
        ├─ 解码 24 字节头部
        │   conv, cmd, frg, wnd, ts, sn, una, len
        │
        ├─ rmt_wnd = wnd  // 更新远端窗口
        │
        ├─ 处理 UNA:
        │   ikcp_parse_una(una)  // 释放已确认段
        │   ikcp_shrink_buf()    // 更新 snd_una
        │
        ├─ 根据 cmd 分支处理:
        │   │
        │   ├── IKCP_CMD_ACK:
        │   │   ├─ 计算 RTT = current - ts
        │   │   ├─ update_ack(RTT)
        │   │   ├─ parse_ack(sn)  // 标记段已确认
        │   │   └─ 记录最大 ACK 用于快速重传
        │   │
        │   ├── IKCP_CMD_PUSH:
        │   │   ├─ ack_push(sn, ts)  // 告知发送方已收到
        │   │   ├─ 如果 sn >= rcv_nxt:
        │   │   │   ├─ 创建新段 IKCPSEG
        │   │   │   ├─ 复制 payload 数据
        │   │   │   └─ parse_data(seg)
        │   │   │       ├─ 检查 sn 范围
        │   │   │       ├─ 如果重复 → 删除
        │   │   │       ├─ 否则插入 rcv_buf 有序位置
        │   │   │       └─ 移动连续数据到 rcv_queue
        │   │   └─ 移动 rcv_buf → rcv_queue
        │   │
        │   ├── IKCP_CMD_WASK:
        │   │   └─ probe |= ASK_TELL
        │   │
        │   └── IKCP_CMD_WINS:
        │       └─ (窗口信息已在上面处理)
        │
        └─ 数据指针前进

    ├─ parse_fastack(maxack, latest_ts)
    │
    └─ 拥塞控制更新
        if (snd_una > prev_una) {
            增长 cwnd (慢启动或拥塞避免)
        }
```

---

## 5. 工作模式详解

### 5.1 ikcp_nodelay 的四种模式

```c
ikcp_nodelay(kcp, nodelay, interval, resend, nc)
```

| 模式 | nodelay | interval | resend | nc | 适用场景 |
|------|---------|----------|--------|-----|----------|
| 默认模式 | 0 | 40 | 0 | 0 | 类似 TCP，通用场景 |
| 普通模式 | 0 | 10 | 0 | 1 | 关闭拥塞控制 |
| 快速模式 | 1 | 10 | 2 | 1 | 游戏、实时通信 |
| 极速模式 | 2 | 10 | 2 | 1 | 延迟极度敏感 |

**各模式关键差异**:

```
模式 0 (默认):
  最小 RTO  = 100ms
  RTO 增长  = ×2 (超时后翻倍)
  快速重传  = 关闭
  拥塞控制  = 启用

模式 1/2 (快速):
  最小 RTO  = 30ms
  RTO 增长  = ×1.5 (超时后增加一半)
  快速重传  = 启用 (resend>0 时)
  拥塞控制  = 由 nc 决定
```

### 5.2 流模式 (Stream Mode)

```c
kcp->stream = 1;  // 启用流模式
```

流模式下，连续的小发送会被合并到一个段中：

```
常规模式:
  send("A", 1)  → 段1 (1字节 + 24字节头部)
  send("B", 1)  → 段2 (1字节 + 24字节头部)
  总计: 50 字节

流模式:
  send("A", 1)  → 段1 (已发送，未满)
  send("B", 1)  → 追加到段1 → 段1 (2字节 + 24字节头部)
  总计: 26 字节，节省 24 字节头部!
```

**注意事项**:
- 流模式下 `frg` 始终为 0
- 接收端按字节流交付，不保留消息边界
- 需要应用层自行处理消息定界

---

## 6. 测试程序分析

### 6.1 测试架构

测试程序包含一个完整的 UDP 网络模拟器和三种模式的性能对比：

```
┌────────────────────────────────────────────┐
│                test.cpp                     │
│                                             │
│  kcp1 (conv=0x11223344, user=0)            │
│       │  output →                           │
│       ▼                                     │
│  LatencySimulator (丢包10%, RTT 60-125ms)  │
│       │  output →                           │
│       ▼                                     │
│  kcp2 (conv=0x11223344, user=1)            │
│                                             │
│  数据流: kcp1 → 网络 → kcp2 → 回射 →       │
│                网络 → kcp1                  │
└────────────────────────────────────────────┘
```

### 6.2 LatencySimulator 模拟器

测试程序实现了一个带延迟和丢包的虚拟网络模拟器：

```cpp
class LatencySimulator {
    // 参数:
    // lostrate: 单程丢包率 (传入值/2)
    // rttmin:   最小单程延迟
    // rttmax:   最大单程延迟
    // nmax:     最大缓存包数 (1000)
    
    DelayTunnel p12;  // 0→1 方向的延迟队列
    DelayTunnel p21;  // 1→0 方向的延迟队列
    Random r12;       // 0→1 方向丢包随机数
    Random r21;       // 1→0 方向丢包随机数
    
    void send(int peer, data, size) {
        // peer=0: 从0发送到1
        // 随机丢包 (基于 lostrate)
        // 计算延迟: rttmin + random(rttmax-rttmin)
        // 设置到达时间戳
        // 放入对应方向的延迟队列
    }
    
    int recv(int peer, data, maxsize) {
        // peer=0: 接收来自1的数据
        // 检查当前时间是否 >= 包的到达时间
        // 到达则取出并复制数据
    }
};
```

### 6.3 测试用例

```cpp
void test(int mode) {
    // 创建模拟网络: 10%丢包, 60-125ms RTT
    vnet = new LatencySimulator(10, 60, 125);
    
    // 创建两个 KCP 端点
    ikcpcb *kcp1 = ikcp_create(0x11223344, (void*)0);
    ikcpcb *kcp2 = ikcp_create(0x11223344, (void*)1);
    
    kcp1->output = udp_output;
    kcp2->output = udp_output;
    
    // 配置窗口
    ikcp_wndsize(kcp1, 128, 128);
    ikcp_wndsize(kcp2, 128, 128);
    
    // 设置模式
    if (mode == 0)      // 默认模式
        ikcp_nodelay(kcp1, 0, 10, 0, 0);
    else if (mode == 1) // 普通模式
        ikcp_nodelay(kcp1, 0, 10, 0, 1);
    else                // 快速模式
        ikcp_nodelay(kcp1, 2, 10, 2, 1);
        kcp1->rx_minrto = 10;
        kcp1->fastresend = 1;
    
    // 主循环
    while (next < 1000) {
        ikcp_update(kcp1, now);
        ikcp_update(kcp2, now);
        
        // kcp1 每 20ms 发送一个包
        ikcp_send(kcp1, buffer, 8);
        
        // 处理网络延迟
        vnet->recv(1, buffer);  // 0→1
        vnet->recv(0, buffer);  // 1→0
        
        // kcp2 回射
        ikcp_recv(kcp2, buf);
        ikcp_send(kcp2, buf);
        
        // kcp1 接收回射
        ikcp_recv(kcp1, buf);
    }
    
    // 打印统计
    printf("avgrtt=%d maxrtt=%d tx=%d\n", avg_rtt, max_rtt, tx_count);
}
```

### 6.4 测试结果

```
default mode result (20917ms):
  avgrtt=740ms  maxrtt=1507ms
  
normal mode result (20131ms):
  avgrtt=156ms  maxrtt=571ms
  
fast mode result (20207ms):
  avgrtt=138ms  maxrtt=392ms
```

**分析**:
- 默认模式下，RTO 翻倍 + 无快速重传导致 RTT 严重放大 (740ms vs 实际~100ms)
- 普通模式关闭了拥塞控制，延迟大幅降低
- 快速模式在普通模式基础上增加了快速重传等优化，进一步降低最大延迟

---

## 7. 协议常量汇总

### 7.1 时间相关常量

| 常量 | 值 | 单位 | 说明 |
|------|----|------|------|
| IKCP_RTO_NDL | 30 | ms | 快速模式最小 RTO |
| IKCP_RTO_MIN | 100 | ms | 普通模式最小 RTO |
| IKCP_RTO_DEF | 200 | ms | 默认 RTO |
| IKCP_RTO_MAX | 60000 | ms | 最大 RTO (60秒) |
| IKCP_INTERVAL | 100 | ms | 默认更新间隔 |
| IKCP_DEADLINK | 20 | - | 死连接判定重传次数 |
| IKCP_PROBE_INIT | 7000 | ms | 初始探测间隔 |
| IKCP_PROBE_LIMIT | 120000 | ms | 最大探测间隔 |

### 7.2 窗口/大小常量

| 常量 | 值 | 说明 |
|------|----|------|
| IKCP_WND_SND | 32 | 默认发送窗口 |
| IKCP_WND_RCV | 128 | 默认接收窗口 |
| IKCP_MTU_DEF | 1400 | 默认 MTU |
| IKCP_OVERHEAD | 24 | 协议头部开销 |
| IKCP_ACK_FAST | 3 | 快速 ACK 计数 |
| IKCP_THRESH_INIT | 2 | 初始 ssthresh |
| IKCP_THRESH_MIN | 2 | 最小 ssthresh |
| IKCP_FASTACK_LIMIT | 5 | 快速重传上限 |

### 7.3 命令常量

| 常量 | 值 | 说明 |
|------|----|------|
| IKCP_CMD_PUSH | 81 | 推送数据 |
| IKCP_CMD_ACK | 82 | 确认 |
| IKCP_CMD_WASK | 83 | 窗口探测请求 |
| IKCP_CMD_WINS | 84 | 窗口通知 |
| IKCP_ASK_SEND | 1 | 需要发送 WASK |
| IKCP_ASK_TELL | 2 | 需要发送 WINS |

---

## 8. 与其他协议对比

### 8.1 对比 TCP

```
TCP 数据包:
  +--------+--------+--------+--------+
  |  Header(20-60 bytes)             |
  |  Src/Dst Port, Seq, Ack, ...    |
  +--------+--------+--------+--------+
  |            Data                   |
  +--------+--------+--------+--------+

KCP 数据包:
  +--------+--------+--------+--------+
  | conv(4)|cmd(1)|frg(1)|wnd(2)|    | ← 24 bytes header
  | ts(4)  | sn(4) | una(4)|len(4)|   |
  +--------+--------+--------+--------+
  |            Data                   |
  +--------+--------+--------+--------+

对比:
  - TCP 头部最小 20 字节 + 数据
  - KCP 头部固定 24 字节 + 数据
  - KCP 头部更大但功能更精简
  - KCP 不需要端口号 (由 conv 替代)
```

### 8.2 对比 RakNet/enet

| 特性 | KCP | RakNet | enet |
|------|-----|--------|------|
| 代码量 | 1300行 | ~50000行 | ~30000行 |
| 文件数 | 2 | 50+ | 30+ |
| 可集成性 | 拷贝即用 | 需完整链接 | 需完整链接 |
| 可定制性 | 极高 | 中等 | 低 |
| 协议扩展 | 自由 | 受限 | 受限 |
| 学习成本 | 低 | 高 | 高 |

### 8.3 对比 QUIC

| 特性 | KCP | QUIC (HTTP/3) |
|------|-----|---------------|
| 传输层 | 任何 UDP | UDP |
| 加密 | 无 (需自行添加) | 内置 TLS 1.3 |
| 多路复用 | 无 (需自行实现) | 内置 |
| 连接迁移 | 无 | 支持 (Connection ID) |
| 头部开销 | 24 字节 | 约 7-20 字节 |
| 实现复杂度 | 极低 | 高 |
| 适用场景 | 嵌入式/游戏/游戏 | Web/通用 |

---

## 9. 典型集成方案

### 9.1 基本集成

```cpp
// 1. 创建
ikcpcb* kcp = ikcp_create(conv, user_ptr);

// 2. 设置输出
kcp->output = my_udp_output;

// 3. 配置 (推荐快速模式)
ikcp_nodelay(kcp, 1, 10, 2, 1);
ikcp_wndsize(kcp, 128, 128);

// 4. 主循环
while (true) {
    ikcp_update(kcp, current_ms);
    
    // 发送 UDP
    char buf[MTU];
    int n = kcp_get_output(buf, sizeof(buf));  // 伪代码
    if (n > 0) sendto(socket, buf, n);
    
    // 接收 UDP
    int n = recvfrom(socket, buf, sizeof(buf));
    if (n > 0) ikcp_input(kcp, buf, n);
}

// 5. 发送/接收
ikcp_send(kcp, data, len);
int len = ikcp_recv(kcp, buf, sizeof(buf));
```

### 9.2 多线程场景

```cpp
// 方案: 锁保护 + 单线程事件循环
std::mutex kcp_mutex;

void on_udp_receive() {
    std::lock_guard<std::mutex> lock(kcp_mutex);
    ikcp_input(kcp, buf, len);
}

void on_user_send() {
    std::lock_guard<std::mutex> lock(kcp_mutex);
    ikcp_send(kcp, data, len);
}

void update_loop() {
    while (running) {
        std::lock_guard<std::mutex> lock(kcp_mutex);
        ikcp_update(kcp, current_ms);
        // 处理 output 回调中的 UDP 发送...
    }
}
```

### 9.3 事件驱动集成

```cpp
// 结合 epoll/select
void event_loop(int epollfd) {
    while (true) {
        IUINT32 now = get_current_ms();
        IUINT32 next_tick = ikcp_check(kcp, now);
        int timeout = (next_tick > now) ? (int)(next_tick - now) : 0;
        
        int n = epoll_wait(epollfd, events, MAX, timeout);
        
        for (int i = 0; i < n; i++) {
            if (events[i].fd == udp_socket) {
                // 接收 UDP
                int len = recvfrom(...);
                ikcp_input(kcp, buf, len);
            }
        }
        
        // 到达 KCP 更新时间
        now = get_current_ms();
        if (now >= ikcp_check(kcp, now)) {
            ikcp_update(kcp, now);
        }
    }
}
```

---

## 10. 常见问题与注意事项

### 10.1 conv 管理

conv 是 KCP 连接的唯一标识，通信双方必须一致。推荐方案：

```cpp
// 方案1: 使用连接索引
uint16_t local_index = allocate_local_index();
uint16_t remote_index = get_remote_index();
uint32_t conv = (local_index << 16) | remote_index;

// 方案2: 使用端口+IP哈希
uint32_t conv = hash(ip, port);
```

### 10.2 MTU 设置

```cpp
// 默认 MTU=1400，可根据网络环境调整
// 以太网标准 MTU 是 1500，扣除 IP(20) + UDP(8) = 28
// 剩余 1472，KCP 头部 24，数据最大 1448
// 所以 MTU=1400 是安全值

ikcp_setmtu(kcp, 1400);  // 标准以太网上
ikcp_setmtu(kcp, 576);   // 低速网络/移动网络
```

### 10.3 时间戳溢出处理

```cpp
// KCP 使用 IINT32 做时间差计算，自动处理 32 位溢出
static inline long _itimediff(IUINT32 later, IUINT32 earlier) 
{
    return ((IINT32)(later - earlier));
}

// 32 位时间戳在 49.7 天后溢出
// 但由于使用有符号差值计算，溢出后依然正确
// 例如: earlier=0xFFFFFFF0, later=0x00000010
// diff = (IINT32)(0x00000010 - 0xFFFFFFF0) = 0x20 = 32
```

### 10.4 性能调优建议

```cpp
// 游戏服务器推荐配置
ikcp_nodelay(kcp, 1, 10, 2, 1);
ikcp_wndsize(kcp, 128, 128);
kcp->rx_minrto = 10;

// 大带宽传输推荐配置
ikcp_nodelay(kcp, 0, 40, 0, 0);
ikcp_wndsize(kcp, 512, 512);
ikcp_setmtu(kcp, 1400);

// 极低延迟场景
ikcp_nodelay(kcp, 2, 10, 2, 1);
ikcp_wndsize(kcp, 64, 64);
kcp->rx_minrto = 5;
```
