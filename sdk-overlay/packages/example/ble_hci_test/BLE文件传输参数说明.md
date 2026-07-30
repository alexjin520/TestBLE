# TestBLE BLE 协议说明

> 对应版本：App `20260728-v79-live-tail-retry`，板端
> `20260728-live-tail-uv-epoll-safe`

## 1. GATT

| 项目 | 值 |
| --- | --- |
| 广播名 | `TestBLE` |
| Service UUID | `78563412-3412-7856-1234-567812345678` |
| Characteristic UUID | `79563412-3412-7856-1234-567812345678` |
| 手机到板端 | Write / Write Without Response |
| 板端到手机 | Notify |
| 多字节整数 | Little Endian |

产品模式由 `my-fnirs` 进程内嵌 GATT 服务，不需要另外启动
`my-server`。独立 `my-server -U` 仅用于旧镜像或协议调试。

## 2. 文件上传：FILE RX

手机或 PC 向板端 `/app_data/ble_rx/` 上传文件。

| 操作码 | 格式 |
| --- | --- |
| START `0x01` | `[op][name_len][name][size:u32][crc32:u32]` |
| DATA `0x02` | `[op][seq:u32][len:u16][payload][crc16:u16]` |
| END `0x03` | `[op]` |
| ABORT `0x11` | `[op]` |

板端通过状态 Notify 回报已接收的包数、字节数和错误状态。文件名只能使用
basename，不能包含路径分隔符。

## 3. 已封口文件下载：FILE TX

| 操作码 | 方向 | 格式 |
| --- | --- | --- |
| REQ `0x21` | 手机写 | `[op][name_len][name]` |
| START `0x22` | 板端 Notify | `[op][name_len][name][size:u32][crc32:u32]` |
| DATA `0x23` | 板端 Notify | `[op][seq:u32][len:u16][payload][crc16:u16]` |
| END `0x24` | 板端 Notify | `[op]` |
| ABORT `0x25` | 双向 | `[op]` |
| ACK `0x26` | 手机写 | `[op][next_seq:u32]` |
| LAST_REQ `0x27` | 手机写 | `[op]` |
| LAST_RSP `0x28` | 板端 Notify | `[op][name_len][name]` |

当前板端参数：

| 参数 | 值 |
| --- | --- |
| 累计 ACK 窗口 | 14 包 |
| 普通 MTU 包间隔 | 6 ms |
| MTU 23 包间隔 | 10 ms |
| ACK 后延迟 | 3 ms |
| 最大 payload | 235 B，并受 ATT MTU 限制 |

手机以 `next_seq` 发送累计 ACK。板端最多发送一个窗口，然后等待手机继续
确认，避免 Notify 队列和 GATT Write 互相阻塞。

## 4. 采集中文件同步：LIVE FILE

LIVE FILE 在采集时同步持续增长的 `*_Hangzhou.bin`。停止采集只会停止生成
新样本；已经开始的下载会继续补齐尾部，直到最终大小和 CRC32 一致。

| 操作码 | 方向 | 格式 |
| --- | --- | --- |
| REQ `0x29` | 手机写 | `[op]` |
| START `0x2A` | 板端 Notify | `[op][name_len][name]` |
| DATA `0x2B` | 板端 Notify | `[op][seq:u32][offset:u32][len:u16][payload][crc16:u16]` |
| FINAL `0x2C` | 板端 Notify | `[op][phase][final_size:u32][crc32:u32]` |
| ACK `0x2D` | 手机写 | `[op][next_seq:u32][next_offset:u32]` |
| ABORT `0x2E` | 双向 | `[op][reason?]` |

`FINAL phase=0` 表示板端文件已经 `fsync` 并封口，大小和 CRC32 从此确定。
`FINAL phase=1` 表示板端已经发送全部字节；手机校验成功后必须再发送一次最终
ACK，板端才释放传输状态。

当前板端参数：

| 参数 | 值 |
| --- | --- |
| DATA 调度间隔 | 8 ms |
| 无新数据时轮询 | 40 ms |
| ACK/窗口重试 | 240 ms |
| 累计 ACK 窗口 | 14 包 |

逻辑分析仪可能看到接近 100 ms 的突发周期，因为一个窗口约为
`14 × 8 ms = 112 ms`，再叠加 BLE 连接事件和手机 ACK。这不表示应用层严格
每 100 ms 只产生一个 DATA 分片。

## 5. 实时预览：STREAM

| 操作码 | 方向 | 格式 |
| --- | --- | --- |
| START `0x31` | 手机写 | `[op][rate_hz:u32][period_ms:u16]` |
| DATA `0x32` | 板端 Notify | `[op][seq:u32][sample_base:u32][count:u16][int16 × count]` |
| STOP `0x33` | 板端 Notify | `[op][total_samples:u32]` |
| ABORT `0x34` | 手机写 | `[op]` |
| TIME SET `0x35` | 手机写 | 时间同步数据 |

STREAM 用于低延迟预览，LIVE FILE/FILE TX 用于可靠保存。最终数据完整性应以
下载文件的大小和 CRC32 为准。

## 6. fNIRS 控制

| 操作码 | 请求内容 |
| --- | --- |
| SCAN `0x40` | `[op]` |
| SAMPLE ON `0x41` | `[op]` |
| SAMPLE OFF `0x42` | `[op]` |
| GAIN `0x43` | `[op][gain]` |
| LED ARRAY `0x44` | `[op][node][led][735mA][850mA]...` |
| STREAM CHANNEL `0x45` | `[op][channel config...]` |
| LATEST PATH `0x46` | `[op]` |
| RECORD STAT `0x47` | `[op]` |
| RESPONSE `0x48` | `[op][echo_op][status][payload...]` |

LED ARRAY 中 node 从 1 开始计数，每项固定 4 字节。例如：

```text
03 02 28 14
```

表示 node 3、LED 2、735 nm 电流 40 mA、850 nm 电流 20 mA。

## 7. 完整性和排错

- 每个文件 DATA 分片使用 CRC16。
- START 给出文件大小和 CRC32，接收端完成后再次校验。
- LIVE FILE ACK 的 seq 和 offset 必须同时单调递增。
- `SAMPLE OFF` 后等待 `FINAL phase=0`，不能把应答本身当作文件已封口。
- MTU 仅为 23 时吞吐量明显下降，但协议仍应正确工作。
- 同一时刻不要运行 `my-fnirs` 和 `my-fnirs-uv`，二者会争用 CAN、UART 和
  Bluetooth。

关键实现：

- 板端：`mybtgatt-server.c`
- App 协议：`mobile_app/lib/protocol.dart`
- App LIVE FILE：`mobile_app/lib/ble_live_sync.dart`
- App 普通下载：`mobile_app/lib/ble_board_rx.dart`
- fNIRS 控制桥：`../x2600-fNIRS/fnirs/src/fnirs_ble_ipc.c`
