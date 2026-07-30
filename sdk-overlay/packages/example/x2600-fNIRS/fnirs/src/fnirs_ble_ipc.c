
#include "stdio.h"
#include "string.h"
#include "errno.h"
#include "unistd.h"
#include "fcntl.h"
#include "sys/stat.h"
#include "sys/socket.h"
#include "sys/un.h"
#include "pthread.h"

#include "fnirs_ble_ipc.h"
#include "fnirs_stream.h"
#include "fNIRS.h"
#include "fnode.h"
#include "fdatalog.h"
#include "my_interface.h"

#if defined(FNIRS_EMBEDDED) && defined(FNIRS_EV_IO)
#include "ev_ble_async.h"
#include "ev_can.h"
#include <event2/event.h>
#include <time.h>

#define BLE_SCAN_TICK_MS        50
#define BLE_SCAN_MAX_TICKS      40
#define BLE_SCAN_MIN_TICKS      6
#define BLE_SCAN_STABLE_REQ     2

/* Match legacy fnirs_ble_handle_scan (100ms x 50, min 40 stable). */
#define BLE_SCAN_BLOCK_TICK_MS  100
#define BLE_SCAN_BLOCK_MAX_TRY  50
#define BLE_SCAN_BLOCK_MIN_TRY  40

#define BLE_STREAM_OFF_WAIT_MS  200
#define BLE_STREAM_RETRY_WAIT_MS 500

typedef enum {
    BLE_STREAM_PHASE_OFF_WAIT = 0,
    BLE_STREAM_PHASE_ON,
    BLE_STREAM_PHASE_RETRY_WAIT,
} ble_stream_phase_t;

typedef struct {
    int active;
    int rsp_fd;
    int tick;
    uint8_t prev_nodes;
    int stable;
    struct event *timer;
} ble_scan_async_t;

typedef struct {
    int active;
    int rsp_fd;
    ble_stream_phase_t phase;
    int retry;
    struct event *timer;
} ble_stream_async_t;

static ble_scan_async_t g_ble_scan;
static ble_stream_async_t g_ble_stream;
static struct event_base *g_ble_ev_base;

static void fnirs_ble_scan_async_disarm(void);
static void fnirs_ble_start_scan_async(int rsp_fd);
static void fnirs_ble_start_stream_async(int rsp_fd);
static int32_t fnirs_ble_handle_sample_on(int fd);
static int32_t fnirs_ble_handle_sample_off(int fd);
static int32_t fnirs_ble_handle_gain(int fd, const uint8_t *payload, uint16_t len);
static int32_t fnirs_ble_handle_led_array(int fd, const uint8_t *payload, uint16_t len);
static int32_t fnirs_ble_handle_stream_ch(int fd, const uint8_t *payload, uint16_t len);
static int32_t fnirs_ble_handle_stream_stop(int fd);
static int32_t fnirs_ble_handle_hangzhou_path(int fd);
static int32_t fnirs_ble_handle_record_stat(int fd);
#endif

static volatile int g_ble_ipc_run;
static volatile int g_ble_stream_want;
static fnirs_ble_stream_cfg_t g_stream_cfg;
static pthread_mutex_t g_stream_cfg_mtx = PTHREAD_MUTEX_INITIALIZER;
#ifndef FNIRS_EV_IO
static pthread_t g_ble_ipc_tid;
#endif

static void fnirs_ble_default_stream_cfg(fnirs_ble_stream_cfg_t *cfg)
{
    static const fnirs_ble_stream_ch_t defs[] = {
        { 10, 2, 2, 1 },
        { 10, 2, 3, 1 },
        { 10, 2, 4, 1 },
        { 10, 2, 5, 1 },
    };

    memset(cfg, 0, sizeof(*cfg));
    cfg->count = sizeof(defs) / sizeof(defs[0]);
    memcpy(cfg->ch, defs, sizeof(defs));
}

static int32_t fnirs_ble_send_rsp(int fd, uint8_t cmd, uint8_t status,
                                  const void *payload, uint16_t len)
{
    uint8_t buf[6 + 512];
    ssize_t n;
    uint16_t total;

    if (len > 512)
        return -1;

    buf[0] = FNIRS_BLE_RSP_MAGIC0;
    buf[1] = FNIRS_BLE_RSP_MAGIC1;
    buf[2] = cmd;
    buf[3] = status;
    buf[4] = (uint8_t)(len & 0xff);
    buf[5] = (uint8_t)((len >> 8) & 0xff);
    if (len > 0 && payload)
        memcpy(buf + 6, payload, len);

    total = (uint16_t)(6 + len);
    n = write(fd, buf, total);
    if (n != (ssize_t)total)
        return -1;

    return 0;
}

