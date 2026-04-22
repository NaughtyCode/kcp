# KCP API 参考手册

## 概述

KCP 提供了 17 个公开 API 函数，分布在 `ikcp.h` 中声明，在 `ikcp.c` 中实现。所有函数都以 `ikcp_` 为前缀。

---

## 1. 对象创建与销毁

### 1.1 ikcp_create

**功能**: 创建一个新的 KCP 控制块对象

```c
ikcpcb* ikcp_create(IUINT32 conv, void *user);
```

**参数**:
| 参数 | 类型 | 说明 |
|------|------|------|
| conv | IUINT32 | 会话编号，通信双方必须一致 |
| user | void* | 用户自定义指针，会传递给 output 回调 |

**返回值**:
- 成功：返回 `ikcpcb*` 指针
- 失败：返回 `NULL`（内存分配失败）

**内部实现细节**:
```
1. 分配 IKCPCB 结构体内存
2. 初始化 conv, user
3. 初始化序列号: snd_una=0, snd_nxt=0, rcv_nxt=0
4. 初始化窗口: snd_wnd=32, rcv_wnd=128, rmt_wnd=128
5. 初始化 MTU=1400, mss=1400-24=1376
6. 初始化 RTT 相关: rx_rto=200, rx_minrto=100
7. 初始化定时器: interval=100, ts_flush=100
8. 初始化模式: nodelay=0, fastresend=0, nocwnd=0
9. 分配输出缓冲区: (mtu+24)*3 = 4632 字节
10. 初始化四个双向链表: snd_queue, rcv_queue, snd_buf, rcv_buf
11. 初始化 acklist = NULL, ackblock = 0
```

**默认值汇总**:
```c
snd_una = 0, snd_nxt = 0, rcv_nxt = 0
snd_wnd = 32,  rcv_wnd = 128, rmt_wnd = 128
cwnd = 0, mss = 1376, mtu = 1400
rx_srtt = 0, rx_rttval = 0, rx_rto = 200, rx_minrto = 100
ssthresh = 2, fastresend = 0, nocwnd = 0
nodelay = 0, updated = 0
interval = 100, ts_flush = 100
dead_link = 20
```

**使用示例**:
```cpp
// 创建 KCP 对象，会话号 0x11223344
ikcpcb *kcp = ikcp_create(0x11223344, (void*)0);
if (kcp == NULL) {
    // 处理分配失败
}
```

---

### 1.2 ikcp_release

**功能**: 释放 KCP 控制块对象及其所有资源

```c
void ikcp_release(ikcpcb *kcp);
```

**参数**:
| 参数 | 类型 | 说明 |
|------|------|------|
| kcp | ikcpcb* | 要释放的 KCP 对象指针 |

**内部实现**:
```
1. 清空 snd_buf，释放所有未确认段
2. 清空 rcv_buf，释放所有乱序段
3. 清空 snd_queue，释放所有待发送段
4. 清空 rcv_queue，释放所有待接收段
5. 释放输出缓冲区 kcp->buffer
6. 释放 ACK 列表 kcp->acklist
7. 释放 IKCPCB 自身
```

**注意**: 
- 释放后不能再使用该 kcp 对象的任何指针
- 不会触发任何回调

---

## 2. 回调设置

### 2.1 ikcp_setoutput

**功能**: 设置 KCP 的输出回调函数

```c
void ikcp_setoutput(ikcpcb *kcp, int (*output)(const char *buf, int len,
    ikcpcb *kcp, void *user));
```

**参数**:
| 参数 | 类型 | 说明 |
|------|------|------|
| kcp | ikcpcb* | KCP 对象指针 |
| output | function pointer | 输出回调函数指针 |

**回调函数签名**:
```c
int output(const char *buf, int len, ikcpcb *kcp, void *user);
```

**回调参数**:
| 参数 | 类型 | 说明 |
|------|------|------|
| buf | const char* | 待发送的数据缓冲区 |
| len | int | 数据长度（字节） |
| kcp | ikcpcb* | 当前 KCP 对象指针 |
| user | void* | 创建时传入的用户指针 |

**返回值**:
- 成功：返回 0 或实际发送的字节数
- 失败：返回负值

