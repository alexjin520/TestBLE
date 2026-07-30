# fNIRS 项目 Libevent 重构分析报告

**代码仓库：** GitHub `alexjin520/TestBLE`，公开分支 `main`  
**分析对象：** 旧版 `my-fnirs`（pthread 轮询，`fnirs/src/main.c`）→ 新版统一产物 `my-fnirs`（libevent + 内嵌 GATT）  
**产品构建：** `FNIRS_EV_IO=1` + `FNIRS_EMBEDDED=1` + `MY_SERVER_EMBEDDED=1`（x2600halley7）

---


| 项目                     | 说明                                                                                                                      |
| ---------------------- | ----------------------------------------------------------------------------------------------------------------------- |
| **做了什么**               | 将板端守护进程从「多线程 + `sleep(2)` 轮询」重构为 **libevent 事件驱动**：CAN/UART/定时器/数据落盘等由 `event_base` 统一调度；BLE GATT 仍保留独立线程（BlueZ 协议栈限制）。 |
| **业务影响**               | **协议与 App 接口未改**（CAN 帧、BLE 命令字、OTA 格式不变）；手机 App 与 PC Hub 无需配合修改。                                                        |
| **板测结论（x2600halley7）** | ✅ BLE 扫描（节点数正确）、配置命令、STREAM 开流、录制状态查询；✅ PC Hub 12 节点 SPDATA；✅ UART `EV_READ` 收包。                                        |
| **相对需求的完成度**           | **核心重构已完成**；非「全进程单一事件循环」（GATT 独立线程为业界常见折中）。第三方静态评审结论为 *Partially meets*，主因是仓库快照无法自证全量构建 + GATT/BLE 边界仍有同步等待，**非功能不可用**。 |
| **主要收益**               | 业务线程从约 **6～7 个降至约 1 个**（另加 GATT 线程）；去掉顶层 `sleep(2)`；模块 init/shutdown 可回滚；I/O 延迟与 CPU 空转改善。                              |
| **待办（非阻塞上线）**          | OTA / 工厂产测需专用工具端到端签字；30min 长稳建议补测；`evthread_`* 初始化失败建议改为 fail-fast（小改动）。                                                |
| **部署**                 | `device/x2600halley7/deploy-my-fnirs-ev.ps1 -Build` → 推送 `/opt/golgi/my-fnirs` 并重启 `S99myapp`。                          |


---

**工作区状态（2026-07-21）：**