#if defined(FNIRS_EMBEDDED) && defined(FNIRS_EV_IO)
static void fnirs_ble_ev_spin_ms(int ms)
{
    struct timespec until;
    struct timespec now;
    int slice_ms = 5;

    if (!g_ble_ev_base || ms <= 0)
        return;

    clock_gettime(CLOCK_MONOTONIC, &until);
    until.tv_sec += ms / 1000;
    until.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }

    for (;;) {
        /*
         * This function runs from the application's libevent thread.  Calling
         * event_base_loop() here would recursively enter the same event base
         * and libevent rejects that with "reentrant invocation".  The scan's
         * blocking compatibility path only needs to service CAN explicitly;
         * the outer event loop resumes after the scan response is complete.
         */
        ev_can_hub_service();

        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > until.tv_sec ||
            (now.tv_sec == until.tv_sec && now.tv_nsec >= until.tv_nsec))
            break;

        if (slice_ms > ms)
            slice_ms = ms;
        usleep((useconds_t)slice_ms * 1000U);
    }
}

static int32_t fnirs_ble_handle_scan_ev_blocking(int fd)
{
    fnode_desc_t desc[FNODE_NID_MAX];
    uint8_t alive[FNODE_NID_MAX];
    fnode_handler_t fh;
    uint32_t i;
    int try;
    uint8_t nodes = 0;
    uint8_t prev_nodes = 0;
    int stable = 0;

    fnirs_ble_scan_async_disarm();

    WLOGI("BLE scan blocking: start fd=%d\r\n", fd);

    (void)fNIRS_lazy_init();
    memset(alive, 0, sizeof(alive));

    /*
     * The idle CAN scanner keeps node descriptions fresh.  Do not clear that
     * cache on a BLE scan if UART or background scan has already found nodes.
     */
    if (fNIRS_desc(desc) == 0) {
        nodes = 0;
        for (i = 0; i < FNODE_NID_MAX; i++) {
            alive[i] = desc[i].alive ? 1 : 0;
            if (alive[i])
                nodes++;
        }

        if (nodes > 0) {
            WLOGI("BLE scan: %u cached nodes online\r\n", nodes);
            fnirs_ble_send_rsp(fd, FNIRS_BLE_CMD_SCAN, FNIRS_BLE_STATUS_OK,
                               alive, FNODE_NID_MAX);
            shutdown(fd, SHUT_WR);
            close(fd);
            return 0;
        }
    }

    if (fNIRS_ison())
        (void)fNIRS_off();
    else
        (void)fNIRS_reset();

    fh = fNIRS_handler_ref();
    if (fh) {
        fnode_ev_kick_idle(fh);
        fnode_ev_hub_service(fh);
        ev_can_hub_service();
    }

    /* Let SP_RESET clear desc and hub enter RESET before counting. */
    fnirs_ble_ev_spin_ms(300);

    for (try = 0; try < BLE_SCAN_BLOCK_MAX_TRY; try++) {
        fh = fNIRS_handler_ref();
        if (fh)
            fnode_ev_idle_service(fh);
        ev_can_hub_service();

        if (fNIRS_desc(desc) == 0) {
            nodes = 0;
            for (i = 0; i < FNODE_NID_MAX; i++) {
                alive[i] = desc[i].alive ? 1 : 0;
                if (alive[i])
                    nodes++;
            }
        }

        if (nodes == prev_nodes && nodes > 0)
            stable++;
        else
            stable = 0;
        prev_nodes = nodes;

        if (try + 1 >= BLE_SCAN_BLOCK_MIN_TRY &&
            stable >= BLE_SCAN_STABLE_REQ)
            break;

        if (try + 1 < BLE_SCAN_BLOCK_MAX_TRY)
            fnirs_ble_ev_spin_ms(BLE_SCAN_BLOCK_TICK_MS);
    }

    WLOGI("BLE scan: %u nodes online (ticks=%d)\r\n", nodes, try + 1);
    fnirs_ble_send_rsp(fd, FNIRS_BLE_CMD_SCAN, FNIRS_BLE_STATUS_OK,
                       alive, FNODE_NID_MAX);
    shutdown(fd, SHUT_WR);
    close(fd);
    return 0;
}
#endif

#if defined(FNIRS_EMBEDDED) && defined(FNIRS_EV_IO)
static void fnirs_ble_scan_async_disarm(void)
{
    if (g_ble_scan.timer)
        event_del(g_ble_scan.timer);
    g_ble_scan.active = 0;
    g_ble_scan.rsp_fd = -1;
}

static void fnirs_ble_scan_async_finish(uint8_t status, const uint8_t *alive)
{
    int fd = g_ble_scan.rsp_fd;
    uint8_t empty[FNODE_NID_MAX];

    fnirs_ble_scan_async_disarm();
    if (fd < 0)
        return;

    if (!alive) {
        memset(empty, 0, sizeof(empty));
        alive = empty;
    }

    fnirs_ble_send_rsp(fd, FNIRS_BLE_CMD_SCAN, status, alive, FNODE_NID_MAX);
    shutdown(fd, SHUT_WR);
    close(fd);
}