**实现细节**:
- `ikcp_output` 内部会先检查 `kcp->output` 是否为 NULL
- 如果启用了日志 (IKCP_LOG_OUTPUT)，会先记录日志
- 当 size == 0 时直接返回 0，不调用回调

**典型实现 (UDP 输出)**:
```cpp
int udp_output(const char *buf, int len, ikcpcb *kcp, void *user)
{
    // 通过 UDP socket 发送数据
    sendto(udp_socket, buf, len, 0, 
           (struct sockaddr*)&remote_addr, sizeof(remote_addr));
    return 0;
}

// 设置回调
kcp->output = udp_output;
// 或者
ikcp_setoutput(kcp, udp_output);
```

---

## 3. 数据发送

### 3.1 ikcp_send

**功能**: 向上层应用提供发送接口，将数据送入 KCP 发送队列

```c
int ikcp_send(ikcpcb *kcp, const char *buffer, int len);
```

**参数**:
| 参数 | 类型 | 说明 |
|------|------|------|
| kcp | ikcpcb* | KCP 对象指针 |
| buffer | const char* | 待发送数据缓冲区 |
| len | int | 待发送数据长度（字节） |

**返回值**:
| 值 | 说明 |
|----|------|
| >= 0 | 成功发送的字节数 |
| -1 | 参数错误 (len < 0) |
| -2 | 内存分配失败或发送窗口满 |

**内部流程**:

```
ikcp_send(buffer, len)
    │
    ├─ 检查 mss > 0
    ├─ 检查 len >= 0
    │
    ├─ [流模式 stream!=0] 尝试合并到上一个段
    │   └─ 如果上一个段未满，追加数据
    │
    ├─ 计算分段数 count = ceil(len / mss)
    ├─ 检查 count < rcv_wnd
    │
    └─ 循环创建段:
        ├─ 每段最大 mss 字节
        ├─ 设置 conv, cmd=IKCP_CMD_PUSH
        ├─ 设置 frg (fragment count - i - 1)
        │   流模式时 frg=0
        ├─ 添加到 snd_queue 尾部
        └─ nsnd_que++
```

**分段逻辑详解**:

当 `len > mss` 时，数据会被分成多个段：

```
发送数据: [========== data (3000 bytes) =========]
mss: 1376 bytes

分成 3 个段:
  段1: [==== 1376 bytes ====] frg=2  (还有2个段)
  段2: [==== 1376 bytes ====] frg=1  (还有1个段)
  段3: [==== 248 bytes =====] frg=0  (最后一个段)

接收端重组:
  收到段2, 段1 → 暂存 rcv_buf (等待段1)
  收到段1     → 按序放入 rcv_queue
  收到段3     → 按序放入 rcv_queue (因为 rcv_nxt 已推进)
```

**流模式 (stream mode)**:
```cpp
kcp->stream = 1;  // 设置为流模式

// 流模式下，连续的小数据会合并到一个段中发送
ikcp_send(kcp, "Hello", 5);
ikcp_send(kcp, " World", 6);
// 可能合并为一个段发送，节省头部开销
```

**重要约束**:
- 单次发送不能超过 `rcv_wnd * mss`
- 流模式下支持数据合并，减少小数据包的头部开销

---

## 4. 数据接收

### 4.1 ikcp_recv

**功能**: 从 KCP 接收队列中读取数据

```c
int ikcp_recv(ikcpcb *kcp, char *buffer, int len);
```

**参数**:
| 参数 | 类型 | 说明 |
|------|------|------|
| kcp | ikcpcb* | KCP 对象指针 |
| buffer | char* | 接收缓冲区 |
| len | int | 接收缓冲区长度 |

**返回值**:
| 值 | 说明 |
|----|------|
| >= 0 | 实际接收的字节数 |
| -1 | 接收队列为空 |
| -2 | 队列中第一条消息不完整 |
| -3 | 接收缓冲区太小 |

**内部流程**:

```
ikcp_recv(buffer, len)
    │
    ├─ 检查 rcv_queue 是否为空 → 返回 -1
    │
    ├─ 处理 len < 0 (peek 模式)
    │
    ├─ peeksize = ikcp_peeksize()
    │   ├─ 获取第一条完整消息的长度
    │   └─ 如果 frg > 0，需等待所有分片
    │
    ├─ 检查 peeksize < 0 → 返回 -2 (不完整)
    ├─ 检查 peeksize > len → 返回 -3 (缓冲区太小)
    │
    ├─ 检查是否需要恢复流控 (nrcv_que >= rcv_wnd)
    │
    ├─ [合并片段循环]:
    │   for (p = rcv_queue.next; p != &rcv_queue; )
    │       ├─ 复制数据到 buffer
    │       ├─ len += seg->len
    │       ├─ fragment = seg->frg
    │       └─ 如果 frg == 0，break (最后一个片段)
    │
    ├─ 释放已读取的段 (非 peek 模式)
    │
    └─ [移动 rcv_buf -> rcv_queue]:
        while (!rcv_buf.empty())
            ├─ 检查 seg->sn == rcv_nxt
            ├─ 检查 nrcv_que < rcv_wnd
            └─ 移动到 rcv_queue，rcv_nxt++
```

**Peek 模式**:
```cpp
// 只查看数据长度，不真正接收
int size = ikcp_recv(kcp, NULL, -len);
// 或者
int size = ikcp_peeksize(kcp);

if (size > 0) {
    char *buf = malloc(size);
    ikcp_recv(kcp, buf, size);  // 正式接收
}
```

---

### 4.2 ikcp_peeksize

**功能**: 查看接收队列中下一条消息的长度

```c
int ikcp_peeksize(const ikcpcb *kcp);
```

**参数**:
| 参数 | 类型 | 说明 |
|------|------|------|
| kcp | const ikcpcb* | KCP 对象指针 (只读) |

**返回值**:
| 值 | 说明 |
|----|------|
| >= 0 | 下一条完整消息的长度 |
| -1 | 队列为空或数据不完整 |
| -2 | 分片未收齐 |

**内部逻辑**:
```c
if (rcv_queue 为空) return -1;

seg = 第一个段;
if (seg->frg == 0) return seg->len;  // 单段消息

if (nrcv_que < frg + 1) return -1;  // 分片未收齐

// 累加所有片段长度
for (p = rcv_queue.next; p != &rcv_queue; p = p->next) {
    len += seg->len;
    if (seg->frg == 0) break;
}
return length;
```

---

## 5. 状态更新

### 5.1 ikcp_update

**功能**: 更新 KCP 状态，处理定时器触发的事件

```c
void ikcp_update(ikcpcb *kcp, IUINT32 current);
```

**参数**:
| 参数 | 类型 | 说明 |
|------|------|------|
| kcp | ikcpcb* | KCP 对象指针 |
| current | IUINT32 | 当前时间戳 (毫秒) |

**调用建议**:
- 每 10ms-100ms 调用一次
- 可以与业务逻辑的更新频率同步
- 不一定要定时调用，可以用 `ikcp_check` 来调度

**内部流程**:

```
ikcp_update(kcp, current)
    │
    ├─ kcp->current = current
    │
    ├─ if (updated == 0)  // 首次调用
    │   ├─ updated = 1
    │   └─ ts_flush = current
    │
    ├─ slap = current - ts_flush
    │
    ├─ 时间跳变过大 (>10秒 或 <-10秒)
    │   ├─ ts_flush = current
    │   └─ slap = 0
    │
    └─ if (slap >= 0)  // 到达刷新时间
        ├─ ts_flush += interval
        ├─ if (current >= ts_flush) ts_flush = current + interval
        └─ ikcp_flush(kcp)  // 核心刷新函数
```

**关键行为**:
1. 只有当距离上次 flush 达到 interval 时才真正执行 flush
2. 会处理超时重传、快速重传、ACK 发送等
3. 第一次调用时会标记 updated=1

---

### 5.2 ikcp_check

**功能**: 计算下次 ikcp_update 应该何时调用（用于事件驱动调度）

```c
IUINT32 ikcp_check(const ikcpcb *kcp, IUINT32 current);
```

**参数**:
| 参数 | 类型 | 说明 |
|------|------|------|
| kcp | const ikcpcb* | KCP 对象指针 (只读) |
| current | IUINT32 | 当前时间戳 (毫秒) |

**返回值**: 下次调用 ikcp_update 的时间戳 (毫秒)

