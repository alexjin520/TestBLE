# fNIRS Event-Loop Refactor

## libevent Production Backend and libuv Comparison Backend

**Repository:** `alexjin520/TestBLE`

**Target:** Ingenic X2600 / `x2600halley7`

**Report revision:** 2026-07-30

## Executive summary

The board service was refactored from a collection of polling threads into a
module-oriented event-loop application. The production binary, `my-fnirs`,
uses libevent. A second binary, `my-fnirs-uv`, runs the same fNIRS, CAN-FD,
UART, recording, OTA, BLE, and embedded GATT sources through a small
libevent-compatible adapter implemented on libuv.

The refactor changes scheduling and lifecycle management, not the external
protocol. CAN-FD frames, BLE commands, GATT characteristics, recording
formats, and node OTA packets remain shared between the two builds.

| Area | Current status |
| --- | --- |
| Production backend | `my-fnirs` with libevent |
| Comparison backend | `my-fnirs-uv` with libuv |
| Default at boot | libevent |
| Business sources | Shared by both binaries |
| Simultaneous execution | Prohibited; both backends own the same hardware |
| libevent hardware validation | Scan, sampling, timestamps, recording/file sync, and OTA validated |
| libuv validation | Build, startup controls, safe probe, and staged activation available; full hardware parity remains a comparison target |

The main architectural result is one controlled application lifecycle and one
event-driven path for CAN-FD, UART, timers, signals, recording, and BLE command
dispatch. Embedded BlueZ GATT retains a dedicated worker boundary where
required by its main-loop and callback model.

## Goals and non-goals

### Goals

- Replace top-level polling and idle threads with readiness and timer events.
- Keep one implementation of the fNIRS business protocol.
- Make initialization and shutdown ordered, observable, and reversible.
- Isolate the event-loop backend from the product logic.
- Provide a guarded libuv build for direct comparison with libevent.
- Preserve the BLE, UART, CAN-FD, file, sampling, and OTA protocols.

### Non-goals

- Running libevent and libuv processes at the same time.
- Rewriting the BlueZ GATT implementation around native libuv APIs.
- Claiming that the libuv comparison build is already the production default.
- Removing every thread from the process.

## Architecture before and after

Before the refactor, the service relied on multiple worker threads and
periodic sleeps. Ownership of startup, shutdown, and hardware I/O was spread
across the application.

```text
Legacy service
├── top-level keep-alive loop
├── CAN / node polling threads
├── hub polling thread
├── UART polling thread
├── recording polling
└── separate BLE / GATT execution
```

The refactored service centralizes module registration and event dispatch:

```text
my-fnirs process
│
├── ev_app lifecycle
│   ├── signal module
│   ├── fNIRS business module
│   ├── CAN-FD module
│   ├── UART / FUSB module
│   ├── BLE cross-thread dispatch module
│   ├── embedded BlueZ GATT module
│   └── housekeeping / recording timer module
│
├── libevent backend (production)
│   └── event_base_dispatch()
│
└── libuv backend (comparison)
    └── libevent-compatible adapter → uv_run()
```

`ev_app` initializes modules in declaration order. If an initialization step
fails, already initialized modules are shut down in reverse order. Normal
shutdown uses the same reverse-order rule.

## Shared-source backend design

The two products are defined in
[`Build.mk`](../sdk-overlay/packages/example/x2600-fNIRS/Build.mk).

| Property | `my-fnirs` | `my-fnirs-uv` |
| --- | --- | --- |
| Intended role | Production | Controlled comparison |
| Event library | libevent | libuv |
| Main application modules | Shared | Shared |
| fNIRS/CAN/UART/recording/OTA sources | Shared | Shared |
| Embedded GATT sources | Shared | Shared |
| Backend flag | `FNIRS_EV_IO=1` | `FNIRS_EV_IO=1`, `FNIRS_UV_BACKEND=1` |
| Main linked loop library | `libevent`, `libevent_pthreads` | `libuv` |
| Preferred runtime executable | `/app_data/golgi/my-fnirs` | `/app_data/golgi/my-fnirs-uv` |
| Read-only image fallback | `/opt/golgi/my-fnirs` | `/opt/golgi/my-fnirs-uv` |

This arrangement is intentionally not two independent implementations. A
business-layer fix is compiled into both binaries, while backend-specific code
is limited to the event compatibility and startup boundary.