static void fnirs_ble_scan_async_tick(evutil_socket_t fd, short events, void *arg)
{
    fnode_desc_t desc[FNODE_NID_MAX];
    uint8_t alive[FNODE_NID_MAX];
    uint32_t i;
    uint8_t nodes = 0;
    struct timeval tv;

    (void)fd;
    (void)events;
    (void)arg;

    if (!g_ble_scan.active)
        return;

    g_ble_scan.tick++;
    memset(alive, 0, sizeof(alive));

    if (fNIRS_lazy_init() == 0) {
        fnode_handler_t fh = fNIRS_handler_ref();

        if (fh)
            fnode_ev_ble_scan_pulse(fh);
    }

    if (fNIRS_desc(desc) == 0) {
        for (i = 0; i < FNODE_NID_MAX; i++) {
            alive[i] = desc[i].alive ? 1 : 0;
            if (alive[i])
                nodes++;
        }
    }

    if (nodes == g_ble_scan.prev_nodes && nodes > 0)
        g_ble_scan.stable++;
    else
        g_ble_scan.stable = 0;
    g_ble_scan.prev_nodes = nodes;

    if ((g_ble_scan.tick >= BLE_SCAN_MIN_TICKS &&
         g_ble_scan.stable >= BLE_SCAN_STABLE_REQ) ||
        g_ble_scan.tick >= BLE_SCAN_MAX_TICKS) {
        WLOGI("BLE scan: %u nodes online (ticks=%d)\r\n", nodes, g_ble_scan.tick);
        fnirs_ble_scan_async_finish(FNIRS_BLE_STATUS_OK, alive);
        return;
    }

    tv.tv_sec = 0;
    tv.tv_usec = (long)BLE_SCAN_TICK_MS * 1000L;
    if (!g_ble_scan.timer)
        return;
    evtimer_add(g_ble_scan.timer, &tv);
}

static void fnirs_ble_start_scan_async(int rsp_fd)
{
    struct timeval tv;
    fnode_handler_t fh;

    if (g_ble_scan.active)
        fnirs_ble_scan_async_disarm();

    g_ble_scan.active = 1;
    g_ble_scan.rsp_fd = rsp_fd;
    g_ble_scan.tick = 0;
    g_ble_scan.prev_nodes = 0;
    g_ble_scan.stable = 0;

    WLOGI("BLE scan async: start fd=%d\r\n", rsp_fd);

    (void)fNIRS_lazy_init();

    fh = fNIRS_handler_ref();
    if (fNIRS_ison() && fh) {
        (void)fnode_sample_off(fh);
        (void)fnode_reset(fh);
        fnode_ev_kick_idle(fh);
        fnode_ev_hub_service(fh);
    } else {
        (void)fNIRS_reset();
        fh = fNIRS_handler_ref();
        if (fh) {
            fnode_ev_kick_idle(fh);
            fnode_ev_hub_service(fh);
        }
    }

    fh = fNIRS_handler_ref();
    if (fh)
        fnode_ev_ble_scan_pulse(fh);

    if (!g_ble_scan.timer) {
        WLOGE("BLE scan async: timer not ready\r\n");
        fnirs_ble_scan_async_finish(FNIRS_BLE_STATUS_FAIL, NULL);
        return;
    }

    tv.tv_sec = 0;
    tv.tv_usec = (long)BLE_SCAN_TICK_MS * 1000L;
    evtimer_add(g_ble_scan.timer, &tv);
}

static void fnirs_ble_stream_async_disarm(void)
{
    if (g_ble_stream.timer)
        event_del(g_ble_stream.timer);
    g_ble_stream.active = 0;
    g_ble_stream.rsp_fd = -1;
}

static void fnirs_ble_stream_async_finish(int8_t rc)
{
    int fd = g_ble_stream.rsp_fd;

    fnirs_ble_stream_async_disarm();
    if (fd < 0)
        return;

    fnirs_ble_send_rsp(fd, FNIRS_BLE_CMD_STREAM_START,
                       rc == 0 ? FNIRS_BLE_STATUS_OK : FNIRS_BLE_STATUS_FAIL,
                       &rc, 1);
    close(fd);
}

static void fnirs_ble_stream_do_on(void)
{
    int8_t rc = (int8_t)fNIRS_on();

    if (rc != 0 && g_ble_stream.retry == 0) {
        WLOGW("BLE stream: fNIRS_on failed (%d), retry after reset\r\n", rc);
        g_ble_stream.retry = 1;
        (void)fNIRS_reset();
        g_ble_stream.phase = BLE_STREAM_PHASE_RETRY_WAIT;
        {
            struct timeval tv = {
                .tv_sec = 0,
                .tv_usec = (long)BLE_STREAM_RETRY_WAIT_MS * 1000L,
            };

            evtimer_add(g_ble_stream.timer, &tv);
        }
        return;
    }

    if (rc == 0)
        WLOGI("BLE stream: sampling on (ison=%u)\r\n", fNIRS_ison());
    else
        WLOGW("BLE stream: sampling still off (%d)\r\n", rc);

    fnirs_stream_arm_writer();
    fnirs_ble_stream_async_finish(rc);
}

