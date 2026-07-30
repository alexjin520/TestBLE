#include "ev_module.h"
#include "ev_app.h"
#include "ev_timer.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <event2/event.h>

#include "ble_service.h"
#include "ev_can.h"
#include "fNIRS.h"
#include "fdatalog.h"
#include "fnode.h"
#include "led.h"
#include "my_interface.h"

extern void ev_uart_retry_if_needed(void);

#define EV_TICK_SEC 1
/* Match legacy fdatalog_task usleep(50ms) poll interval. */
#define EV_FDATALOG_POLL_MS 50

typedef struct {
    ev_app_t *app;
    struct event *tick_ev;
    struct event *datalog_poll_ev;
    int timerfd;
    int datalog_poll_timerfd;
    uint32_t tick_count;
} ev_timer_ctx_t;

static ev_timer_ctx_t *g_timer_ctx;
static pthread_mutex_t g_tick_log_mtx = PTHREAD_MUTEX_INITIALIZER;

static void ev_tick_log_line(const char *msg, int lock)
{
    char line[160];
    int fd;
    int n;
    time_t now;

    if (!msg)
        return;

    now = time(NULL);
    n = snprintf(line, sizeof(line), "%ld pid=%d %s\n",
                 (long)now, (int)getpid(), msg);
    if (n <= 0)
        return;

    if (lock)
        pthread_mutex_lock(&g_tick_log_mtx);

    fd = open("/tmp/ev_tick.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        (void)write(fd, line, (size_t)n);
        close(fd);
    }

    if (lock)
        pthread_mutex_unlock(&g_tick_log_mtx);
}

void ev_timer_note(const char *msg)
{
    ev_tick_log_line(msg, 1);
}

void ev_timer_note_unsafe(const char *msg)
{
    ev_tick_log_line(msg, 0);
}

static void ev_tick_touch_debug(uint32_t count)
{
    char line[24];
    int fd;
    int n;

    n = snprintf(line, sizeof(line), "%u\n", count);
    if (n <= 0)
        return;

    fd = open("/tmp/ev_tick.count", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        (void)write(fd, line, (size_t)n);
        close(fd);
    }
}

