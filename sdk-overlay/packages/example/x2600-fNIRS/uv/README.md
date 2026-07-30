# my-fnirs-uv

`my-fnirs-uv` is the libuv comparison build for the production
`my-fnirs` (libevent) process.

Both binaries compile the same fNIRS, CAN FD, UART, BLE/GATT, sampling state
machine, compiler optimization and logging sources. The UV build places a
small `event2` compatibility header before the sysroot headers and implements
the API subset used by the application with:

- `uv_loop_t` for dispatch
- `uv_poll_t` for CAN, UART, GATT, eventfd and timerfd readiness
- `uv_timer_t` for application one-shot timers
- `uv_signal_t` for SIGINT/SIGTERM
- `uv_async_t` for cross-thread BLE-to-main-loop wakeups

This arrangement deliberately avoids copying `fnode.c`, so a measurement
difference is attributable to the event-loop backend rather than divergent
sampling logic.

## Build

```sh
source build/envsetup.sh
lunch x2600halley7.v10_msc_5.10-eng
make my-fnirs my-fnirs-uv
```

## Select one backend

The two processes must never run together because they share CAN, UART and
Bluetooth resources.

```sh
echo FNIRS_BACKEND=event >/etc/default/fnirs
/etc/init.d/S99myapp restart

echo FNIRS_BACKEND=uv >/etc/default/fnirs
/etc/init.d/S99myapp restart
```

The default remains `event`. The runtime banner identifies the selected
implementation.