static void fnirs_ble_stream_async_tick(evutil_socket_t fd, short events, void *arg)
{
    (void)fd;
    (void)events;
    (void)arg;

    if (!g_ble_stream.active)
        return;

    switch (g_ble_stream.phase) {
    case BLE_STREAM_PHASE_OFF_WAIT:
        g_ble_stream.phase = BLE_STREAM_PHASE_ON;
        fnirs_ble_stream_do_on();
        break;
    case BLE_STREAM_PHASE_RETRY_WAIT:
        g_ble_stream.phase = BLE_STREAM_PHASE_ON;
        fnirs_ble_stream_do_on();
        break;
    default:
        break;
    }
}

static void fnirs_ble_start_stream_async(int rsp_fd)
{
    struct timeval tv;
    int32_t log_rc;

    if (g_ble_stream.active)
        fnirs_ble_stream_async_disarm();

    g_ble_stream.active = 1;
    g_ble_stream.rsp_fd = rsp_fd;
    g_ble_stream.retry = 0;

    WLOGI("BLE ipc: STREAM_START (async)\r\n");
    g_ble_stream_want = 1;
    fnirs_stream_ctl_local_start();
    log_rc = fdatalog_start();
    if (log_rc != 0) {
        WLOGE("BLE stream: recording start failed (%d)\r\n", log_rc);
        g_ble_stream_want = 0;
        fnirs_stream_ctl_local_stop();
        fnirs_ble_stream_async_finish(-1);
        return;
    }

    if (fNIRS_ison()) {
        WLOGI("BLE stream: reset sampling before start\r\n");
        fNIRS_off();
        g_ble_stream.phase = BLE_STREAM_PHASE_OFF_WAIT;
        tv.tv_sec = 0;
        tv.tv_usec = (long)BLE_STREAM_OFF_WAIT_MS * 1000L;
        evtimer_add(g_ble_stream.timer, &tv);
        return;
    }

    g_ble_stream.phase = BLE_STREAM_PHASE_ON;
    fnirs_ble_stream_do_on();
}

void fnirs_ble_ev_init(struct event_base *base)
{
    g_ble_ev_base = base;
    memset(&g_ble_scan, 0, sizeof(g_ble_scan));
    memset(&g_ble_stream, 0, sizeof(g_ble_stream));
    g_ble_scan.rsp_fd = -1;
    g_ble_stream.rsp_fd = -1;

    g_ble_scan.timer = evtimer_new(base, fnirs_ble_scan_async_tick, NULL);
    g_ble_stream.timer = evtimer_new(base, fnirs_ble_stream_async_tick, NULL);

    if (!g_ble_scan.timer || !g_ble_stream.timer)
        WLOGE("fnirs_ble_ev_init: timer create failed\r\n");
}

void fnirs_ble_ev_shutdown(void)
{
    fnirs_ble_scan_async_disarm();
    fnirs_ble_stream_async_disarm();

    if (g_ble_scan.timer) {
        event_free(g_ble_scan.timer);
        g_ble_scan.timer = NULL;
    }

    if (g_ble_stream.timer) {
        event_free(g_ble_stream.timer);
        g_ble_stream.timer = NULL;
    }
}

void fnirs_ble_ev_process_job(uint8_t cmd, const uint8_t *payload, uint16_t len,
                              int rsp_fd)
{
    WLOGI("fnirs_ble_ev_process_job: cmd=0x%02x fd=%d\r\n", cmd, rsp_fd);

    switch (cmd) {
    case FNIRS_BLE_CMD_SCAN:
        fnirs_ble_handle_scan_ev_blocking(rsp_fd);
        break;
    case FNIRS_BLE_CMD_STREAM_START:
        fnirs_ble_start_stream_async(rsp_fd);
        break;
    case FNIRS_BLE_CMD_SAMPLE_ON:
        fnirs_ble_handle_sample_on(rsp_fd);
        close(rsp_fd);
        break;
    case FNIRS_BLE_CMD_SAMPLE_OFF:
        fnirs_ble_handle_sample_off(rsp_fd);
        close(rsp_fd);
        break;
    case FNIRS_BLE_CMD_SET_GAIN:
        fnirs_ble_handle_gain(rsp_fd, payload, len);
        close(rsp_fd);
        break;
    case FNIRS_BLE_CMD_SET_LED_ARRAY:
        fnirs_ble_handle_led_array(rsp_fd, payload, len);
        close(rsp_fd);
        break;
    case FNIRS_BLE_CMD_SET_STREAM_CH:
        fnirs_ble_handle_stream_ch(rsp_fd, payload, len);
        close(rsp_fd);
        break;
    case FNIRS_BLE_CMD_STREAM_STOP:
        fnirs_ble_handle_stream_stop(rsp_fd);
        close(rsp_fd);
        break;
    case FNIRS_BLE_CMD_HANGZHOU_PATH:
        fnirs_ble_handle_hangzhou_path(rsp_fd);
        close(rsp_fd);
        break;
    case FNIRS_BLE_CMD_RECORD_STAT:
        fnirs_ble_handle_record_stat(rsp_fd);
        close(rsp_fd);
        break;
    default:
        fnirs_ble_send_rsp(rsp_fd, cmd, FNIRS_BLE_STATUS_FAIL, NULL, 0);
        close(rsp_fd);
        break;
    }
}

