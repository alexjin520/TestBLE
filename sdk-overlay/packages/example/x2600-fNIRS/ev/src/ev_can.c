#include "ev_module.h"
#include "ev_app.h"
#include "ev_can.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/timerfd.h>

#include <event2/event.h>
#include <linux/can.h>

#include "fNIRS.h"
#include "fnode.h"
#include "my_interface.h"

/* Match legacy fhub_task ~50ms loop; poll slightly faster. */
#define EV_CAN_HUB_POLL_MS 20
/* Match fnode_task IDLE scan cadence (FNODE_SCAN_RESCAN_MS). */
#define EV_CAN_IDLE_SCAN_MS 100
/* OTA wait/poll cadence; ACK windows are seconds-scale. */
#define EV_CAN_OTA_TICK_MS 10
/* Factory test uses the same ms-scale timing as sampling. */
#define EV_CAN_FACTORY_TICK_MS 1

typedef struct {
    ev_app_t *app;
    struct event *read_ev;
    struct event *hub_poll_ev;
    struct event *idle_scan_ev;
    struct event *sample_tick_ev;
    struct event *ota_tick_ev;
    struct event *factory_tick_ev;
    int can_fd;
    int hub_poll_timerfd;
    int idle_scan_timerfd;
    int sample_tick_timerfd;
    int ota_tick_timerfd;
    int factory_tick_timerfd;
    fnode_handler_t node;
} ev_can_ctx_t;

static ev_can_ctx_t *g_can_ctx;