- `Build.mk` 仅保留一个产品目标：`LOCAL_MODULE := my-fnirs`（ev 栈源码 + 上述三个宏）；旧 pthread `main.c` 目标已从产品构建移除。
- `S99myapp` / 部署脚本启动与推送 `/opt/golgi/my-fnirs`。
- 业务源码对齐 `5956a59`；**OTA 仍为 Phase E 事件状态机**（`fnode_ev_ota_`* + `ev_can` 10ms tick），**未**回退阻塞 `fnode_task_OTA_handler`。
- **2026-07-21 上午**：EV 构建下 **移除** `fnode_task` **/** `fhub_task` **线程**；IDLE/reset/扫描/采样入口迁入 `fnode_ev_idle_service()` + `fnode_ev_kick_idle()`，由 `ev_can` 100ms 定时器与 API 即时 kick 驱动；板测 BLE 扫描/采样/OTA/工厂测试通过。
- **2026-07-21 傍晚**：**UART 真** `EV_READ`：`/dev/ttyS1` 注册 `EV_READ` + `fusb_ev_drain_read()`；20ms timer 仅跑 `fusb_ev_poll()`（心跳/SPDATA/电压/工厂）；板测 `fusb SPDATA tx nodes=12 size=14016`。

---



## 一、执行摘要

本次重构将 fNIRS Hub 守护进程从「多 pthread + 顶层 `sleep(2)` 保活」迁移为「单一 `event_base` + 模块化 `ev_`* 驱动 I/O 与时序」。**业务协议层（CAN 帧格式、FUSB 命令字、BLE GATT 特征、OTA 包协议）基本未改**；变化集中在**调度模型**与**线程模型**。产物命名上，原实验二进制 `my-fnirs-ev` 已**并入**产品名 `my-fnirs`（实现仍是 libevent 栈）。

### 迁移完成度（当前产品构建）


| 领域                         | 状态     | 驱动方式                                                          |
| -------------------------- | ------ | ------------------------------------------------------------- |
| 进程生命周期 / 信号                | ✅ 完成   | `ev_app` + `ev_signal`                                        |
| CAN 收帧 + Hub 事件            | ✅ 完成   | `ev_can`：`EV_READ` + 20ms hub poll                            |
| IDLE 节点扫描                  | ✅ 完成   | `ev_can` 100ms → `fnode_ev_idle_service()`                    |
| 采样时序                       | ✅ 完成   | `fnode_ev_sample_*` + `ev_can` 1ms tick                       |
| UART / FUSB                | ✅ 完成   | `ev_uart`：`EV_READ` 收包 + 20ms housekeeping → `fusb_ev_poll()` |
| 数据落盘                       | ✅ 完成   | `ev_timer` 50ms → `fdatalog_ev_poll()`                        |
| LED                        | ✅ 完成   | 去掉空转 `led_task`                                               |
| 内嵌 GATT                    | ✅ 完成   | `ev_gatt` 独立线程（BlueZ 限制）                                      |
| **BLE 命令跨线程分发**            | ✅ 完成   | `ev_ble_async`：GATT → 主 `event_base` 任务队列 + socketpair 同步等待   |
| **BLE SCAN（App 扫描）**       | ✅ 完成   | 主线程 `fnirs_ble_handle_scan_ev_blocking()` + `ev_can_pump`     |
| **BLE STREAM_START**       | 🟡 半异步 | 主线程 `evtimer` 状态机（`fnirs_ble_start_stream_async`）             |
| 工厂产测 CAN 序列                | ✅ 完成   | `fnode_ev_factory_*` + 1ms tick                               |
| **OTA**                    | ✅ 完成   | `fnode_ev_ota_`* + `ev_can` 10ms tick（Phase E）                |
| `fnode_task` / `fhub_task` | ✅ 已移除  | EV 构建不创建线程；逻辑在 `fnode_ev_*` + `ev_can`                        |
| `ev_ble_ipc` socket 服务     | ⬜ 未注册  | embedded 构建不需要                                                |
| 产物命名                       | ✅ 已并入  | 产品二进制统一为 `my-fnirs`（不再单独安装 `my-fnirs-ev`）                     |


**净效果：** 日常路径（BLE 扫描/采样/STREAM、PC Hub 12 节点 SPDATA、UART 心跳）已事件化并板测通过；**2026-07-21** 进一步去掉 GATT 内 `usleep` 与残留 `fnode_task`；架构为 **libevent 主循环 + GATT 线程 +（可选）阻塞式 BLE job 回调内嵌套** `EVLOOP_NONBLOCK`，而非纯单线程无阻塞模型。

---



## 二、分析范围与证据来源

- **源码对比：** `fnirs/src/main.c` vs `ev/src/main_ev.c`；`#ifdef FNIRS_EV_IO` 双路径（`fnode.c`、`fusb.c`、`led.c`、`fdatalog.c` 等）
- **libevent 用法：** 以 `timerfd` + `EV_READ | EV_PERSIST` 为主，辅以 CAN fd 读事件与 `evsignal_new()`
- **运行时证据：** x2600halley7 板测日志（`/tmp/fnirs.log`、`/tmp/ev_tick.count`）

---



## 三、重构前后顶层架构



### 3.1 旧版 `my-fnirs`

```c
// main.c
signal(SIGTERM/SIGINT, signal_handler);  // 信号内直接 teardown + exit(0)
init: led, ble_service, fusb, fdatalog, fNIRS, stream, ble_ipc
while (1) { sleep(2); ble_service_tick(); led_system(1); }
```

长期存活线程：`fhub_task`、`fnode_task`（SCHED_FIFO）、`fusb_task`、`fdatalog_task`、`led_task`、`fnirs_ble_ipc_thread`；GATT 由独立 `my-server` 子进程提供。

### 3.2 新版统一 `my-fnirs`（原 `my-fnirs-ev`）

```c
// main_ev.c
app = ev_app_create();                    // evthread_use_pthreads + notifiable base
ev_app_register_modules(app, modules);    // signal → fnirs → can → uart → ble_async → gatt → timer
ev_app_init_modules(app);
ev_app_run(app);                          // event_base_dispatch()
ev_app_shutdown_modules(app);             // 逆序关闭
```

**模块注册顺序（2026-07-21）：** `signal → fnirs → can → uart → **ble_async** → gatt → timer`（`ble_async` 须在 `gatt` 之前初始化）。

**共存线程：** libevent 主 dispatch 线程 + `ev_gatt` BlueZ 线程（**无** `fnode_task` / `fhub_task`）。

**命名说明：** 历史文档与板测日志中的 `my-fnirs-ev` 与当前 `/opt/golgi/my-fnirs` 为同一套 libevent 实现；`Build.mk` 已不再产出单独的 `my-fnirs-ev` 目标。

### 3.3 架构示意

```
my-fnirs（libevent + 内嵌 GATT，2026-07-21）
┌──────────────────────────────────────────────────────────────┐
│ event_base_dispatch()  [主线程]                               │
│  ev_signal   SIGINT/SIGTERM → ev_app_stop()                  │
│  ev_fnirs    业务 init/shutdown                             │
│  ev_can      CAN EV_READ + hub 20ms + idle 100ms             │
│              + sample 1ms + factory 1ms + ota 10ms           │
│              ev_can_hub_service() = drain CAN + hub 事件     │
│  ev_uart     UART EV_READ → fusb_ev_drain_read()             │
│              20ms housekeeping → fusb_ev_poll()            │
│  ev_ble_async  eventfd + event_base_once → BLE job 队列      │
│              SCAN：阻塞 job 内 spin + ev_can_pump            │
│              STREAM：evtimer 异步状态机                       │
│  ev_gatt     BlueZ mainloop [独立线程]                        │
│  ev_timer    1s housekeeping + fdatalog 50ms                 │
└──────────────────────────────────────────────────────────────┘
         ▲ GATT 写特征 → fnirs_ble_dispatch_request()
         │              → ev_ble_async_dispatch() [GATT 线程 poll 等响应]
         └────────────── job 在主线程 fnirs_ble_ev_process_job() 执行
```

---



## 四、组件逐项对比

> **表格列说明**
>
> - **组件**：子系统名称  
> - **旧实现**：旧版 pthread `my-fnirs`（`fnirs/src/main.c`）中的线程/循环/信号处理方式  
> - **新实现**：统一产物 `my-fnirs`（原 `my-fnirs-ev`）中的 libevent 模块与回调  
> - **被替换的阻塞行为**：该迁移主要消除的 sleep/阻塞/不安全路径



### 4.1 总览表


| 组件               | 旧实现                                                                                                    | 新实现（统一 `my-fnirs`）                                                                             | 被替换的阻塞行为                                            |
| ---------------- | ------------------------------------------------------------------------------------------------------ | ---------------------------------------------------------------------------------------------- | --------------------------------------------------- |
| 进程启动与生命周期        | `main.c` 初始化后 `while(1) sleep(2)`                                                                      | `main_ev.c` + `ev_app.c`：`event_base_dispatch()` + 模块化 shutdown                                | 顶层 sleep 保活；无结构化 teardown                           |
| 信号处理             | `main.c`：`signal()` → `signal_handler()` 内直接调 `ble_service_stop`、`fNIRS_exit`、`fusb_exit`… 后 `exit(0)` | `ev_signal.c`：`evsignal_new(SIGINT/SIGTERM)` → 日志 + `ev_app_stop()` → 正常模块逆序 shutdown          | 在原始信号上下文中执行复杂清理（非 async-signal-safe）                |
| 致命信号诊断           | 无                                                                                                      | `main_ev.c`：`SIGSEGV/SIGABRT/SIGBUS` → `ev_timer_note_unsafe()` 后 re-raise                     | —                                                   |
| CAN 收帧           | `fhub_task`：`wcanfd_read(10ms timeout)` + `usleep(50)`                                                 | `ev_can.c`：CAN fd `O_NONBLOCK` + `EV_READ` → `ev_can_drain()` → `fnode_ev_hub_process_frame()` | 专用 hub 线程上的阻塞读 + sleep 轮询                           |
| Hub 内部事件（msgq）   | `fhub_task` 每轮调用 `fnode_hub_process_events()`                                                          | `ev_can.c`：20ms `timerfd` → `ev_can_pump()` → `ev_can_drain()` + `fnode_ev_hub_service()`      | `fhub_task` 线程（EV 构建不创建）                            |
| IDLE 节点扫描        | `fnode_task_IDLE_handler`：等 `node->timer` 到期后发 CAN SCAN                                                | `ev_can.c`：100ms `timerfd` → `fnode_ev_idle_scan()`；`fnode_task` 只轮询状态切换                       | IDLE 态在 `fnode_task` 内等定时器                          |
| 采样时序（LED/ADC/轮询） | `fnode_task_SAMPLE_handler`：内层 `usleep` 大循环（`fnode_10HZ_sample_handler_poll`）                          | `fnode_ev_sample_begin/tick/finish` + `ev_can` 1ms tick；`fnode_task` SAMPLE 态仅 `usleep(100ms)` | 采样线程内多层 `usleep` 阻塞                                 |
| OTA 升级           | `fnode_task` → `fnode_task_OTA_handler()`：`while` + `usleep(1000)` 等 ACK                               | `fnode_ev_ota_*` + `ev_can` 10ms tick；`fnode_task` OTA 态仅 `usleep(100ms)`                      | OTA 线程内阻塞 ACK 循环                                    |
| 工厂产测 CAN 序列      | `fnode_task_FACTORY_TEST_handler`：阻塞 `while` + `usleep`                                                | `fnode_ev_factory_*` + `ev_can` 1ms factory tick；`fnode_task` 仅 `usleep(100ms)`                | 产测线程内长阻塞循环                                          |
| UART / FUSB      | `fusb_task`：每 20ms `usleep` → 心跳/读命令/SPDATA/电压/工厂上报                                                    | `ev_uart.c`：20ms `timerfd` → `fusb_ev_poll()`；UART `O_NONBLOCK` + `fusb_uart_write_all()` 循环写  | 专用 `fusb_task` 线程；大包一次 `write` 失败即关 fd              |
| 数据落盘             | `fdatalog_task`：有数据写盘，空队列 `usleep(50)`（50μs）                                                           | `ev_timer.c`：50ms `timerfd` → `fdatalog_ev_poll()`                                             | `fdatalog_wr` 写盘线程                                  |
| LED 指示           | `led_task`：空循环 `usleep(50ms)`                                                                          | `led_init` 不建线程；`led_system()` 等由 1s tick / 业务直接调用                                             | 无意义的 `led_task` 空转                                  |
| BLE GATT 服务      | 独立 `my-server` 子进程 + `ble_service_tick()` spawn                                                        | `ev_gatt.c`：进程内 `my_server_embedded_main(-U)` 独立线程                                             | 子进程管理开销；与主进程分离                                      |
| BLE 文件服务         | `ble_service_init/start` + `main` 每 2s `ble_service_tick()`                                            | `FNIRS_EMBEDDED` 构建跳过 `ble_service_*`（GATT 已内嵌）                                                | `spawn_my_server()` 内 `sleep(1)` 阻塞事件循环（非 embedded） |
| BLE IPC（App 命令）  | `fnirs_ble_ipc_thread`：`accept` 循环处理 Unix socket                                                       | embedded：`fnirs_ble_dispatch_request()` 进程内同步；`ev_ble_ipc.c` 已实现但 **未注册**                      | 阻塞 `accept`（embedded 不需要 socket）                    |
| CAN/节点懒加载        | 启动即 `fNIRS_init()` → 拉起 `fhub_task` + `fnode_task`                                                     | embedded EV：`fNIRS_lazy_init()` 于首次 BLE/UART 命令；`ev_can_try_attach()` 重试                       | 启动即占 CAN、总线噪声导致早期 abort 的风险                         |
| Housekeeping     | `main` 每 2s：`ble_service_tick` + `led_system(1)`                                                       | `ev_timer.c` 1s tick：`led_system`、lazy CAN attach 重试、`/tmp/ev_tick.count`                      | 2s 粒度保活                                             |


---



### 4.2 分项详解（与 PDF 同级粒度）



#### 4.2.1 信号处理（Signal handling）


| 列            | 内容                                                                                                                                                                                                                                            |
| ------------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **旧实现**      | `main.c` 用 `signal(SIGTERM, signal_handler)` 和 `signal(SIGINT, signal_handler)` 注册处理函数。`signal_handler()` 依次调用 `ble_service_stop()`、`fnirs_stream_exit()`、`fnirs_ble_ipc_exit()`、`fNIRS_exit()`、`fusb_exit()`、`fdatalog_exit()`，然后 `exit(0)`。 |
| **新实现**      | `ev_signal.c` 为 `SIGINT`、`SIGTERM` 创建持久 signal event（`evsignal_new` + `event_add`）。回调 `ev_signal_cb()` 打日志并调用 `ev_app_stop(app)`，由 `event_base_dispatch()` 正常返回后执行 `ev_app_shutdown_modules()` 逆序关闭各模块。另 `signal(SIGPIPE, SIG_IGN)`。          |
| **被替换的阻塞行为** | 避免在异步信号上下文中执行子系统 teardown（非 signal-safe）。                                                                                                                                                                                                     |
| **源文件**      | 旧：`fnirs/src/main.c`；新：`ev/src/ev_signal.c`、`ev/src/ev_app.c`                                                                                                                                                                                 |




#### 4.2.2 进程生命周期（Process lifecycle）


| 列            | 内容                                                                                                                                                                                                            |
| ------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **旧实现**      | 线性 `main()`：一次性 init 全部子系统 → 无限 `sleep(2)` 循环 → 正常路径不可达的 exit 代码。无模块边界，失败时难以局部回滚。                                                                                                                             |
| **新实现**      | `ev_app_create()` 启用 `evthread_use_pthreads()` 与 `evthread_make_base_notifiable()`。`ev_app_register_modules()` 按序注册；`ev_app_init_modules()` 失败时截断并逆序 shutdown 已成功模块。`ev_app_run()` = `event_base_dispatch()`。 |
| **被替换的阻塞行为** | 顶层 `sleep(2)` 保活循环；ad hoc 清理顺序。                                                                                                                                                                               |
| **源文件**      | `ev/src/main_ev.c`、`ev/src/ev_app.c`                                                                                                                                                                          |




#### 4.2.3 CAN 收帧与 Hub 任务（CAN RX / fhub_task）


| 列            | 内容                                                                                                                                                                                                    |
| ------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **旧实现**      | `fhub_task`（`fnode.c`）：`fhub_open` → 循环内先 `fnode_hub_process_events()` 处理 hub msgq，再 `wcanfd_read(..., 10ms)`，无效帧或读失败则 `usleep(50)`。有效帧调用 `fnode_CANFD_frame_handler()`。                              |
| **新实现**      | `ev_can.c`：`ev_can_read_cb` 在 CAN fd 可读时触发 `ev_can_drain()` 非阻塞读尽所有帧 → `fnode_ev_hub_process_frame()`。Hub msgq 由 **20ms timerfd** 单独驱动 `fnode_ev_hub_service()`。`fhub_task` 在 `FNIRS_EV_IO` 下不编译/不创建。 |
| **被替换的阻塞行为** | 去掉整个 `fhub_task` 线程；消除读超时 + `usleep(50)` 组合轮询。                                                                                                                                                        |
| **设计注意**     | EV 路径**先 drain CAN 再处理 hub 事件**，保证 `hub_fnum` 与帧顺序一致（迁移早期回归点）。                                                                                                                                        |
| **源文件**      | `fnirs/src/fnode.c`（`fhub_task`）、`ev/src/ev_can.c`                                                                                                                                                    |




#### 4.2.4 IDLE 扫描（Node discovery scan）


| 列            | 内容                                                                                                                                                                                                                        |
| ------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **旧实现**      | `fnode_task_IDLE_handler`：在 `FHUB_STATE_RESET` 下等待 `wos_timer`（`FNODE_SCAN_RESCAN_MS`≈100ms）到期，发 CAN SCAN，处理在线统计；`ota_start_flag` 时 `scan_count++`，>2 次后切 `FHUB_STATE_OTA`。                                               |
| **新实现**      | **后台扫描**：`ev_can` 100ms → `fnode_ev_idle_service()`（reset + `fnode_task_IDLE_handler` + CAN SCAN）。**即时 kick**：`fnode_ev_kick_idle()`。**App 显式 SCAN**：主线程 `fnirs_ble_handle_scan_ev_blocking()`（100ms×50 + `ev_can_pump`）。 |
| **被替换的阻塞行为** | `fnode_task` 线程；GATT 内 `usleep` 扫描。                                                                                                                                                                                       |
| **源文件**      | `fnode.c`（`fnode_ev_idle_service`）、`fnirs_ble_ipc.c`、`ev_can.c`                                                                                                                                                           |




#### 4.2.5 采样状态机（Sampling）


| 列            | 内容                                                                                                                                                                                                     |
| ------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| **旧实现**      | `fnode_task` 进入 `FHUB_STATE_SAMPLE` 后调用 `fnode_task_SAMPLE_handler()`：CAN 复位 → `fnode_10HZ_sample_handler_poll()` 内层循环（LED on/off、`usleep` 等）→ 停采样 → 回 `FHUB_STATE_RESET`。                             |
| **新实现**      | IDLE 检测到 `node->sample` 后设 `FHUB_STATE_SAMPLE` 并 **直接** `fnode_ev_sample_begin()`（无 `sleep(1)`）。`ev_can` **1ms** sample tick 驱动 `FNODE_EV_SP_`* 阶段。结束 `fnode_ev_sample_finish()` → `FHUB_STATE_RESET`。 |
| **被替换的阻塞行为** | 采样路径上大量 `usleep` 与内层 poll 循环。                                                                                                                                                                          |
| **并发修复**     | `5956a59`：`fnode_ev_sample_tick()` 读/清 `node_ack_ready` 纳入 `event_mtx`，与写端一致。                                                                                                                          |
| **板测**       | `fnode_ev_sample_begin: event mode (12 alive)`、`can sample tick armed (1ms)`、`fusb SPDATA tx nodes=12 size=14016`。                                                                                     |
| **源文件**      | `fnode.c`（`fnode_ev_sample_`*）、`ev_can.c`                                                                                                                                                              |




#### 4.2.6 OTA 升级（OTA）— **Phase E 事件化（当前）**


| 列            | 内容                                                                                                                                                                                                                            |
| ------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **旧实现**      | UART 收 `0x2001`–`0x2005` 准备 bin → `fnode_ota_start()` 置 `ota_start_flag` → IDLE 扫描 >2 次后进 `FHUB_STATE_OTA` → `fnode_task_OTA_handler()` 阻塞执行 START/PACKAGE/FINISH，内部 `while` + `usleep(1000)` 等节点 ACK。                        |
| **新实现（当前）**  | IDLE 进 `FHUB_STATE_OTA` 后调用 `fnode_ev_ota_begin()` → `ev_can_ota_arm_deferred()`；`ev_can` **10ms** tick 驱动 `fnode_ev_ota_tick()`。`fnode_task` 在 OTA 态仅 `usleep(100ms)`。`fnode_task_OTA_handler()` 仍保留在源码中，但 **EV 构建不走该阻塞路径**。 |
| **被替换的阻塞行为** | OTA 期间 `fnode_task` 内长时间 `while` + `usleep(1000)` 等 ACK。                                                                                                                                                                      |
| **板测**       | ☐ 需 OTA 工具 + `.bin` 端到端回归（代码路径已事件化）。                                                                                                                                                                                          |
| **源文件**      | `fnode.c`（`fnode_ev_ota_`*）、*`ev_can.c`*（*`ev_can_ota_arm`）                                                                                                                                                                   |




#### 4.2.7 工厂产测（Factory test）


| 列            | 内容                                                                                                                                                                         |
| ------------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **旧实现**      | UART 命令 `0x1001`–`0x1008` → `fnode_factory_test_mode_enter()` → IDLE 进 `FHUB_STATE_FACTORY_TEST` → `fnode_task_FACTORY_TEST_handler()` 阻塞执行各 `FUSB_`* 模式（测距、交替波长、LED 校准等）。 |
| **新实现**      | IDLE 进 `FHUB_STATE_FACTORY_TEST` 后 `fnode_ev_factory_kick()`；`ev_can` **1ms** factory tick 驱动 `fnode_ev_factory_tick()` 分步状态机。`fnode_task` 仅 `usleep(100ms)`。              |
| **被替换的阻塞行为** | 工厂模式内 `while` + `usleep` 长循环。                                                                                                                                              |
| **板测**       | ☐ 需产测上位机端到端验证。                                                                                                                                                             |
| **源文件**      | `fnode.c`（`fnode_ev_factory_`*）、`ev_can.c`                                                                                                                                 |




#### 4.2.8 UART / FUSB（PC Hub 串口）


| 列            | 内容                                                                                                                                                                                          |
| ------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **旧实现**      | `fusb_task`：每 20ms 执行 `fusb_hbeat_handler`、`fusb_read_handler`、`fusb_spdata_handler`、`fusb_voltage_detect_handler`、`fusb_factory_cmd_data_handler`。阻塞 UART；大包 SPDATA 一次写失败会 `fusb_close()`。 |
| **新实现**      | `ev_uart.c`：`/dev/ttyS1` @ 2M baud、`O_NONBLOCK`；UART fd `EV_READ                                                                                                                            |
| **被替换的阻塞行为** | 去掉 `fusb_task`；去掉「20ms 定时器兼做 UART 读」的伪事件化；修复非阻塞 fd 下 SPDATA 大包写失败断连。                                                                                                                        |
| **板测**       | 启动日志 `uart open (EV_READ)`；采样时 `fusb SPDATA tx nodes=12 size=14016`（心跳默认不写日志）。                                                                                                              |
| **源文件**      | `fusb.c`、`ev_uart.c`                                                                                                                                                                        |




#### 4.2.9 数据落盘（fdatalog）


| 列            | 内容                                                                                                    |
| ------------ | ----------------------------------------------------------------------------------------------------- |
| **旧实现**      | `fdatalog_task`：从 ring buffer 取包写 eMMC；队列空时 `usleep(50)`（50**微秒**，注释与代码一致）。                           |
| **新实现**      | `fdatalog_init` 不创建线程；`ev_timer.c` 50ms timerfd → `fdatalog_ev_poll()` 排空队列；`fdatalog_exit` 同步 flush。 |
| **被替换的阻塞行为** | `fdatalog_wr` 写盘线程。                                                                                   |
| **源文件**      | `fdatalog.c`、`ev_timer.c`                                                                             |




#### 4.2.10 LED


| 列            | 内容                                                                                                                                     |
| ------------ | -------------------------------------------------------------------------------------------------------------------------------------- |
| **旧实现**      | `led_task`：仅 `usleep(50000)` 空转，无业务逻辑。                                                                                                 |
| **新实现**      | `led_init` 设 `ev_mode=1`，不 `pthread_create`；扫描/采样/系统灯由 `led_scan_nodes()`、`led_sample_nodes()`、`led_system()` 在业务或 1s tick 中直接调用 GPIO。 |
| **被替换的阻塞行为** | 无意义的 LED 空转线程。                                                                                                                         |
| **源文件**      | `led.c`、`ev_timer.c`                                                                                                                   |




#### 4.2.11 BLE GATT（内嵌）


| 列            | 内容                                                                                                                                                                 |
| ------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| **旧实现**      | `ble_service` 拉起独立 `my-server` 子进程；`main` 每 2s `ble_service_tick()` 监控重启。                                                                                          |
| **新实现**      | `ev_gatt.c`：`pthread_create` → `my_server_embedded_main(2, argv)`，`-U` 表示无 UART 桥接；init 前等待 `hci0` 最多 15s。shutdown 调 `my_server_embedded_stop()` + `pthread_join`。 |
| **被替换的阻塞行为** | 子进程管理；与主循环解耦的 GATT 生命周期仍保留**独立线程**（BlueZ mainloop 限制，无法并入 dispatch 线程）。                                                                                            |
| **源文件**      | `ev_gatt.c`、`ble_service.c`                                                                                                                                        |




#### 4.2.12 BLE IPC / App 命令分发（embedded + 2026-07-21 异步化）


| 列             | 内容                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 |
| ------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **旧实现**       | 非 embedded：`fnirs_ble_ipc_thread` 监听 `/tmp/fnirs_ble.sock`。embedded 早期：GATT 回调内 **同步** `fnirs_ble_dispatch_request()`，SCAN/STREAM 路径 `usleep` **阻塞 GATT 线程**（最长数秒）。                                                                                                                                                                                                                                                                                                                                |
| **新实现（当前产品）** | `FNIRS_EMBEDDED=1` + `FNIRS_EV_IO=1`：`ev_ble_async_module` 注册于 `gatt` 之前。GATT 线程 `ev_ble_async_dispatch()`：入队 job → `eventfd` + `event_base_once` 唤醒主循环 → 主线程 `fnirs_ble_ev_process_job()`。**SCAN**：`fnirs_ble_handle_scan_ev_blocking()` 在 job 回调内完成（100ms×50，对齐旧版时序），等待期间 `event_base_loop(EVLOOP_NONBLOCK)` + `ev_can_hub_service()`**（含 CAN drain）**。**STREAM_START**：仍用 `evtimer` 分阶段异步（`fnirs_ble_start_stream_async`）。GATT 线程用 `poll(socketpair)` 等响应（最长 12s/8s）。调试：`/tmp/ble_async.trace`。 |
| **被替换的阻塞行为**  | GATT 线程内 `usleep` 轮询扫描；仅处理 hub 队列不读 CAN 导致扫描 0 节点。                                                                                                                                                                                                                                                                                                                                                                                                                                                 |
| **重要说明**      | `ev_ble_ipc.c`（socket listener + worker）**仍未注册**；embedded 不需要。SCAN 未采用纯 `evtimer` 异步（实测主循环在 GATT 阻塞等待时 timer 不可靠，故改 blocking job + pump）。                                                                                                                                                                                                                                                                                                                                                          |
| **源文件**       | `ev_ble_async.c`、`fnirs_ble_ipc.c`、`mybtgatt-server.c`、`main_ev.c`                                                                                                                                                                                                                                                                                                                                                                                                                                 |




#### 4.2.13 fnode_task（EV 构建已移除）


| 列            | 内容                                                                                                                                                                                                                 |
| ------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| **旧实现**      | `SCHED_FIFO` 高优先级线程；RESET/SAMPLE/OTA/FACTORY 各态均运行**完整阻塞 handler**。另 `fhub_task` 专责 CAN 读。                                                                                                                         |
| **新实现（EV）**  | `fnode_init` **不** `pthread_create`（`tid[0/1]=0`）。`fnode_ev_idle_service()` 承担原 RESET 态 IDLE handler + 扫描；`fnode_ev_kick_idle()` 在 API 边界即时触发；采样/OTA/工厂由 `ev_can` 各 tick 驱动。非 EV 构建仍保留 `fnode_task` + `fhub_task`。 |
| **被替换的阻塞行为** | 整个 `fnode_task` / `fhub_task` 线程及其中 `usleep` 大循环。                                                                                                                                                                  |
| **源文件**      | `fnode.c`（`#ifdef FNIRS_EV_IO`）、`ev_can.c`                                                                                                                                                                         |




#### 4.2.14 Housekeeping 与调试


| 列            | 内容                                                                                                                                  |
| ------------ | ----------------------------------------------------------------------------------------------------------------------------------- |
| **旧实现**      | `main` 每 2s：`ble_service_tick()`、`led_system(1)`。                                                                                   |
| **新实现**      | `ev_timer.c` 1s tick：`led_system(1)`、`ev_can_try_attach()`、`ev_uart_retry_if_needed()`；写 `/tmp/ev_tick.count` 与 `/tmp/ev_tick.log`。 |
| **被替换的阻塞行为** | 2s 粒度主循环职责分散到专用 timer 事件。                                                                                                           |


---



## 五、什么没变、什么比表面改动小

1. **CAN 协议语义**：`fnode_CANFD_frame_handler()`、hub `fnum`、OTA 包格式、工厂 CAN 命令序列均未重写。
2. **FUSB 线协议**：帧头尾、命令字 `0x0001` SCAN、`0x0003` 采样、`0x2001`–`0x2005` OTA、`0x1001`–`0x1008` 产测保持不变。
3. **BLE 命令语义**：SCAN / SAMPLE / STREAM / 增益等仍在 `fnirs_ble_ipc.c`；EV 下 `STREAM_STOP` 故意不调用 `fNIRS_off()` 以免多次 STREAM 间重置 CAN。
4. **懒加载**：embedded 构建首次 BLE/UART 命令才 `fNIRS_lazy_init()`，降低启动期 CAN 噪声风险。

---



## 六、并发与正确性



### 6.1 已缓解


| 问题                  | 处理方式                                                                                    |
| ------------------- | --------------------------------------------------------------------------------------- |
| 信号内 teardown        | `ev_app_stop` + 模块 shutdown                                                             |
| CAN/hub 顺序          | 先 drain RX 再 hub service；`ev_can_hub_service()` **对外等价** `ev_can_pump()`（2026-07-21 修正） |
| BLE 命令竞态            | GATT 入队 + 主线程执行 job；`run_jobs_cb` 校验主线程 TID                                             |
| BLE 扫描 0 节点         | job 内必须 pump CAN，不能仅 `fnode_ev_hub_service`                                             |
| SPDATA 大包写          | `fusb_uart_write_all` + 非阻塞 fd                                                          |
| `node_ack_ready` 竞态 | `5956a59` 读/清加 `event_mtx`                                                              |




### 6.2 仍须关注


| 问题                           | 说明                                                        | 严重程度        |
| ---------------------------- | --------------------------------------------------------- | ----------- |
| GATT 阻塞等待 vs 主循环             | SCAN job 在主回调内可阻塞数秒；期间靠嵌套 `EVLOOP_NONBLOCK` + pump 维持 CAN | 中（已缓解）      |
| BLE STREAM 纯 evtimer 异步      | STREAM 仍依赖主循环调度 timer；若再遇 GATT 长阻塞需与 SCAN 同样处理            | 低           |
| `ev_ble_ipc` 未注册             | 仅影响非 embedded EV 构建                                       | 低（当前产品 N/A） |
| EV 进 SAMPLE 无 `sleep(1)`     | 2026-07-21 已去掉 EV 路径 1s 延迟，直接 `fnode_ev_sample_begin`     | 已关闭         |
| `msgq_snd` 满时 `usleep(10ms)` | 仍在部分路径                                                    | 低           |


---



## 七、板上验证状态（x2600halley7）


| 项目                    | 结果  | 说明                                                                               |
| --------------------- | --- | -------------------------------------------------------------------------------- |
| 进程与 libevent          | ✅   | 产品进程 `my-fnirs`；`libevent-2.1.so.7`                                              |
| BLE 扫描/采样/STREAM      | ✅   | 2026-07-21 修复 SCAN 跨线程 + CAN pump；节点数与 App 一致；`ble_async.trace` 无 `poll timeout` |
| BLE 配置命令 (0x04–0x06)  | ✅   | SET_GAIN / LED_ARRAY / STREAM_CH：`dispatch ok`                                   |
| BLE 录制状态 (0x0a)       | ✅   | 开流后多次 `RECORD_STAT` 均 `dispatch ok`                                              |
| PC Hub 12 节点 + SPDATA | ✅   | `nodes=12 size=14016`                                                            |
| 采样结束                  | ✅   | `fnode_ev_sample_finish`                                                         |
| UART 心跳               | ✅   | ~3s `FUSB_HBEAT`                                                                 |
| 电池 sysfs 上报           | N/A | 测试板无 `bq25890-charger`                                                           |
| 工厂产测                  | ☐   | 需产测软件                                                                            |
| OTA                   | ☐   | Phase E 事件路径已合入；需 OTA 工具 + `.bin` 复测                                             |
| 30min 长稳              | ☐   | 建议补测                                                                             |


---



## 八、线程与资源对比


| 指标                                   | 旧版 pthread `my-fnirs`                      | 新版统一 `my-fnirs`（原 my-fnirs-ev）                |
| ------------------------------------ | ------------------------------------------ | --------------------------------------------- |
| 业务 pthread                           | ~6～7（fhub/fnode/fusb/fdatalog/led/ble_ipc） | **~1**（GATT + dispatch 主线程；无 fnode/fhub task） |
| 独立 my-server 子进程                     | 有                                          | 无                                             |
| fnode_task / fhub_task               | 有                                          | **EV 构建无**                                    |
| fusb_task / fdatalog_task / led_task | 有                                          | **无**                                         |
| 空闲 CPU                               | 多线程周期性 wake                                | 事件驱动，无事则 epoll_wait                           |
| OTA 执行模型                             | fnode_task 阻塞                              | **ev_can 10ms +** `fnode_ev_ota`_*            |
| 采样/产测执行模型                            | fnode_task 阻塞                              | ev_can timer + 状态机                            |


---



## 九、结论与建议

1. **日常功能迁移已完成**：BLE、PC Hub、CAN 采样、UART SPDATA、心跳均在 libevent 栈验证通过；产品进程名为统一的 `my-fnirs`。
2. **2026-07-21 增量**：去掉 EV 下 `fnode_task`；BLE 异步化 + SCAN blocking/CAN pump；UART `EV_READ` 收包 + housekeeping 分离。
3. **OTA 策略**：Phase E 事件化 OTA 为当前代码路径；代码已合入，**正式签字**仍需 OTA 工具 + `.bin` 端到端复测（见第七节）。
4. **工厂产测**：CAN 序列已事件化（Phase F）；**正式签字**仍需产线上位机端到端复测（见第七节）。
5. **可选后续（非上线阻塞）**：`ev_app_create` 对 `evthread_`* 失败 fail-fast；fdatalog eventfd 唤醒；清理 `#ifndef FNIRS_EV_IO` dead code；非 embedded 注册 `ev_ble_ipc_module`。**不建议**为追求「单一 loop」合并 BlueZ mainloop。

---



## 十、2026-07-21 工作日志（上午 vs 下午）



### 10.1 上午：去掉 `fnode_task`，补齐 EV 空闲状态机


| 项目     | 内容                                                                                                                                                                                                                                                                      |
| ------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **动机** | `fNIRS_on()` 仅置 `sample=1`，原由 `fnode_task` 的 IDLE handler 真正启动采样；EV 下若只删线程会导致采样/扫描/OTA 入口断裂。                                                                                                                                                                            |
| **改动** | `fnode_ev_idle_service()`：合并 reset、`fnode_task_IDLE_handler`、CAN SCAN；`fnode_ev_kick_idle()` 在 `fnode_reset` / `fnode_sample_on` 等 API 即时调用；`fnode_ev_apply_reset()` 处理 hub 事件；`fnode_init` 在 `FNIRS_EV_IO` 下不创建 `fnode_task`/`fhub_task`；进 SAMPLE 去掉 EV 路径 `sleep(1)`。 |
| **验证** | 板测 BLE 扫描、STREAM 采集路径正常（与下午 SCAN 修复为同一轮联调；OTA/工厂见第七节签字项）。                                                                                                                                                                                                               |




### 10.2 下午：BLE 异步化与 SCAN 修复（迭代过程）


| 阶段                         | 做法                                                               | 结果                                     |
| -------------------------- | ---------------------------------------------------------------- | -------------------------------------- |
| **A. 初版** `ev_ble_async`   | GATT 入队 → eventfd → 主线程 job；SCAN/STREAM 用 `evtimer` 异步           | GATT 能 `dispatch`，但 12s 超时，timer 未可靠完成 |
| **B. 唤醒与线程**               | `event_base_once` 唤醒；禁止 GATT 调 `event_base_loop`；主线程 TID 校验      | 能 `process job`，SCAN 仍超时               |
| **C. SCAN 改 blocking job** | 主线程 `fnirs_ble_handle_scan_ev_blocking` + `EVLOOP_NONBLOCK` 等待   | IPC 成功，但 **0/12 节点**                   |
| **D. 时序对齐**                | 100ms×50、reset 后 300ms、`fnode_ev_idle_service`、采集中先 `fNIRS_off`  | 仍 0 节点                                 |
| **E. 根因修复**                | `ev_can_hub_service()` **→** `ev_can_pump()`（先 `read` CAN 再 hub） | **扫描节点数正常**                            |




### 10.3 关键教训

1. `desc[].alive` **仅在** `FHUB_STATE_RESET` **下由 SCAN ACK 置位**；`fNIRS_reset()` 会清空 `desc`，扫描必须重新 discover。
2. **在 BLE job 回调内阻塞时**，不能假设 libevent 会像平时一样处理 CAN `EV_READ`；必须显式 `ev_can_drain`。
3. **GATT 线程仍需同步等待 App**（`poll` + socketpair），与「完全异步通知」不同；当前是 **主线程执行重活 + GATT 阻塞等结果**。



### 10.4 傍晚：UART `EV_READ`


| 项目     | 内容                                                                                                                                        |
| ------ | ----------------------------------------------------------------------------------------------------------------------------------------- |
| **动机** | 20ms timer 轮询读 UART，RX 最坏延迟 20ms，且读写混在同一定时器回调。                                                                                            |
| **改动** | `ev_uart.c`：UART fd `EV_READ` → `fusb_ev_drain_read()`；20ms timer 改名为 housekeeping，仅 `fusb_ev_poll()`。`fusb.c` 新增 `fusb_ev_drain_read()`。 |
| **验证** | 启动 `uart open (EV_READ): /dev/ttyS1 @ 2000000`；PC Hub 采样 `fusb SPDATA tx #N nodes=12 size=14016`。                                         |


---



## 附录 A：关键源文件索引


| 文件                                      | 职责                                                       |
| --------------------------------------- | -------------------------------------------------------- |
| `fnirs/src/main.c`                      | 旧版入口与信号处理（对照用；产品构建不编此入口）                                 |
| `ev/src/main_ev.c`                      | 新版入口、模块表、致命信号 hook（产品 `my-fnirs`）                        |
| `ev/src/ev_app.c`                       | `event_base` 生命周期                                        |
| `ev/src/ev_signal.c`                    | SIGINT/SIGTERM                                           |
| `ev/src/ev_can.c`                       | CAN 读 + timerfd；`ev_can_pump` **/** `ev_can_hub_service` |
| `ev/src/ev_uart.c`                      | UART `EV_READ` + 20ms FUSB housekeeping                  |
| `ev/src/ev_timer.c`                     | 1s tick + 50ms fdatalog                                  |
| `ev/src/ev_gatt.c`                      | 内嵌 GATT 线程                                               |
| `ev/src/ev_ble_async.c`                 | GATT→主线程 BLE 任务队列                                        |
| `ev/src/ev_fnirs.c`                     | 业务 init/shutdown                                         |
| `fnirs/src/fnirs_ble_ipc.c`             | BLE 命令、SCAN blocking、STREAM 异步状态机                        |
| `ev/src/ev_ble_ipc.c`                   | socket IPC（未注册）                                          |
| `fnirs/src/fnode.c`                     | CAN 协议、EV 状态机（含 `fnode_ev_ota_*`）                        |
| `fnirs/src/fusb.c`                      | `fusb_ev_poll` / `fusb_ev_drain_read`、大包写                |
| `packages/example/x2600-fNIRS/Build.mk` | 唯一产品目标 `my-fnirs`；含 `ev_ble_async.c`                     |