void fnirs_ble_ev_cancel_async(void)
{
    if (g_ble_scan.active && g_ble_scan.rsp_fd >= 0) {
        int fd = g_ble_scan.rsp_fd;

        fnirs_ble_scan_async_disarm();
        fnirs_ble_send_rsp(fd, FNIRS_BLE_CMD_SCAN, FNIRS_BLE_STATUS_FAIL,
                           NULL, 0);
        shutdown(fd, SHUT_WR);
        close(fd);
    } else {
        fnirs_ble_scan_async_disarm();
    }

    if (g_ble_stream.active && g_ble_stream.rsp_fd >= 0) {
        int fd = g_ble_stream.rsp_fd;

        fnirs_ble_stream_async_disarm();
        fnirs_ble_send_rsp(fd, FNIRS_BLE_CMD_STREAM_START,
                           FNIRS_BLE_STATUS_FAIL, NULL, 0);
        close(fd);
    } else {
        fnirs_ble_stream_async_disarm();
    }
}
#endif /* FNIRS_EMBEDDED && FNIRS_EV_IO */

static int32_t fnirs_ble_handle_scan(int fd)
{
#if defined(FNIRS_EMBEDDED) && defined(FNIRS_EV_IO)
    fnirs_ble_start_scan_async(fd);
    return 0;
#else
    fnode_desc_t desc[FNODE_NID_MAX];
    uint8_t alive[FNODE_NID_MAX];
    uint32_t i;
    int try;
    uint8_t nodes = 0;
    uint8_t prev_nodes = 0;
    int stable = 0;
    const int poll_us = 100000;
    const int max_tries = 50;
    const int min_tries = 40;

    memset(alive, 0, sizeof(alive));

    /*
     * Always reset on explicit BLE scan so desc is fresh and every node
     * (including late responders like nid 6) gets a chance to answer.
     */
    (void)fNIRS_reset();

    for (try = 0; try < max_tries; try++) {
        usleep((useconds_t)poll_us);

        if (fNIRS_desc(desc) != 0)
            continue;

        nodes = 0;
        for (i = 0; i < FNODE_NID_MAX; i++) {
            alive[i] = desc[i].alive ? 1 : 0;
            if (alive[i])
                nodes++;
        }

        if (nodes == prev_nodes && nodes > 0)
            stable++;
        else
            stable = 0;
        prev_nodes = nodes;

        if (try >= min_tries && stable >= 2)
            break;
        if (try >= max_tries - 1)
            break;
    }

    WLOGI("BLE scan: %u nodes online (tries=%d)\r\n", nodes, try);

    return fnirs_ble_send_rsp(fd, FNIRS_BLE_CMD_SCAN, FNIRS_BLE_STATUS_OK,
                              alive, FNODE_NID_MAX);
#endif
}

static int32_t fnirs_ble_handle_sample_on(int fd)
{
    int8_t rc = 0;
    int32_t log_rc = 0;

    if (!fNIRS_ison()) {
        log_rc = fdatalog_start();
        if (log_rc == 0)
            rc = (int8_t)fNIRS_on();
        else
            rc = -1;
    }

    return fnirs_ble_send_rsp(fd, FNIRS_BLE_CMD_SAMPLE_ON,
                            rc == 0 && log_rc == 0 ? FNIRS_BLE_STATUS_OK :
                            FNIRS_BLE_STATUS_FAIL, &rc, 1);
}

static int32_t fnirs_ble_handle_sample_off(int fd)
{
    int8_t rc = 0;
    int32_t log_rc;

    if (fNIRS_ison())
        rc = (int8_t)fNIRS_off();

#ifdef FNIRS_EV_IO
    /*
     * Keep ATT responsive after SAMPLE_OFF. The writer thread drains and
     * seals the file while LIVE FILE continues transferring its remaining
     * bytes; FINAL is emitted only after fdatalog reports FINALIZED.
     */
    log_rc = fdatalog_stop_async();
#else
    log_rc = fdatalog_stop();
#endif
    g_ble_stream_want = 0;
    WLOGI("BLE sample off: sample_rc=%d finalize_rc=%d\r\n",
          rc, log_rc);

    return fnirs_ble_send_rsp(fd, FNIRS_BLE_CMD_SAMPLE_OFF,
                            rc == 0 && log_rc == 0 ?
                            FNIRS_BLE_STATUS_OK : FNIRS_BLE_STATUS_FAIL,
                            &rc, 1);
}