**使用场景**: 实现 epoll/selector 类似的调度机制

```cpp
// 事件驱动调度示例
void event_loop() {
    while (running) {
        // 等待网络事件
        IUINT32 now = get_current_time();
        IUINT32 next_update = ikcp_check(kcp, now);
        IUINT32 wait_ms = (next_update > now) ? (next_update - now) : 0;
        
        // 等待网络事件或下次更新时间
        epoll_wait(epfd, events, MAX_EVENTS, wait_ms);
        
        // 处理网络 I/O...
        
        // 到达更新时间
        now = get_current_time();
        if (now >= ikcp_check(kcp, now)) {
            ikcp_update(kcp, now);
        }
    }
}
```

**内部逻辑**:
```
ikcp_check(kcp, current)
    │
    ├─ if (updated == 0) return current  // 立即更新
    │
    ├─ 时间跳变检测
    │
    ├─ if (current >= ts_flush) return current  // 立即更新
    │
    ├─ tm_flush = ts_flush - current  // 距下次 flush 的时间
    │
    ├─ 遍历 snd_buf 找最小 RTO:
    │   for (seg in snd_buf) {
    │       diff = seg->resendts - current
    │       if (diff <= 0) return current  // 有超时
    │       tm_packet = min(tm_packet, diff)
    │   }
    │
    └─ minimal = min(tm_flush, tm_packet)
       if (minimal < interval) minimal = interval
       return current + minimal
```

---

## 6. 下层数据输入

### 6.1 ikcp_input

**功能**: 当下层协议 (如 UDP) 收到数据时，将此数据输入到 KCP

```c
int ikcp_input(ikcpcb *kcp, const char *data, long size);
```

**参数**:
| 参数 | 类型 | 说明 |
|------|------|------|
| kcp | ikcpcb* | KCP 对象指针 |
| data | const char* | 收到的原始数据 |
| size | long | 数据长度 (字节) |

**返回值**:
| 值 | 说明 |
|----|------|
| 0 | 成功处理 |
| -1 | 数据为空或长度不足 (小于 24 字节) |
| -2 | 数据长度不足 |
| -3 | 未知命令 |

**内部流程详解**:

```
ikcp_input(data, size)
    │
    ├─ 检查 conv 是否匹配 → 不匹配返回 -1
    │
    └─ 循环解析多个段:
        ├─ 解码头部 (24字节):
        │   conv, cmd, frg, wnd, ts, sn, una, len
        │
        ├─ rmt_wnd = wnd  // 更新远端窗口
        │
        ├─ ikcp_parse_una(una)  // 处理 UNA，释放已确认段
        ├─ ikcp_shrink_buf()    // 收缩发送缓冲区
        │
        ├─ 根据 cmd 分类处理:
        │   │
        │   ├─ IKCP_CMD_ACK:
        │   │   ├─ 计算 RTT = current - ts
        │   │   ├─ ikcp_update_ack(RTT)  // 更新 RTT 估算
        │   │   ├─ ikcp_parse_ack(sn)    // 处理 ACK 确认
        │   │   └─ 记录最大 ACK (用于快速重传)
        │   │
        │   ├─ IKCP_CMD_PUSH (数据段):
        │   │   ├─ ikcp_ack_push(sn, ts)  // 确认收到数据
        │   │   ├─ 如果 sn >= rcv_nxt:
        │   │   │   ├─ 创建新段 IKCPSEG
        │   │   │   ├─ 复制数据
        │   │   │   └─ ikcp_parse_data(seg)  // 插入 rcv_buf
        │   │   └─ 移动可用数据 rcv_buf → rcv_queue
        │   │
        │   ├─ IKCP_CMD_WASK (窗口探测请求):
        │   │   └─ probe |= IKCP_ASK_TELL  // 准备回复窗口信息
        │   │
        │   └─ IKCP_CMD_WINS (窗口通知):
        │       └─ 更新 rmt_wnd (已在上面处理)
        │
        └─ 数据指针前进: data += len

    ├─ ikcp_parse_fastack(maxack, latest_ts)  // 快速重传检测
    │
    └─ 拥塞控制:
        if (snd_una > prev_una) {  // 有数据被确认
            if (cwnd < rmt_wnd) {
                if (cwnd < ssthresh) {
                    // 慢启动: cwnd++
                    cwnd++;
                    incr += mss;
                } else {
                    // 拥塞避免: cwnd += mss*mss/incr + mss/16
                    incr += (mss*mss)/incr + mss/16;
                }
            }
            if (cwnd > rmt_wnd) {
                cwnd = rmt_wnd;
                incr = rmt_wnd * mss;
            }
        }
```