## Common module pipeline

| Module | Responsibility | Event source |
| --- | --- | --- |
| `ev_signal` | Graceful SIGINT/SIGTERM handling | Signal event |
| `ev_fnirs` | fNIRS state and business-layer lifecycle | Application module |
| `ev_can` | CAN-FD receive, node scan, sampling, factory flow, OTA | CAN fd and timerfds |
| `ev_uart` | UART receive and FUSB housekeeping | UART fd and timerfd |
| `ev_ble_async` | GATT-to-main-loop command dispatch and wakeup | eventfd/socketpair |
| `ev_gatt` | Embedded BlueZ GATT server | GATT worker and readiness events |
| `ev_timer` | Recording and periodic housekeeping | timerfds |

Important scheduling periods currently include:

| Operation | Period |
| --- | ---: |
| Hub polling | 20 ms |
| Idle node scan service | 100 ms |
| Sampling state-machine service | 1 ms |
| OTA state-machine service | 10 ms |
| Factory state-machine service | 1 ms |
| UART/FUSB housekeeping | 20 ms |
| Recording/file polling | 50 ms |
| General housekeeping | 1 s |

These periods describe service opportunities, not guaranteed end-to-end
latency. Device response time, CAN arbitration, storage, and BLE transport
still contribute to observed timing.

## libevent production backend

The production binary uses a normal libevent `event_base`.

- File descriptors are registered with persistent read events.
- CAN-FD and UART input are drained when readiness is reported.
- `timerfd` sources drive scan, sampling, OTA, factory, recording, and
  housekeeping state machines.
- `evsignal_new()` handles termination signals.
- Cross-thread BLE work is posted back to the main event loop.
- `event_base_loopbreak()` provides controlled shutdown.

This design removes the old top-level sleep loop and the dedicated
`fnode_task`/`fhub_task` polling threads from the event build. It does not force
BlueZ GATT callbacks into the same thread; the GATT integration keeps a worker
boundary and explicitly dispatches product work to the main loop.

## libuv comparison backend

The libuv build prepends a compatibility layer to the same event-oriented
application. The current adapter is implemented in
[`uv_event_compat.c`](../sdk-overlay/packages/example/x2600-fNIRS/uv/src/uv_event_compat.c).

### API mapping

| Application-facing operation | libuv implementation |
| --- | --- |
| `event_base_dispatch()` | `uv_run(loop, UV_RUN_DEFAULT)` |
| Non-blocking `event_base_loop()` | `uv_run(loop, UV_RUN_NOWAIT)` |
| `event_base_loopbreak()` | `uv_stop()` plus async wakeup |
| Persistent fd read/write event | `uv_poll_t` |
| Timer event | `uv_timer_t` |
| Signal event | `uv_signal_t` |
| Immediate cross-thread one-shot work | Mutex-protected queue plus `uv_async_t` |
| CAN/UART/GATT/eventfd/timerfd readiness | `uv_poll_t` |

The adapter uses `uv_default_loop()` and implements only the libevent subset
used by the current application. It is a controlled compatibility backend, not
a general replacement for libevent.

### Current constraints

- The compatibility implementation of `event_base_once()` schedules immediate
  work through `uv_async_t`; its timeout argument is not implemented.
- The `evthread_use_pthreads()` and `evthread_make_base_notifiable()` adapter
  functions are compatibility no-ops.
- Cross-thread safety exists for the adapter's explicit queued-work path. New
  code must not assume that every libevent thread guarantee is reproduced.
- GATT uses a dedicated worker boundary in the libuv build so BlueZ callbacks
  do not compete with `uv_default_loop()` ownership.
- Every active libuv handle must be stopped and closed before the loop can be
  considered fully drained.

The current application uses `event_base_once()` for immediate dispatch. If a
future feature depends on delayed one-shot semantics, the adapter must be
extended before that feature is enabled in the UV build.

## Runtime selection and safety controls

Runtime defaults are stored in `/etc/default/fnirs`:

```text
FNIRS_BACKEND=event
FNIRS_UV_AUTOBOOT=0
FNIRS_UV_PROFILE=full
FNIRS_UV_SAFE_MODE=0
```

The init service enforces single ownership of CAN-FD, UART, Bluetooth, and
recording resources. It verifies the executable behind the running PID instead
of relying only on a stale PID file.

The UV backend supports staged profiles:

| Profile | Enabled modules | Intended use |
| --- | --- | --- |
| `safe` | Signal handling only | Loader and event-loop probe |
| `business` | Signal plus fNIRS business initialization | Business-layer isolation |
| `core` | Signal, fNIRS, CAN, UART, and timers | Hardware core without GATT |
| `full` | All shared modules including BLE/GATT | End-to-end comparison |

The safe probe starts `my-fnirs-uv` briefly without taking hardware ownership
away from the running event backend. A successful probe records a fingerprint
of the tested binary. Switching to UV requires the currently installed binary
to match that probe result.

Unless UV autoboot is explicitly enabled, a reboot returns to the event
backend. A failed UV activation also triggers rollback to the production event
service.

Example board commands:

```sh
/etc/init.d/S99myapp status
/etc/init.d/S99myapp probe uv
/etc/init.d/S99myapp switch uv safe
/etc/init.d/S99myapp switch uv core
/etc/init.d/S99myapp switch uv full
/etc/init.d/S99myapp switch event
```

## Product paths preserved by the refactor

### Node scan

BLE scan commands are dispatched from the GATT boundary to the event-loop
business path. Idle scanning is serviced every 100 ms and CAN-FD replies update
the shared node state. Scan responses use the same external protocol in both
builds.

### Sampling and timestamps

The 10 Hz sample-and-upload path uses the setup/upload command for normal
channels and the last-data/upload command to flush the final channel.
`fhub_append()` adds one 8-byte big-endian millisecond timestamp to each
recorded sample frame before the sample data.

A board recording check reported:

```text
Status       : PASS
Timestamp1Ms : 252097
Timestamp2Ms : 252187
DeltaMs      : 90
```

The 90 ms observed delta is close to the nominal 100 ms acquisition period.
It is a measured sample, not a hard real-time guarantee.

### Recording and BLE file synchronization

Recording data is polled without restoring the old dedicated polling thread.
The BLE file path supports:

- live synchronization while a recording is open;
- asynchronous finalization after collection stops;
- sealed/final-file notification;
- cumulative acknowledgements and retry handling;
- CRC-protected transport packets.

The same recording and GATT business sources are compiled into both backends.

### Node OTA

The host file path validates OTA start, package, and finish commands, including
the announced size and CRC. The CAN-FD OTA state machine then services start,
package transfer, completion, and result reporting without blocking the main
loop.

Recent event-backend board logs confirmed:

- the uploaded firmware file passed size and CRC validation;
- three nodes acknowledged OTA start;
- the same three nodes acknowledged OTA finish;
- the completion bitmap was queued and returned through the host protocol.

This replaces the older report state in which OTA still required end-to-end
sign-off. It does not by itself establish full OTA parity for the libuv
comparison backend.

## Concurrency boundaries

The process is event-driven, but it is not described as universally
single-threaded:

```text
BlueZ/GATT worker
       │ queued command + wakeup
       ▼
main event loop
       │
       ├── fNIRS state
       ├── CAN-FD
       ├── UART/FUSB
       ├── OTA and sampling state machines
       └── recording/file service
```

Shared product state should be mutated on the main event-loop side. GATT
callbacks enqueue work rather than directly taking over CAN, UART, or fNIRS
state. Blocking operations in event callbacks remain a risk because they delay
all work owned by that loop.

## Validation status

The following table deliberately separates production evidence from UV
comparison status.

| Capability | libevent production | libuv comparison |
| --- | --- | --- |
| SDK build | Validated | Validated |
| Process lifecycle and signals | Validated | Build/probe validated |
| Safe staged startup | Not required | Available |
| CAN-FD node scan | Board validated | Full-profile parity test required |
| 10 Hz sampling/upload | Board validated | Full-profile parity test required |
| Embedded timestamps | Board validated | Shared source; hardware parity test required |
| UART/FUSB receive and housekeeping | Board validated | Core/full parity test required |
| Recording and live file sync | Board validated | Full-profile parity test required |
| Node OTA start/transfer/finish/result | Board validated | Full-profile parity test required |
| BLE/GATT end-to-end workflow | Board validated | Full-profile parity test required |
| Failure rollback to event | Runtime guard implemented | Activation path guarded |

“Shared source” reduces behavioral drift, but it is not a substitute for board
testing. The UV backend should remain opt-in until the same hardware matrix has
passed under `core` and `full` profiles.