static int32_t fnirs_ble_handle_gain(int fd, const uint8_t *payload, uint16_t len)
{
    int8_t rc = -1;

    if (!payload || len < 1)
        return fnirs_ble_send_rsp(fd, FNIRS_BLE_CMD_SET_GAIN,
                                  FNIRS_BLE_STATUS_FAIL, NULL, 0);

    rc = (int8_t)fNIRS_s_gain(payload[0]);
    return fnirs_ble_send_rsp(fd, FNIRS_BLE_CMD_SET_GAIN,
                            rc == 0 ? FNIRS_BLE_STATUS_OK :
                            FNIRS_BLE_STATUS_FAIL, &rc, 1);
}

static int32_t fnirs_ble_handle_led_array(int fd, const uint8_t *payload, uint16_t len)
{
    int32_t rc;

    if (!payload || len < 4 || (len % 4) != 0)
        return fnirs_ble_send_rsp(fd, FNIRS_BLE_CMD_SET_LED_ARRAY,
                                  FNIRS_BLE_STATUS_FAIL, NULL, 0);

    rc = fnode_set_array((uint8_t *)payload, len);
    return fnirs_ble_send_rsp(fd, FNIRS_BLE_CMD_SET_LED_ARRAY,
                            rc >= 0 ? FNIRS_BLE_STATUS_OK :
                            FNIRS_BLE_STATUS_FAIL, (uint8_t *)&rc, 1);
}

static int32_t fnirs_ble_handle_stream_ch(int fd, const uint8_t *payload, uint16_t len)
{
    fnirs_ble_stream_cfg_t cfg;
    uint8_t count;
    uint16_t i;

    if (!payload || len < 1)
        return fnirs_ble_send_rsp(fd, FNIRS_BLE_CMD_SET_STREAM_CH,
                                  FNIRS_BLE_STATUS_FAIL, NULL, 0);

    count = payload[0];
    if (count > FNIRS_BLE_MAX_STREAM_CH)
        return fnirs_ble_send_rsp(fd, FNIRS_BLE_CMD_SET_STREAM_CH,
                                  FNIRS_BLE_STATUS_FAIL, NULL, 0);

    memset(&cfg, 0, sizeof(cfg));
    cfg.count = count;

    if (len >= (uint16_t)(1 + count * 4)) {
        for (i = 0; i < count; i++) {
            cfg.ch[i].src_node = payload[1 + i * 4];
            cfg.ch[i].led_id = payload[1 + i * 4 + 1];
            cfg.ch[i].det_node = payload[1 + i * 4 + 2];
            cfg.ch[i].det_id = payload[1 + i * 4 + 3];
            if (cfg.ch[i].det_id < 1 || cfg.ch[i].det_id > FNODE_DETID_MAX)
                cfg.ch[i].det_id = 1;
        }
    } else if (len >= (uint16_t)(1 + count * 3)) {
        for (i = 0; i < count; i++) {
            cfg.ch[i].src_node = payload[1 + i * 3];
            cfg.ch[i].led_id = payload[1 + i * 3 + 1];
            cfg.ch[i].det_node = payload[1 + i * 3 + 2];
            cfg.ch[i].det_id = 1;
        }
    } else {
        return fnirs_ble_send_rsp(fd, FNIRS_BLE_CMD_SET_STREAM_CH,
                                  FNIRS_BLE_STATUS_FAIL, NULL, 0);
    }

    pthread_mutex_lock(&g_stream_cfg_mtx);
    g_stream_cfg = cfg;
    pthread_mutex_unlock(&g_stream_cfg_mtx);

    return fnirs_ble_send_rsp(fd, FNIRS_BLE_CMD_SET_STREAM_CH,
                            FNIRS_BLE_STATUS_OK, &count, 1);
}

static int32_t fnirs_ble_handle_stream_start(int fd)
{
#if defined(FNIRS_EMBEDDED) && defined(FNIRS_EV_IO)
    fnirs_ble_start_stream_async(fd);
    return 0;
#else
    int8_t rc = 0;
    int32_t log_rc;

    WLOGI("BLE ipc: STREAM_START\r\n");
    g_ble_stream_want = 1;
    fnirs_stream_ctl_local_start();

    log_rc = fdatalog_start();
    if (log_rc != 0) {
        rc = -1;
        g_ble_stream_want = 0;
        fnirs_stream_ctl_local_stop();
        return fnirs_ble_send_rsp(fd, FNIRS_BLE_CMD_STREAM_START,
                                  FNIRS_BLE_STATUS_FAIL, &rc, 1);
    }
    if (!fNIRS_ison()) {
        rc = (int8_t)fNIRS_on();
        if (rc != 0) {
            WLOGW("BLE stream: fNIRS_on failed (%d), retry after reset\r\n", rc);
            fNIRS_reset();
            usleep(500000);
            rc = (int8_t)fNIRS_on();
        }
    }

    if (rc == 0)
        WLOGI("BLE stream: sampling on (ison=%u)\r\n", fNIRS_ison());
    else
        WLOGW("BLE stream: sampling still off (%d)\r\n", rc);

    return fnirs_ble_send_rsp(fd, FNIRS_BLE_CMD_STREAM_START,
                            FNIRS_BLE_STATUS_OK, &rc, 1);
#endif
}