## 附录 B：迁移阶段与提交对照


| 阶段                            | 内容                                                  | 代表性提交 / 状态                  |
| ----------------------------- | --------------------------------------------------- | --------------------------- |
| 基础框架                          | `ev_app`、内嵌 GATT、部署脚本                               | `29bd245` 等                 |
| B                             | IDLE CAN 扫描 → ev_can 100ms                          | 已 push                      |
| C                             | 采样 → `fnode_ev_sample_*` 1ms tick                   | `262a697`                   |
| D                             | led / fdatalog 去线程                                  | 已 push                      |
| E                             | OTA 事件状态机                                           | `e607bdf`（**当前仍使用**）        |
| F                             | 工厂测试 → `fnode_ev_factory_*`                         | `a2f5086`                   |
| 修补                            | 电池 UART、跳过 ble_service                              | `e721fe3`                   |
| 修补                            | `node_ack_ready` 锁                                  | `5956a59`                   |
| 产品命名                          | `my-fnirs-ev` 并入 `my-fnirs`                         | 工作区 `Build.mk` / `S99myapp` |
| **G. fnode_task 移除 + BLE 异步** | `fnode_ev_idle_service`；`ev_ble_async`；SCAN pump 修复 | **2026-07-21 工作区**          |
| **H. UART EV_READ**           | `fusb_ev_drain_read`；housekeeping 与 RX 分离           | **2026-07-21 工作区**          |


---

*文档版本：2026-07-21 v2（含管理层摘要、PDF 评审对照、板测日志更新、分支名与模块顺序修正）*