## Build and deployment

Build both products from the SDK:

```bash
source build/envsetup.sh
lunch x2600halley7.v10_msc_5.10-eng
make my-fnirs my-fnirs-uv
```

Deploy the production event backend from PowerShell:

```powershell
cd device\x2600halley7
.\deploy-my-fnirs-ev.ps1 -Build
```

Deploy and probe the UV comparison build separately:

```powershell
.\deploy-my-fnirs-uv.ps1 -Build
```

Do not manually start both executables. Use `S99myapp` for backend selection so
that ownership checks and rollback rules remain active.

Useful logs include:

```text
/tmp/fnirs.log
/app_data/golgi/fnirs-uv.log
/app_data/golgi/uv-stage.log
/app_data/golgi/fnirs-uv-probe.log
```

## Recommended UV parity sequence

1. Build and deploy both binaries from the same source revision.
2. Confirm that the event backend is healthy.
3. Run the UV safe probe and verify its binary fingerprint.
4. Switch to `business`, then `core`, and inspect startup/shutdown logs.
5. Under `core`, test CAN-FD scan, UART, sampling timing, timestamps, and OTA.
6. Under `full`, test BLE scan, LED configuration, waveforms, recording, live
   file sync, finalized download, and OTA result delivery.
7. Run a long-duration collection test and check CPU, memory, CAN errors, file
   integrity, and shutdown behavior.
8. Switch back to event and verify that the production service reacquires all
   hardware cleanly.

## Trade-offs and remaining work

### Benefits

- One business implementation for both backends.
- Fewer idle polling threads and less top-level sleep-based scheduling.
- Explicit module initialization, rollback, and shutdown.
- Readiness-driven CAN-FD and UART receive paths.
- Non-blocking state machines for sampling, factory operations, and OTA.
- A measurable way to compare libevent and libuv without changing the
  external protocol.

### Remaining work

- Complete the UV `core` and `full` board-validation matrix.
- Add delayed `event_base_once()` semantics before any UV feature depends on
  them.
- Add automated lifecycle tests for partial initialization failure and handle
  closure.
- Repeat long-duration sampling, live-file, and OTA tests under both backends.
- Continue auditing callbacks for blocking storage, Bluetooth, or protocol
  operations.

## Source map

- Application lifecycle:
  [`ev_app.c`](../sdk-overlay/packages/example/x2600-fNIRS/ev/src/ev_app.c)
- Shared module registration:
  [`main_ev.c`](../sdk-overlay/packages/example/x2600-fNIRS/ev/src/main_ev.c)
- CAN-FD event integration:
  [`ev_can.c`](../sdk-overlay/packages/example/x2600-fNIRS/ev/src/ev_can.c)
- UART/FUSB event integration:
  [`ev_uart.c`](../sdk-overlay/packages/example/x2600-fNIRS/ev/src/ev_uart.c)
- BLE cross-thread dispatch:
  [`ev_ble_async.c`](../sdk-overlay/packages/example/x2600-fNIRS/ev/src/ev_ble_async.c)
- Embedded GATT integration:
  [`ev_gatt.c`](../sdk-overlay/packages/example/x2600-fNIRS/ev/src/ev_gatt.c)
- libuv compatibility layer:
  [`uv_event_compat.c`](../sdk-overlay/packages/example/x2600-fNIRS/uv/src/uv_event_compat.c)
- libuv backend notes:
  [`uv/README.md`](../sdk-overlay/packages/example/x2600-fNIRS/uv/README.md)
- Build definitions:
  [`Build.mk`](../sdk-overlay/packages/example/x2600-fNIRS/Build.mk)
- Runtime defaults:
  [`fnirs`](../sdk-overlay/device/x2600halley7/rootfs-overlay/etc/default/fnirs)
- Runtime backend manager:
  [`S99myapp`](../sdk-overlay/device/x2600halley7/rootfs-overlay/etc/init.d/S99myapp)

## Conclusion

The production refactor is complete enough to operate the current fNIRS
workflow through libevent, including node discovery, timestamped sampling,
recording/file synchronization, and node OTA. The libuv backend now provides a
meaningful comparison because it compiles the same product sources and changes
only the event-loop boundary.

The correct deployment position remains conservative: libevent is the default
production backend, while libuv is opt-in, staged, observable, and subject to
the remaining hardware-parity tests.