static void ev_can_drain(ev_can_ctx_t *ctx)
{
    struct canfd_frame frame;
    ssize_t n;

    if (!ctx || !ctx->node || ctx->can_fd < 0)
        return;

    for (;;) {
        memset(&frame, 0, sizeof(frame));
        n = read(ctx->can_fd, &frame, sizeof(frame));
        if (n == (ssize_t)sizeof(frame)) {
            fnode_ev_hub_process_frame(ctx->node, &frame);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        break;
    }
}

static void ev_can_pump(ev_can_ctx_t *ctx)
{
    fnode_handler_t node;

    if (!ctx)
        return;

    node = fNIRS_handler_ref();
    if (!node)
        return;

    ctx->node = node;

    if (ctx->can_fd >= 0)
        ev_can_drain(ctx);

    fnode_ev_hub_service(node);
}

void ev_can_hub_service(void)
{
    ev_can_pump(g_can_ctx);
}

static int ev_can_hub_poll_timerfd_open(void)
{
    struct itimerspec its;
    int fd;

    fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0)
        return -1;

    memset(&its, 0, sizeof(its));
    its.it_value.tv_nsec = (long)EV_CAN_HUB_POLL_MS * 1000000L;
    its.it_interval.tv_nsec = (long)EV_CAN_HUB_POLL_MS * 1000000L;
    if (timerfd_settime(fd, 0, &its, NULL) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static void ev_can_hub_poll_cb(evutil_socket_t fd, short events, void *arg)
{
    ev_can_ctx_t *ctx = arg;
    uint64_t expirations;

    (void)fd;
    (void)events;

    while (read((int)fd, &expirations, sizeof(expirations)) ==
           (ssize_t)sizeof(expirations)) {
        ev_can_pump(ctx);
    }
}

static int ev_can_idle_scan_timerfd_open(void)
{
    struct itimerspec its;
    int fd;

    fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0)
        return -1;

    memset(&its, 0, sizeof(its));
    its.it_value.tv_nsec = (long)EV_CAN_IDLE_SCAN_MS * 1000000L;
    its.it_interval.tv_nsec = (long)EV_CAN_IDLE_SCAN_MS * 1000000L;
    if (timerfd_settime(fd, 0, &its, NULL) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static void ev_can_start_hub_poll(ev_can_ctx_t *ctx)
{
    if (!ctx || ctx->hub_poll_ev)
        return;

    ctx->hub_poll_timerfd = ev_can_hub_poll_timerfd_open();
    if (ctx->hub_poll_timerfd < 0)
        return;

    ctx->hub_poll_ev = event_new(ctx->app->base, ctx->hub_poll_timerfd,
                                 EV_READ | EV_PERSIST, ev_can_hub_poll_cb, ctx);
    if (!ctx->hub_poll_ev) {
        close(ctx->hub_poll_timerfd);
        ctx->hub_poll_timerfd = -1;
        return;
    }

    if (event_add(ctx->hub_poll_ev, NULL) != 0) {
        event_free(ctx->hub_poll_ev);
        ctx->hub_poll_ev = NULL;
        close(ctx->hub_poll_timerfd);
        ctx->hub_poll_timerfd = -1;
        return;
    }

    WLOGI("can hub poll armed (%dms)\r\n", EV_CAN_HUB_POLL_MS);
}

static void ev_can_idle_scan_cb(evutil_socket_t fd, short events, void *arg)
{
    ev_can_ctx_t *ctx = arg;
    uint64_t expirations;

    (void)events;

    if (!ctx || !ctx->node)
        return;

    while (read((int)fd, &expirations, sizeof(expirations)) ==
           (ssize_t)sizeof(expirations)) {
        fnode_ev_idle_service(ctx->node);
    }
}

static void ev_can_start_idle_scan(ev_can_ctx_t *ctx)
{
    if (!ctx || ctx->idle_scan_ev)
        return;

    ctx->idle_scan_timerfd = ev_can_idle_scan_timerfd_open();
    if (ctx->idle_scan_timerfd < 0)
        return;

    ctx->idle_scan_ev = event_new(ctx->app->base, ctx->idle_scan_timerfd,
                                  EV_READ | EV_PERSIST, ev_can_idle_scan_cb, ctx);
    if (!ctx->idle_scan_ev) {
        close(ctx->idle_scan_timerfd);
        ctx->idle_scan_timerfd = -1;
        return;
    }

    if (event_add(ctx->idle_scan_ev, NULL) != 0) {
        event_free(ctx->idle_scan_ev);
        ctx->idle_scan_ev = NULL;
        close(ctx->idle_scan_timerfd);
        ctx->idle_scan_timerfd = -1;
        return;
    }

    WLOGI("can idle scan armed (%dms)\r\n", EV_CAN_IDLE_SCAN_MS);
}

static int ev_can_sample_tick_timerfd_open(void)
{
    return timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
}

static int ev_can_sample_timerfd_arm(int fd, uint32_t delay_us)
{
    struct itimerspec its;

    if (fd < 0)
        return -1;
    if (delay_us == 0)
        delay_us = 1;

    memset(&its, 0, sizeof(its));
    its.it_value.tv_sec = delay_us / 1000000U;
    its.it_value.tv_nsec = (long)(delay_us % 1000000U) * 1000L;
    return timerfd_settime(fd, 0, &its, NULL);
}

static void ev_can_sample_tick_cb(evutil_socket_t fd, short events, void *arg)
{
    ev_can_ctx_t *ctx = arg;
    uint64_t expirations;

    (void)events;

    if (!ctx || !ctx->node)
        return;

    while (read((int)fd, &expirations, sizeof(expirations)) ==
           (ssize_t)sizeof(expirations)) {
        if (!fnode_ev_sample_active(ctx->node)) {
            ev_can_sample_disarm();
            return;
        }
        fnode_ev_sample_tick(ctx->node);
        if (!fnode_ev_sample_active(ctx->node)) {
            ev_can_sample_disarm();
            return;
        }
        if (ev_can_sample_timerfd_arm(ctx->sample_tick_timerfd,
                fnode_ev_sample_next_delay_us(ctx->node)) < 0) {
            WLOGE("can sample timer rearm failed: %s\r\n", strerror(errno));
            ev_can_sample_disarm();
            return;
        }
    }
}

static void ev_can_sample_arm_evt(evutil_socket_t fd, short what, void *arg)
{
    (void)fd;
    (void)what;
    (void)arg;

    ev_can_sample_arm();
}

void ev_can_sample_arm_deferred(void)
{
    ev_can_ctx_t *ctx = g_can_ctx;
    struct timeval tv = { 0, 0 };

    if (!ctx || !ctx->app || !ctx->app->base) {
        WLOGW("ev_can_sample_arm_deferred: can ctx not ready\r\n");
        return;
    }

    if (event_base_once(ctx->app->base, -1, EV_TIMEOUT, ev_can_sample_arm_evt,
                        NULL, &tv) != 0)
        WLOGW("ev_can_sample_arm_deferred: event_base_once failed\r\n");
}

void ev_can_sample_arm(void)
{
    ev_can_ctx_t *ctx = g_can_ctx;

    if (!ctx) {
        WLOGW("ev_can_sample_arm: no can ctx\r\n");
        return;
    }

    if (ctx->sample_tick_ev)
        ev_can_sample_disarm();

    if (!ctx->node)
        ctx->node = fNIRS_handler_ref();
    if (!ctx->node) {
        WLOGW("ev_can_sample_arm: no fnode handler\r\n");
        return;
    }

    ctx->sample_tick_timerfd = ev_can_sample_tick_timerfd_open();
    if (ctx->sample_tick_timerfd < 0) {
        WLOGW("ev_can_sample_arm: timerfd failed\r\n");
        return;
    }

    ctx->sample_tick_ev = event_new(ctx->app->base, ctx->sample_tick_timerfd,
                                    EV_READ | EV_PERSIST, ev_can_sample_tick_cb, ctx);
    if (!ctx->sample_tick_ev) {
        WLOGW("ev_can_sample_arm: event_new failed\r\n");
        close(ctx->sample_tick_timerfd);
        ctx->sample_tick_timerfd = -1;
        return;
    }

    if (event_add(ctx->sample_tick_ev, NULL) != 0) {
        WLOGW("ev_can_sample_arm: event_add failed\r\n");
        event_free(ctx->sample_tick_ev);
        ctx->sample_tick_ev = NULL;
        close(ctx->sample_tick_timerfd);
        ctx->sample_tick_timerfd = -1;
        return;
    }

    if (ev_can_sample_timerfd_arm(ctx->sample_tick_timerfd, 1) < 0) {
        WLOGW("ev_can_sample_arm: timerfd arm failed\r\n");
        ev_can_sample_disarm();
        return;
    }

    WLOGI("can sample one-shot timer armed\r\n");
}

void ev_can_sample_disarm(void)
{
    ev_can_ctx_t *ctx = g_can_ctx;

    if (!ctx)
        return;

    if (ctx->sample_tick_ev) {
        event_del(ctx->sample_tick_ev);
        event_free(ctx->sample_tick_ev);
        ctx->sample_tick_ev = NULL;
    }

    if (ctx->sample_tick_timerfd >= 0) {
        close(ctx->sample_tick_timerfd);
        ctx->sample_tick_timerfd = -1;
    }
}

static int ev_can_ota_tick_timerfd_open(void)
{
    struct itimerspec its;
    int fd;

    fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0)
        return -1;

    memset(&its, 0, sizeof(its));
    its.it_value.tv_nsec = (long)EV_CAN_OTA_TICK_MS * 1000000L;
    its.it_interval.tv_nsec = (long)EV_CAN_OTA_TICK_MS * 1000000L;
    if (timerfd_settime(fd, 0, &its, NULL) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static void ev_can_ota_tick_cb(evutil_socket_t fd, short events, void *arg)
{
    ev_can_ctx_t *ctx = arg;
    uint64_t expirations;

    (void)events;

    if (!ctx || !ctx->node)
        return;

    while (read((int)fd, &expirations, sizeof(expirations)) ==
           (ssize_t)sizeof(expirations)) {
        if (!fnode_ev_ota_active(ctx->node)) {
            ev_can_ota_disarm();
            return;
        }
        fnode_ev_ota_tick(ctx->node);
    }
}

static void ev_can_ota_arm_evt(evutil_socket_t fd, short what, void *arg)
{
    (void)fd;
    (void)what;
    (void)arg;

    ev_can_ota_arm();
}

void ev_can_ota_arm_deferred(void)
{
    ev_can_ctx_t *ctx = g_can_ctx;
    struct timeval tv = { 0, 0 };

    if (!ctx || !ctx->app || !ctx->app->base) {
        WLOGW("ev_can_ota_arm_deferred: can ctx not ready\r\n");
        return;
    }

    if (event_base_once(ctx->app->base, -1, EV_TIMEOUT, ev_can_ota_arm_evt,
                        NULL, &tv) != 0)
        WLOGW("ev_can_ota_arm_deferred: event_base_once failed\r\n");
}

void ev_can_ota_arm(void)
{
    ev_can_ctx_t *ctx = g_can_ctx;

    if (!ctx) {
        WLOGW("ev_can_ota_arm: no can ctx\r\n");
        return;
    }

    if (ctx->ota_tick_ev)
        ev_can_ota_disarm();

    if (!ctx->node)
        ctx->node = fNIRS_handler_ref();
    if (!ctx->node) {
        WLOGW("ev_can_ota_arm: no fnode handler\r\n");
        return;
    }

    ctx->ota_tick_timerfd = ev_can_ota_tick_timerfd_open();
    if (ctx->ota_tick_timerfd < 0) {
        WLOGW("ev_can_ota_arm: timerfd failed\r\n");
        return;
    }

    ctx->ota_tick_ev = event_new(ctx->app->base, ctx->ota_tick_timerfd,
                                 EV_READ | EV_PERSIST, ev_can_ota_tick_cb, ctx);
    if (!ctx->ota_tick_ev) {
        WLOGW("ev_can_ota_arm: event_new failed\r\n");
        close(ctx->ota_tick_timerfd);
        ctx->ota_tick_timerfd = -1;
        return;
    }

    if (event_add(ctx->ota_tick_ev, NULL) != 0) {
        WLOGW("ev_can_ota_arm: event_add failed\r\n");
        event_free(ctx->ota_tick_ev);
        ctx->ota_tick_ev = NULL;
        close(ctx->ota_tick_timerfd);
        ctx->ota_tick_timerfd = -1;
        return;
    }

    WLOGI("can ota tick armed (%dms)\r\n", EV_CAN_OTA_TICK_MS);
}

void ev_can_ota_disarm(void)
{
    ev_can_ctx_t *ctx = g_can_ctx;

    if (!ctx)
        return;

    if (ctx->ota_tick_ev) {
        event_del(ctx->ota_tick_ev);
        event_free(ctx->ota_tick_ev);
        ctx->ota_tick_ev = NULL;
    }

    if (ctx->ota_tick_timerfd >= 0) {
        close(ctx->ota_tick_timerfd);
        ctx->ota_tick_timerfd = -1;
    }
}

static int ev_can_factory_tick_timerfd_open(void)
{
    struct itimerspec its;
    int fd;

    fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0)
        return -1;

    memset(&its, 0, sizeof(its));
    its.it_value.tv_nsec = (long)EV_CAN_FACTORY_TICK_MS * 1000000L;
    its.it_interval.tv_nsec = (long)EV_CAN_FACTORY_TICK_MS * 1000000L;
    if (timerfd_settime(fd, 0, &its, NULL) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static void ev_can_factory_tick_cb(evutil_socket_t fd, short events, void *arg)
{
    ev_can_ctx_t *ctx = arg;
    uint64_t expirations;

    (void)events;

    if (!ctx || !ctx->node)
        return;

    while (read((int)fd, &expirations, sizeof(expirations)) ==
           (ssize_t)sizeof(expirations)) {
        if (!fnode_ev_factory_active(ctx->node)) {
            ev_can_factory_disarm();
            return;
        }
        fnode_ev_factory_tick(ctx->node);
    }
}

static void ev_can_factory_arm_evt(evutil_socket_t fd, short what, void *arg)
{
    (void)fd;
    (void)what;
    (void)arg;

    ev_can_factory_arm();
}

void ev_can_factory_arm_deferred(void)
{
    ev_can_ctx_t *ctx = g_can_ctx;
    struct timeval tv = { 0, 0 };

    if (!ctx || !ctx->app || !ctx->app->base) {
        WLOGW("ev_can_factory_arm_deferred: can ctx not ready\r\n");
        return;
    }

    if (event_base_once(ctx->app->base, -1, EV_TIMEOUT, ev_can_factory_arm_evt,
                        NULL, &tv) != 0)
        WLOGW("ev_can_factory_arm_deferred: event_base_once failed\r\n");
}

void ev_can_factory_arm(void)
{
    ev_can_ctx_t *ctx = g_can_ctx;

    if (!ctx) {
        WLOGW("ev_can_factory_arm: no can ctx\r\n");
        return;
    }

    if (ctx->factory_tick_ev)
        ev_can_factory_disarm();

    if (!ctx->node)
        ctx->node = fNIRS_handler_ref();
    if (!ctx->node) {
        WLOGW("ev_can_factory_arm: no fnode handler\r\n");
        return;
    }

    ctx->factory_tick_timerfd = ev_can_factory_tick_timerfd_open();
    if (ctx->factory_tick_timerfd < 0) {
        WLOGW("ev_can_factory_arm: timerfd failed\r\n");
        return;
    }

    ctx->factory_tick_ev = event_new(ctx->app->base, ctx->factory_tick_timerfd,
                                     EV_READ | EV_PERSIST, ev_can_factory_tick_cb, ctx);
    if (!ctx->factory_tick_ev) {
        WLOGW("ev_can_factory_arm: event_new failed\r\n");
        close(ctx->factory_tick_timerfd);
        ctx->factory_tick_timerfd = -1;
        return;
    }

    if (event_add(ctx->factory_tick_ev, NULL) != 0) {
        WLOGW("ev_can_factory_arm: event_add failed\r\n");
        event_free(ctx->factory_tick_ev);
        ctx->factory_tick_ev = NULL;
        close(ctx->factory_tick_timerfd);
        ctx->factory_tick_timerfd = -1;
        return;
    }

    WLOGI("can factory tick armed (%dms)\r\n", EV_CAN_FACTORY_TICK_MS);
}

void ev_can_factory_disarm(void)
{
    ev_can_ctx_t *ctx = g_can_ctx;

    if (!ctx)
        return;

    if (ctx->factory_tick_ev) {
        event_del(ctx->factory_tick_ev);
        event_free(ctx->factory_tick_ev);
        ctx->factory_tick_ev = NULL;
    }

    if (ctx->factory_tick_timerfd >= 0) {
        close(ctx->factory_tick_timerfd);
        ctx->factory_tick_timerfd = -1;
    }
}

static void ev_can_read_cb(evutil_socket_t fd, short events, void *arg)
{
    ev_can_ctx_t *ctx = arg;

    (void)fd;
    (void)events;

    ev_can_pump(ctx);
}

void ev_can_try_attach(void)
{
    ev_can_ctx_t *ctx = g_can_ctx;
    fnode_handler_t node;
    int fd;
    int flags;

    if (!ctx || ctx->read_ev)
        return;

    node = fNIRS_handler_ref();
    if (!node)
        return;

    fd = fnode_ev_can_fd(node);
    if (fd < 0)
        return;

    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return;

    ctx->node = node;
    ctx->can_fd = fd;
    ctx->read_ev = event_new(ctx->app->base, fd, EV_READ | EV_PERSIST,
                             ev_can_read_cb, ctx);
    if (!ctx->read_ev) {
        ctx->can_fd = -1;
        ctx->node = NULL;
        return;
    }

    if (event_add(ctx->read_ev, NULL) != 0) {
        event_free(ctx->read_ev);
        ctx->read_ev = NULL;
        ctx->can_fd = -1;
        ctx->node = NULL;
        return;
    }

    ev_can_start_hub_poll(ctx);
    ev_can_start_idle_scan(ctx);

    /* Send one scan immediately; the periodic timer maintains discovery. */
    fnode_ev_kick_idle(node);

    WLOGI("can listener attached (fd=%d)\r\n", fd);
}

static int ev_can_init(ev_app_t *app, ev_module_t *mod)
{
    ev_can_ctx_t *ctx = calloc(1, sizeof(*ctx));

    if (!ctx)
        return -1;

    ctx->app = app;
    ctx->can_fd = -1;
    ctx->hub_poll_timerfd = -1;
    ctx->idle_scan_timerfd = -1;
    ctx->sample_tick_timerfd = -1;
    ctx->ota_tick_timerfd = -1;
    ctx->factory_tick_timerfd = -1;
    mod->ctx = ctx;
    g_can_ctx = ctx;

    WLOGI("can module ready (lazy attach after fNIRS init)\r\n");
    return 0;
}

static void ev_can_shutdown(ev_app_t *app, ev_module_t *mod)
{
    ev_can_ctx_t *ctx = mod->ctx;

    (void)app;

    if (!ctx)
        return;

    if (ctx->hub_poll_ev) {
        event_del(ctx->hub_poll_ev);
        event_free(ctx->hub_poll_ev);
        ctx->hub_poll_ev = NULL;
    }

    if (ctx->hub_poll_timerfd >= 0) {
        close(ctx->hub_poll_timerfd);
        ctx->hub_poll_timerfd = -1;
    }

    if (ctx->idle_scan_ev) {
        event_del(ctx->idle_scan_ev);
        event_free(ctx->idle_scan_ev);
        ctx->idle_scan_ev = NULL;
    }

    if (ctx->idle_scan_timerfd >= 0) {
        close(ctx->idle_scan_timerfd);
        ctx->idle_scan_timerfd = -1;
    }

    ev_can_sample_disarm();
    ev_can_ota_disarm();
    ev_can_factory_disarm();

    if (ctx->read_ev) {
        event_del(ctx->read_ev);
        event_free(ctx->read_ev);
        ctx->read_ev = NULL;
    }

    ctx->can_fd = -1;
    ctx->node = NULL;

    if (g_can_ctx == ctx)
        g_can_ctx = NULL;

    free(ctx);
    mod->ctx = NULL;
}

ev_module_t ev_can_module = EV_MODULE_REGISTER("can", ev_can_init, ev_can_shutdown);