static int32_t fnirs_ble_handle_stream_stop(int fd)
{
    WLOGI("BLE ipc: STREAM_STOP\r\n");
    g_ble_stream_want = 0;
    fnirs_stream_ctl_local_stop();
#ifdef FNIRS_EV_IO
    /*
     * ev: App resetBoardProtocol sends ABORT before START; my-server only
     * forwards STREAM_STOP when a stream was active.  Avoid fNIRS_off() here
     * so ABORT/STOP does not reset the CAN state machine between STARTs.
     */
#else
    if (fNIRS_ison())
        fNIRS_off();
#endif
    return fnirs_ble_send_rsp(fd, FNIRS_BLE_CMD_STREAM_STOP,
                            FNIRS_BLE_STATUS_OK, NULL, 0);
}

static int32_t fnirs_ble_handle_hangzhou_path(int fd)
{
    char path[128];

    if (fdatalog_get_latest_path(path, sizeof(path)) != 0)
        return fnirs_ble_send_rsp(fd, FNIRS_BLE_CMD_HANGZHOU_PATH,
                                  FNIRS_BLE_STATUS_FAIL, NULL, 0);

    return fnirs_ble_send_rsp(fd, FNIRS_BLE_CMD_HANGZHOU_PATH,
                            FNIRS_BLE_STATUS_OK, path,
                            (uint16_t)(strlen(path) + 1));
}

static int32_t fnirs_ble_handle_record_stat(int fd)
{
    uint32_t bytes = 0;
    uint32_t frames = 0;
    uint8_t payload[8];

    if (fdatalog_get_recording_stat(&bytes, &frames) != 0)
        return fnirs_ble_send_rsp(fd, FNIRS_BLE_CMD_RECORD_STAT,
                                  FNIRS_BLE_STATUS_FAIL, NULL, 0);

    payload[0] = (uint8_t)(bytes);
    payload[1] = (uint8_t)(bytes >> 8);
    payload[2] = (uint8_t)(bytes >> 16);
    payload[3] = (uint8_t)(bytes >> 24);
    payload[4] = (uint8_t)(frames);
    payload[5] = (uint8_t)(frames >> 8);
    payload[6] = (uint8_t)(frames >> 16);
    payload[7] = (uint8_t)(frames >> 24);
    return fnirs_ble_send_rsp(fd, FNIRS_BLE_CMD_RECORD_STAT,
                            FNIRS_BLE_STATUS_OK, payload, 8);
}

void fnirs_ble_handle_client(int cfd)
{
    uint8_t hdr[5];
    uint8_t cmd;
    uint16_t len;
    uint8_t payload[512];
    ssize_t n;

    n = read(cfd, hdr, 5);
    if (n != 5 || hdr[0] != FNIRS_BLE_REQ_MAGIC0 ||
        hdr[1] != FNIRS_BLE_REQ_MAGIC1)
        return;

    cmd = hdr[2];
    len = (uint16_t)hdr[3] | ((uint16_t)hdr[4] << 8);
    if (len > sizeof(payload))
        return;

    if (len > 0) {
        n = read(cfd, payload, len);
        if (n != (ssize_t)len)
            return;
    }

    switch (cmd) {
    case FNIRS_BLE_CMD_SCAN:
        fnirs_ble_handle_scan(cfd);
        break;
    case FNIRS_BLE_CMD_SAMPLE_ON:
        fnirs_ble_handle_sample_on(cfd);
        break;
    case FNIRS_BLE_CMD_SAMPLE_OFF:
        fnirs_ble_handle_sample_off(cfd);
        break;
    case FNIRS_BLE_CMD_SET_GAIN:
        fnirs_ble_handle_gain(cfd, payload, len);
        break;
    case FNIRS_BLE_CMD_SET_LED_ARRAY:
        fnirs_ble_handle_led_array(cfd, payload, len);
        break;
    case FNIRS_BLE_CMD_SET_STREAM_CH:
        fnirs_ble_handle_stream_ch(cfd, payload, len);
        break;
    case FNIRS_BLE_CMD_STREAM_START:
        fnirs_ble_handle_stream_start(cfd);
        break;
    case FNIRS_BLE_CMD_STREAM_STOP:
        fnirs_ble_handle_stream_stop(cfd);
        break;
    case FNIRS_BLE_CMD_HANGZHOU_PATH:
        fnirs_ble_handle_hangzhou_path(cfd);
        break;
    case FNIRS_BLE_CMD_RECORD_STAT:
        fnirs_ble_handle_record_stat(cfd);
        break;
    default:
        fnirs_ble_send_rsp(cfd, cmd, FNIRS_BLE_STATUS_FAIL, NULL, 0);
        break;
    }
}