**命令类型说明**:

| 命令 | 值 | 说明 |
|------|----|------|
| IKCP_CMD_PUSH | 81 | 推送数据 |
| IKCP_CMD_ACK | 82 | 确认 |
| IKCP_CMD_WASK | 83 | 窗口探测请求 |
| IKCP_CMD_WINS | 84 | 窗口通知 |

**协议包头格式** (24字节):
```
Offset  Size  Field
0       4     conv    (会话ID)
4       1     cmd     (命令类型)
5       1     frg     (分片数)
6       2     wnd     (窗口大小)
8       4     ts      (时间戳)
12      4     sn      (序列号)
16      4     una     (未确认序列号)
20      4     len     (数据长度)
24      -     data    (可选数据)
```

---

## 7. 数据刷新

### 7.1 ikcp_flush

**功能**: 将所有待发送的数据和 ACK 编码并输出

```c
void ikcp_flush(ikcpcb *kcp);
```

**说明**: 此函数通常由 `ikcp_update` 内部调用，用户一般不需要直接调用。

**核心职责**:

1. **发送 ACK**: 将所有待发的 ACK 打包发送
2. **窗口探测**: 如果远端窗口为 0，发送探测请求
3. **窗口通知**: 回复远端的窗口信息
4. **数据发送**: 将 snd_queue 中的数据移到 snd_buf 并按需发送
5. **超时重传**: 重传超时的数据段
6. **快速重传**: 重传被快速重传标记的数据段
7. **拥塞控制更新**: 根据 ACK 情况调整 cwnd

**刷新流程伪代码**:

```
ikcp_flush()
    │
    ├─ if (updated == 0) return  // 未初始化则返回
    │
    ├─ [阶段1: 发送 ACK]
    │   for (i = 0; i < ackcount; i++) {
    │       获取 ack[i].sn, ack[i].ts
    │       如果 + 包头 > MTU，先输出
    │       编码 ACK 段
    │   }
    │   ackcount = 0
    │
    ├─ [阶段2: 窗口探测]
    │   if (rmt_wnd == 0) {
    │       如果到达探测时间，发送 WASK
    │   }
    │
    ├─ [阶段3: 窗口通知]
    │   if (probe & ASK_TELL) 发送 WINS
    │
    ├─ [阶段4: 移动数据到发送缓冲]
    │   while (snd_nxt < snd_una + min(snd_wnd, rmt_wnd, cwnd)) {
    │       从 snd_queue 移一个段到 snd_buf
    │       设置初始参数
    │   }
    │
    ├─ [阶段5: 超时/快速重传]
    │   for (seg in snd_buf) {
    │       if (xmit == 0) {
    │           // 首次发送
    │           needsend = 1
    │       } else if (current >= resendts) {
    │           // 超时重传
    │           needsend = 1
    │           rto *= (nodelay ? 1.5 : 2)
    │       } else if (fastack >= resend) {
    │           // 快速重传
    │           needsend = 1
    │       }
    │       
    │       if (needsend) {
    │           编码并输出段
    │           xmit++
    │           if (xmit >= dead_link) state = -1
    │       }
    │   }
    │
    ├─ [阶段6: 输出剩余数据]
    │
    └─ [阶段7: 更新拥塞窗口]
        if (change) {
            // 快速重传后的恢复
            ssthresh = inflight / 2
            cwnd = ssthresh + resend
        }
        if (lost) {
            // 超时后的恢复
            ssthresh = cwnd / 2
            cwnd = 1  // 慢启动
        }
```

---

## 8. 配置接口

### 8.1 ikcp_nodelay

**功能**: 配置 KCP 工作模式（最重要的配置函数）

```c
int ikcp_nodelay(ikcpcb *kcp, int nodelay, int interval, int resend, int nc);
```

