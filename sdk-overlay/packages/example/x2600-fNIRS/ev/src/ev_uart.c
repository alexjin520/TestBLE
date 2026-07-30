#include "ev_module.h"
#include "ev_app.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/timerfd.h>

#include <event2/event.h>

#include "fusb.h"
#include "my_interface.h"

#define EV_UART_DEV         "/dev/ttyS1"
#define EV_UART_BAUD        2000000
/* Heartbeat / SPDATA / voltage / factory reporting (not UART RX). */
#define EV_FUSB_HOUSEKEEP_MS  20

typedef struct {
    ev_app_t *app;
    int fd;
    struct event *read_ev;
    struct event *housekeeping_ev;
    int housekeeping_timerfd;
} ev_uart_ctx_t;

static ev_uart_ctx_t *g_uart_ctx;

static void ev_uart_disarm_read(ev_uart_ctx_t *ctx)
{
    if (!ctx || !ctx->read_ev)
        return;

    event_del(ctx->read_ev);
    event_free(ctx->read_ev);
    ctx->read_ev = NULL;
}

static void ev_uart_read_cb(evutil_socket_t fd, short events, void *arg)
{
    ev_uart_ctx_t *ctx = arg;

    (void)fd;
    (void)events;

    if (!ctx || ctx->fd < 0)
        return;

    fusb_ev_drain_read();
}

static int ev_uart_arm_read(ev_uart_ctx_t *ctx)
{
    if (!ctx || ctx->fd < 0 || !ctx->app || !ctx->app->base)
        return -1;

    if (ctx->read_ev)
        return 0;

    ctx->read_ev = event_new(ctx->app->base, ctx->fd, EV_READ | EV_PERSIST,
                             ev_uart_read_cb, ctx);
    if (!ctx->read_ev)
        return -1;

    if (event_add(ctx->read_ev, NULL) != 0) {
        event_free(ctx->read_ev);
        ctx->read_ev = NULL;
        return -1;
    }

    return 0;
}

static void ev_uart_close_locked(ev_uart_ctx_t *ctx)
{
    if (!ctx)
        return;

    ev_uart_disarm_read(ctx);

    if (ctx->fd >= 0) {
        fusb_ev_detach_fd();
        wos_uart_close(ctx->fd);
        ctx->fd = -1;
    }
}

static void ev_uart_try_open(ev_uart_ctx_t *ctx)
{
    int flags;

    if (!ctx || ctx->fd >= 0)
        return;

    ctx->fd = wos_uart_fopen(EV_UART_DEV, EV_UART_BAUD, 8, 'N', 1);
    if (ctx->fd < 0) {
        WLOGW("uart open %s failed\r\n", EV_UART_DEV);
        return;
    }

    flags = fcntl(ctx->fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(ctx->fd, F_SETFL, flags | O_NONBLOCK);

    fusb_ev_attach_fd(ctx->fd);

    if (ev_uart_arm_read(ctx) != 0) {
        WLOGE("uart EV_READ setup failed\r\n");
        ev_uart_close_locked(ctx);
        return;
    }

    WLOGI("uart open (EV_READ): %s @ %d fd=%d\r\n",
          EV_UART_DEV, EV_UART_BAUD, ctx->fd);
}

void ev_uart_retry_if_needed(void)
{
    if (!g_uart_ctx)
        return;

    ev_uart_try_open(g_uart_ctx);
}

static int ev_fusb_housekeeping_timerfd_open(void)
{
    struct itimerspec its;
    int fd;

    fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0)
        return -1;

    memset(&its, 0, sizeof(its));
    its.it_value.tv_nsec = (long)EV_FUSB_HOUSEKEEP_MS * 1000000L;
    its.it_interval.tv_nsec = (long)EV_FUSB_HOUSEKEEP_MS * 1000000L;
    if (timerfd_settime(fd, 0, &its, NULL) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static void ev_fusb_housekeeping_cb(evutil_socket_t fd, short events, void *arg)
{
    ev_uart_ctx_t *ctx = arg;
    uint64_t expirations;

    (void)events;

    if (!ctx)
        return;

    while (read((int)fd, &expirations, sizeof(expirations)) ==
           (ssize_t)sizeof(expirations)) {
        fusb_ev_poll();
    }
}

static int ev_uart_init(ev_app_t *app, ev_module_t *mod)
{
    ev_uart_ctx_t *ctx = calloc(1, sizeof(*ctx));

    if (!ctx)
        return -1;

    ctx->app = app;
    ctx->fd = -1;
    ctx->housekeeping_timerfd = -1;
    mod->ctx = ctx;
    g_uart_ctx = ctx;

    ev_uart_try_open(ctx);

    ctx->housekeeping_timerfd = ev_fusb_housekeeping_timerfd_open();
    if (ctx->housekeeping_timerfd < 0) {
        WLOGE("fusb housekeeping timerfd failed\r\n");
        ev_uart_close_locked(ctx);
        free(ctx);
        mod->ctx = NULL;
        g_uart_ctx = NULL;
        return -1;
    }

    ctx->housekeeping_ev = event_new(app->base, ctx->housekeeping_timerfd,
                                     EV_READ | EV_PERSIST,
                                     ev_fusb_housekeeping_cb, ctx);
    if (!ctx->housekeeping_ev) {
        close(ctx->housekeeping_timerfd);
        ev_uart_close_locked(ctx);
        free(ctx);
        mod->ctx = NULL;
        g_uart_ctx = NULL;
        return -1;
    }

    if (event_add(ctx->housekeeping_ev, NULL) != 0) {
        event_free(ctx->housekeeping_ev);
        close(ctx->housekeeping_timerfd);
        ev_uart_close_locked(ctx);
        free(ctx);
        mod->ctx = NULL;
        g_uart_ctx = NULL;
        return -1;
    }

    WLOGI("fusb housekeeping timer armed (%dms)\r\n", EV_FUSB_HOUSEKEEP_MS);
    return 0;
}

static void ev_uart_shutdown(ev_app_t *app, ev_module_t *mod)
{
    ev_uart_ctx_t *ctx = mod->ctx;

    (void)app;

    if (!ctx)
        return;

    if (ctx->housekeeping_ev) {
        event_free(ctx->housekeeping_ev);
        ctx->housekeeping_ev = NULL;
    }

    if (ctx->housekeeping_timerfd >= 0) {
        close(ctx->housekeeping_timerfd);
        ctx->housekeeping_timerfd = -1;
    }

    ev_uart_close_locked(ctx);
    free(ctx);
    mod->ctx = NULL;
    g_uart_ctx = NULL;
}

ev_module_t ev_uart_module = EV_MODULE_REGISTER("uart", ev_uart_init, ev_uart_shutdown);