int32_t fnirs_ble_dispatch_request(uint8_t cmd, const uint8_t *req, uint16_t req_len,
                                  uint8_t *status_out, uint8_t *rsp, uint16_t rsp_max,
                                  uint16_t *rsp_len_out)
{
    int pair[2];
    uint8_t hdr[5];
    uint8_t rhdr[6];
    ssize_t n;
    uint16_t rsp_len = 0;

    if (status_out)
        *status_out = 0xFF;
    if (rsp_len_out)
        *rsp_len_out = 0;

#if defined(FNIRS_EMBEDDED) && defined(FNIRS_EV_IO)
    WLOGI("fnirs_ble_dispatch_request cmd=0x%02x\r\n", cmd);
    (void)fNIRS_lazy_init();
    return ev_ble_async_dispatch(cmd, req, req_len, status_out, rsp, rsp_max,
                                 rsp_len_out);
#else
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)
        return -1;

    hdr[0] = FNIRS_BLE_REQ_MAGIC0;
    hdr[1] = FNIRS_BLE_REQ_MAGIC1;
    hdr[2] = cmd;
    hdr[3] = (uint8_t)(req_len & 0xff);
    hdr[4] = (uint8_t)((req_len >> 8) & 0xff);

    if (write(pair[1], hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr) ||
        (req_len > 0 && req &&
         write(pair[1], req, req_len) != (ssize_t)req_len)) {
        close(pair[0]);
        close(pair[1]);
        return -1;
    }

    /*
     * socketpair: write on pair[1] -> readable on pair[0] (request path).
     * handle_client reads req on pair[0] and writes rsp on pair[0], which
     * lands on pair[1].  Keep pair[1] open until the rsp is read — closing
     * it early makes the peer write fail with EPIPE.
     */
    shutdown(pair[1], SHUT_WR);

    fnirs_ble_handle_client(pair[0]);

    n = read(pair[1], rhdr, sizeof(rhdr));
    if (n != (ssize_t)sizeof(rhdr) || rhdr[0] != FNIRS_BLE_RSP_MAGIC0 ||
        rhdr[1] != FNIRS_BLE_RSP_MAGIC1) {
        close(pair[0]);
        close(pair[1]);
        return -1;
    }

    if (status_out)
        *status_out = rhdr[3];
    rsp_len = (uint16_t)rhdr[4] | ((uint16_t)rhdr[5] << 8);
    if (rsp_len > 0 && rsp && rsp_max > 0) {
        if (rsp_len > rsp_max)
            rsp_len = rsp_max;
        n = read(pair[1], rsp, rsp_len);
        if (n != (ssize_t)rsp_len) {
            close(pair[0]);
            close(pair[1]);
            return -1;
        }
        if (rsp_len_out)
            *rsp_len_out = rsp_len;
    }

    close(pair[0]);
    close(pair[1]);
    return 0;
#endif
}

#ifndef FNIRS_EV_IO
static void *fnirs_ble_ipc_thread(void *arg)
{
    int sfd, cfd;
    struct sockaddr_un addr;

    (void)arg;

    unlink(FNIRS_BLE_SOCK_PATH);
    sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd < 0)
        return NULL;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, FNIRS_BLE_SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(sfd, 4) != 0) {
        close(sfd);
        return NULL;
    }

    WLOGI("fnirs_ble_ipc listening on %s\r\n", FNIRS_BLE_SOCK_PATH);

    while (g_ble_ipc_run) {
        cfd = accept(sfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR)
                continue;
            usleep(100000);
            continue;
        }
        fnirs_ble_handle_client(cfd);
        close(cfd);
    }

    close(sfd);
    unlink(FNIRS_BLE_SOCK_PATH);
    return NULL;
}
#endif

int32_t fnirs_ble_ipc_init(void)
{
    fnirs_ble_default_stream_cfg(&g_stream_cfg);

#ifdef FNIRS_EMBEDDED
    g_ble_ipc_run = 0;
    WLOGI("fnirs_ble_ipc: embedded mode (in-process dispatch)\r\n");
    return 0;
#elif defined(FNIRS_EV_IO)
    g_ble_ipc_run = 0;
    WLOGI("fnirs_ble_ipc: ev mode (socket served by ev_ble_ipc)\r\n");
    return 0;
#else
    g_ble_ipc_run = 1;

    if (pthread_create(&g_ble_ipc_tid, NULL, fnirs_ble_ipc_thread, NULL) != 0)
        return -1;

    return 0;
#endif
}

void fnirs_ble_ipc_exit(void)
{
#if defined(FNIRS_EMBEDDED) || defined(FNIRS_EV_IO)
    return;
#else
    g_ble_ipc_run = 0;
    pthread_join(g_ble_ipc_tid, NULL);
#endif
}

void fnirs_ble_ipc_get_stream_cfg(fnirs_ble_stream_cfg_t *cfg)
{
    if (!cfg)
        return;

    pthread_mutex_lock(&g_stream_cfg_mtx);
    *cfg = g_stream_cfg;
    pthread_mutex_unlock(&g_stream_cfg_mtx);
}

int32_t fnirs_ble_ipc_stream_active(void)
{
    return g_ble_stream_want;
}