**参数**:
| 参数 | 类型 | 说明 |
|------|------|------|
| kcp | ikcpcb* | KCP 对象指针 |
| nodelay | int | 模式: 0=关闭, 1=普通加速, 2=快速加速 |
| interval | int | 内部时钟间隔 (毫秒) |
| resend | int | 快速重传次数: 0=关闭, >0=启用 |
| nc | int | 关闭流控: 0=启用, 1=关闭 |

**推荐配置**:

```cpp
// 默认模式 (类似 TCP)
ikcp_nodelay(kcp, 0, 40, 0, 0);

// 极速模式
ikcp_nodelay(kcp, 1, 10, 2, 1);
```

**各参数详解**:

**nodelay**:
- **0 (默认)**: 标准 ARQ 模式，RTO 翻倍
- **1**: 普通加速，RTO ×1.5，最小 RTO 30ms
- **2**: 快速加速，RTO ×1.5，最小 RTO 30ms，其他优化

**interval**:
- 内部处理时钟的刷新间隔
- 范围: 10ms - 5000ms，默认 100ms
- 越小延迟越低，CPU 占用越高
- 推荐: 10ms-20ms

**resend**:
- **0**: 关闭快速重传
- **1-5**: ACK 跨越此次数即触发重传
- 推荐: 2 (平衡重传及时性和冗余度)

**nc (no congestion control)**:
- **0**: 启用拥塞控制
- **1**: 关闭拥塞控制，仅用窗口大小控制发送频率

### 8.2 ikcp_wndsize

**功能**: 设置发送窗口和接收窗口大小

```c
int ikcp_wndsize(ikcpcb *kcp, int sndwnd, int rcvwnd);
```

**参数**:
| 参数 | 类型 | 说明 |
|------|------|------|
| kcp | ikcpcb* | KCP 对象指针 |
| sndwnd | int | 发送窗口大小 (包数) |
| rcvwnd | int | 接收窗口大小 (包数) |

**默认值**: sndwnd=32, rcvwnd=128

**说明**:
- 单位是"包"，不是字节
- rcvwnd 必须 >= 最大分片数
- 对应 TCP 的 SND_BUF 和 RCV_BUF，但单位不同

**带宽延迟积计算**:
```
所需窗口 = (带宽 × RTT) / (mss × 8)

例如: 10Mbps 带宽, 100ms RTT, mss=1376
窗口 = (10×10^6 × 0.1) / (1376 × 8) ≈ 91 包
```

### 8.3 ikcp_setmtu

**功能**: 设置最大传输单元

```c
int ikcp_setmtu(ikcpcb *kcp, int mtu);
```

**参数**:
| 参数 | 类型 | 说明 |
|------|------|------|
| kcp | ikcpcb* | KCP 对象指针 |
| mtu | int | MTU 大小 (字节) |

**返回值**:
| 值 | 说明 |
|----|------|
| 0 | 成功 |
| -1 | MTU 过小 (< 50 或小于 24) |
| -2 | 内存分配失败 |

**默认值**: 1400 字节

**说明**:
- mss = mtu - 24 (协议头部开销)
- MTU 影响数据分片和输出缓冲大小
- 修改后会重新分配缓冲区

### 8.4 ikcp_interval (辅助函数)

**功能**: 单独修改内部时钟间隔

```c
int ikcp_interval(ikcpcb *kcp, int interval);
```

**参数**:
| 参数 | 类型 | 说明 |
|------|------|------|
| kcp | ikcpcb* | KCP 对象指针 |
| interval | int | 新间隔 (毫秒) |

**限制**: 10 <= interval <= 5000

---

## 9. 信息查询

### 9.1 ikcp_waitsnd

**功能**: 查询等待发送的包数量

```c
int ikcp_waitsnd(const ikcpcb *kcp);
```

**返回值**: 等待发送的包数 = nsnd_buf + nsnd_que

**用途**:
- 流量控制：发送方可以根据这个值调整发送速率
- 连接状态监控
- 背压控制

### 9.2 ikcp_getconv

**功能**: 从原始数据中提取会话编号

```c
IUINT32 ikcp_getconv(const void *ptr);
```

**参数**:
| 参数 | 类型 | 说明 |
|------|------|------|
| ptr | const void* | 指向 KCP 数据包起始位置的指针 |