static int ev_tick_timerfd_open(void)
{
    struct itimerspec its;
    int fd;

    fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0)
        return -1;

    memset(&its, 0, sizeof(its));
    its.it_value.tv_sec = EV_TICK_SEC;
    its.it_interval.tv_sec = EV_TICK_SEC;
    if (timerfd_settime(fd, 0, &its, NULL) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static int ev_datalog_poll_timerfd_open(void)
{
    struct itimerspec its;
    int fd;

    fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0)
        return -1;

    memset(&its, 0, sizeof(its));
    its.it_value.tv_nsec = (long)EV_FDATALOG_POLL_MS * 1000000L;
    its.it_interval.tv_nsec = (long)EV_FDATALOG_POLL_MS * 1000000L;
    if (timerfd_settime(fd, 0, &its, NULL) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static void ev_datalog_poll_cb(evutil_socket_t fd, short events, void *arg)
{
    (void)arg;
    uint64_t expirations;

    (void)events;

    while (read((int)fd, &expirations, sizeof(expirations)) ==
           (ssize_t)sizeof(expirations)) {
        fdatalog_ev_poll();
    }
}

static void ev_timer_start_datalog_poll(ev_timer_ctx_t *ctx)
{
    if (!ctx || ctx->datalog_poll_ev)
        return;

    ctx->datalog_poll_timerfd = ev_datalog_poll_timerfd_open();
    if (ctx->datalog_poll_timerfd < 0)
        return;

    ctx->datalog_poll_ev = event_new(ctx->app->base, ctx->datalog_poll_timerfd,
                                     EV_READ | EV_PERSIST, ev_datalog_poll_cb, ctx);
    if (!ctx->datalog_poll_ev) {
        close(ctx->datalog_poll_timerfd);
        ctx->datalog_poll_timerfd = -1;
        return;
    }

    if (event_add(ctx->datalog_poll_ev, NULL) != 0) {
        event_free(ctx->datalog_poll_ev);
        ctx->datalog_poll_ev = NULL;
        close(ctx->datalog_poll_timerfd);
        ctx->datalog_poll_timerfd = -1;
        return;
    }

    WLOGI("fdatalog poll armed (%dms)\r\n", EV_FDATALOG_POLL_MS);
}

static void ev_tick_cb(evutil_socket_t fd, short events, void *arg)
{
    ev_timer_ctx_t *ctx = arg;
    uint64_t expirations;

    (void)events;

    if (!ctx)
        return;

    while (read((int)fd, &expirations, sizeof(expirations)) == (ssize_t)sizeof(expirations)) {
        ctx->tick_count++;
        ev_tick_touch_debug(ctx->tick_count);

        if (ctx->tick_count == 1) {
            char note[64];

            snprintf(note, sizeof(note), "first timer cb gettid=%ld",
                     (long)syscall(SYS_gettid));
            ev_timer_note(note);
        } else if ((ctx->tick_count % 60) == 0) {
            char note[48];

            snprintf(note, sizeof(note), "tick %u", ctx->tick_count);
            ev_timer_note(note);
        }

#ifdef FNIRS_EV_IO
        ev_can_try_attach();

        {
            fnode_handler_t fh = fNIRS_handler_ref();

            if (fh)
                fnode_ev_led_scan_tick(fh);
        }
#endif

        /* Match legacy main.c sleep(2) + led_system(1). */
        if ((ctx->tick_count % 2) == 0)
            led_system(1);

#if !defined(FNIRS_EMBEDDED)
        if ((ctx->tick_count % 2) == 0)
            ble_service_tick();
#endif

        if ((ctx->tick_count % 30) == 0)
            ev_uart_retry_if_needed();
    }
}

static int ev_timer_init(ev_app_t *app, ev_module_t *mod)
{
    ev_timer_ctx_t *ctx = calloc(1, sizeof(*ctx));

    if (!ctx)
        return -1;

    ctx->app = app;
    ctx->timerfd = -1;
    ctx->datalog_poll_timerfd = -1;
    mod->ctx = ctx;
    g_timer_ctx = ctx;

    ctx->timerfd = ev_tick_timerfd_open();
    if (ctx->timerfd < 0) {
        ev_timer_note("timerfd_create failed");
        free(ctx);
        mod->ctx = NULL;
        g_timer_ctx = NULL;
        return -1;
    }

    ctx->tick_ev = event_new(app->base, ctx->timerfd,
                             EV_READ | EV_PERSIST, ev_tick_cb, ctx);
    if (!ctx->tick_ev) {
        close(ctx->timerfd);
        free(ctx);
        mod->ctx = NULL;
        g_timer_ctx = NULL;
        return -1;
    }

    ev_tick_touch_debug(0);
    ev_timer_note("timerfd created (arm deferred)");

    if (event_add(ctx->tick_ev, NULL) != 0) {
        ev_timer_note("timerfd event_add failed in init");
        WLOGE("timerfd event_add failed\r\n");
        return -1;
    }

    ev_timer_note("timerfd armed in init");
    WLOGI("timerfd armed (%ds, EV_READ)\r\n", EV_TICK_SEC);

    ev_timer_start_datalog_poll(ctx);

    return 0;
}

void ev_timer_arm(void)
{
    ev_timer_ctx_t *ctx = g_timer_ctx;

    if (!ctx || !ctx->tick_ev || !ctx->app || !ctx->app->base)
        return;

    /* Already armed during ev_timer_init. */
    ev_timer_note("timerfd arm skipped (already active)");
}

static void ev_timer_shutdown(ev_app_t *app, ev_module_t *mod)
{
    ev_timer_ctx_t *ctx = mod->ctx;

    (void)app;

    if (!ctx)
        return;

    if (ctx->tick_ev) {
        event_free(ctx->tick_ev);
        ctx->tick_ev = NULL;
    }

    if (ctx->datalog_poll_ev) {
        event_del(ctx->datalog_poll_ev);
        event_free(ctx->datalog_poll_ev);
        ctx->datalog_poll_ev = NULL;
    }

    if (ctx->datalog_poll_timerfd >= 0) {
        close(ctx->datalog_poll_timerfd);
        ctx->datalog_poll_timerfd = -1;
    }

    if (ctx->timerfd >= 0) {
        close(ctx->timerfd);
        ctx->timerfd = -1;
    }

    if (g_timer_ctx == ctx)
        g_timer_ctx = NULL;

    free(ctx);
    mod->ctx = NULL;
}

ev_module_t ev_timer_module = EV_MODULE_REGISTER("timer", ev_timer_init, ev_timer_shutdown);