**返回值**: 32 位会话编号

**用途**: 用于在接收端根据 conv 找到对应的 kcp 对象

```cpp
// 接收 UDP 数据时
void on_udp_receive(char *buf, int len) {
    IUINT32 conv = ikcp_getconv(buf);
    ikcpcb *kcp = find_kcp_by_conv(conv);  // 查找对应对象
    if (kcp) {
        ikcp_input(kcp, buf, len);
    }
}
```

---

## 10. 高级接口

### 10.1 ikcp_log

**功能**: 写入 KCP 日志

```c
void ikcp_log(ikcpcb *kcp, int mask, const char *fmt, ...);
```

**参数**:
| 参数 | 类型 | 说明 |
|------|------|------|
| kcp | ikcpcb* | KCP 对象指针 |
| mask | int | 日志掩码，需要与 kcp->logmask 匹配 |
| fmt | const char* | 格式化字符串 |

**日志掩码**:

| 掩码值 | 常量 | 说明 |
|--------|------|------|
| 1 | IKCP_LOG_OUTPUT | 输出日志 |
| 2 | IKCP_LOG_INPUT | 输入日志 |
| 4 | IKCP_LOG_SEND | 发送日志 |
| 8 | IKCP_LOG_RECV | 接收日志 |
| 16 | IKCP_LOG_IN_DATA | 输入数据日志 |
| 32 | IKCP_LOG_IN_ACK | 输入 ACK 日志 |
| 64 | IKCP_LOG_IN_PROBE | 输入探测日志 |
| 128 | IKCP_LOG_IN_WINS | 输入窗口日志 |
| 256 | IKCP_LOG_OUT_DATA | 输出数据日志 |
| 512 | IKCP_LOG_OUT_ACK | 输出 ACK 日志 |
| 1024 | IKCP_LOG_OUT_PROBE | 输出探测日志 |
| 2048 | IKCP_LOG_OUT_WINS | 输出窗口日志 |

**启用日志**:
```cpp
kcp->writelog = my_log_func;  // 设置日志回调
kcp->logmask = IKCP_LOG_OUTPUT | IKCP_LOG_SEND | IKCP_LOG_RECV;
```

### 10.2 ikcp_allocator

**功能**: 替换 KCP 的内存分配器

```c
void ikcp_allocator(void* (*new_malloc)(size_t), void (*new_free)(void*));
```

**参数**:
| 参数 | 类型 | 说明 |
|------|------|------|
| new_malloc | function pointer | 自定义 malloc 实现 |
| new_free | function pointer | 自定义 free 实现 |

**注意**: 
- 全局生效，影响后续所有 kcp 对象的内存分配
- 应在创建任何 kcp 对象之前调用

---

## API 速查表

| 函数 | 分类 | 返回值 | 说明 |
|------|------|--------|------|
| ikcp_create | 创建 | ikcpcb*/NULL | 创建 KCP 对象 |
| ikcp_release | 销毁 | void | 释放 KCP 对象 |
| ikcp_setoutput | 回调 | void | 设置输出回调 |
| ikcp_send | 发送 | int (字节数/-1/-2) | 发送数据 |
| ikcp_recv | 接收 | int (字节数/-1/-2/-3) | 接收数据 |
| ikcp_update | 更新 | void | 更新状态 |
| ikcp_check | 更新 | IUINT32 | 计算下次更新时间 |
| ikcp_input | 输入 | int (0/-1/-2/-3) | 输入下层数据 |
| ikcp_flush | 刷新 | void | 刷新输出 |
| ikcp_peeksize | 查询 | int (长度/-1) | 查看消息长度 |
| ikcp_setmtu | 配置 | int (0/-1/-2) | 设置 MTU |
| ikcp_wndsize | 配置 | int | 设置窗口 |
| ikcp_waitsnd | 查询 | int | 等待发送数 |
| ikcp_nodelay | 配置 | int | 设置模式 |
| ikcp_interval | 配置 | int | 设置间隔 |
| ikcp_log | 调试 | void | 写入日志 |
| ikcp_allocator | 高级 | void | 替换分配器 |
| ikcp_getconv | 辅助 | IUINT32 | 提取会话号 |
