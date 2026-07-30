#include "ev_module.h"
#include "ev_app.h"
#include "my_server_embedded.h"
#include "ev_timer.h"

#include <errno.h>
#include <event2/event.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "my_interface.h"

typedef struct {
#ifdef FNIRS_UV_BACKEND
    pthread_t worker;
    int wake_fd;
    volatile uint32_t running;
    uint8_t worker_started;
#else
    struct event *listen_event;
    struct event *mainloop_event;
#endif
} ev_gatt_ctx_t;

#ifdef FNIRS_UV_BACKEND
/*
 * On this target, neither uv_poll_t nor poll(2) reliably reports readiness for
 * the Bluetooth SOCK_SEQPACKET listener or BlueZ's nested epoll fd. Both entry
 * points are deliberately nonblocking, so pump accept + dispatch every 10 ms
 * and use poll only on an eventfd for an interruptible sleep. GATT callbacks
 * already use ev_ble_async to marshal hardware work onto the UV main thread.
 */
static uint32_t ev_gatt_uv_running(ev_gatt_ctx_t *ctx)
{
    return __atomic_load_n(&ctx->running, __ATOMIC_ACQUIRE);
}

static void ev_gatt_uv_set_running(ev_gatt_ctx_t *ctx, uint32_t running)
{
    __atomic_store_n(&ctx->running, running, __ATOMIC_RELEASE);
}

static void *ev_gatt_uv_worker(void *arg)
{
    ev_gatt_ctx_t *ctx = arg;
    const int listen_fd = my_server_embedded_get_listen_fd();
    const int mainloop_fd = my_server_embedded_get_mainloop_fd();
    int listen_active = 1;

    WLOGI("UV GATT worker started: listen=%d mainloop=%d\r\n",
          listen_fd, mainloop_fd);

    while (ev_gatt_uv_running(ctx)) {
        struct pollfd wake;
        int rc;

        memset(&wake, 0, sizeof(wake));
        wake.fd = ctx->wake_fd;
        wake.events = POLLIN;
        rc = poll(&wake, 1, 10);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            WLOGE("UV GATT poll failed: %s\r\n", strerror(errno));
            break;
        }

        if (wake.revents) {
            uint64_t value;

            while (read(ctx->wake_fd, &value, sizeof(value)) ==
                   (ssize_t)sizeof(value)) {
            }
            if (!ev_gatt_uv_running(ctx))
                break;
        }

        if (listen_active) {
            int ret = my_server_embedded_accept();

            if (ret < 0)
                WLOGE("UV embedded GATT accept failed\r\n");
            else if (ret > 0)
                listen_active = 0;
        }

        {
            int ret = my_server_embedded_dispatch();

            if (ret < 0)
                WLOGE("UV embedded GATT dispatch failed: %d\r\n", ret);
            else if (ret > 0)
                listen_active = 1;
        }
    }

    ev_gatt_uv_set_running(ctx, 0);
    WLOGI("UV GATT worker stopped\r\n");
    return NULL;
}
#else
static void ev_gatt_accept_cb(evutil_socket_t fd, short events, void *arg)
{
    ev_gatt_ctx_t *ctx = arg;
    int ret;

    (void)fd;
    (void)events;
    ret = my_server_embedded_accept();
    if (ret < 0)
        WLOGE("embedded GATT accept failed\r\n");
    else if (ret > 0)
        event_del(ctx->listen_event);
}

static void ev_gatt_mainloop_cb(evutil_socket_t fd, short events, void *arg)
{
    ev_gatt_ctx_t *ctx = arg;
    int ret;

    (void)fd;
    (void)events;
    ret = my_server_embedded_dispatch();
    if (ret < 0)
        WLOGE("embedded GATT dispatch failed: %d\r\n", ret);
    else if (ret > 0 && event_add(ctx->listen_event, NULL) != 0)
        WLOGE("embedded GATT listener re-arm failed\r\n");
}
#endif

static int ev_gatt_init(ev_app_t *app, ev_module_t *mod)
{
    ev_gatt_ctx_t *ctx = calloc(1, sizeof(*ctx));
    int i;

#ifdef FNIRS_UV_BACKEND
    if (ctx)
        ctx->wake_fd = -1;
#endif
    (void)app;

    if (!ctx)
        return -1;

    for (i = 0; i < 15; i++) {
        if (access("/sys/class/bluetooth/hci0", F_OK) == 0)
            break;
        sleep(1);
    }

    if (my_server_embedded_start() != 0)
        goto fail;

#ifdef FNIRS_UV_BACKEND
    ctx->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (ctx->wake_fd < 0)
        goto fail;
    if (my_server_embedded_get_listen_fd() < 0 ||
        my_server_embedded_get_mainloop_fd() < 0)
        goto fail;

    ev_gatt_uv_set_running(ctx, 1);
    if (pthread_create(&ctx->worker, NULL, ev_gatt_uv_worker, ctx) != 0) {
        ev_gatt_uv_set_running(ctx, 0);
        goto fail;
    }
    ctx->worker_started = 1;
#else
    ctx->listen_event = event_new(app->base,
                                  my_server_embedded_get_listen_fd(),
                                  EV_READ | EV_PERSIST,
                                  ev_gatt_accept_cb, ctx);
    ctx->mainloop_event = event_new(app->base,
                                    my_server_embedded_get_mainloop_fd(),
                                    EV_READ | EV_PERSIST,
                                    ev_gatt_mainloop_cb, ctx);
    if (!ctx->listen_event || !ctx->mainloop_event ||
        event_add(ctx->listen_event, NULL) != 0 ||
        event_add(ctx->mainloop_event, NULL) != 0)
        goto fail;
#endif

    mod->ctx = ctx;
#ifdef FNIRS_UV_BACKEND
    WLOGI("embedded GATT attached to dedicated UV worker\r\n");
#else
    WLOGI("embedded GATT attached to main event loop\r\n");
#endif
    return 0;

fail:
#ifdef FNIRS_UV_BACKEND
    if (ctx->worker_started) {
        uint64_t one = 1;

        ev_gatt_uv_set_running(ctx, 0);
        (void)write(ctx->wake_fd, &one, sizeof(one));
        pthread_join(ctx->worker, NULL);
    }
    if (ctx->wake_fd >= 0)
        close(ctx->wake_fd);
#else
    if (ctx->listen_event)
        event_free(ctx->listen_event);
    if (ctx->mainloop_event)
        event_free(ctx->mainloop_event);
#endif
    my_server_embedded_stop();
    free(ctx);
    return -1;
}

static void ev_gatt_shutdown(ev_app_t *app, ev_module_t *mod)
{
    ev_gatt_ctx_t *ctx = mod->ctx;

    (void)app;

    if (!ctx)
        return;

#ifdef FNIRS_UV_BACKEND
    if (ctx->worker_started) {
        uint64_t one = 1;

        ev_gatt_uv_set_running(ctx, 0);
        (void)write(ctx->wake_fd, &one, sizeof(one));
        pthread_join(ctx->worker, NULL);
        ctx->worker_started = 0;
    }
    if (ctx->wake_fd >= 0) {
        close(ctx->wake_fd);
        ctx->wake_fd = -1;
    }
#else
    event_del(ctx->listen_event);
    event_del(ctx->mainloop_event);
    event_free(ctx->listen_event);
    event_free(ctx->mainloop_event);
#endif
    my_server_embedded_stop();
    free(ctx);
    mod->ctx = NULL;

    WLOGI("embedded GATT server stopped\r\n");
}

ev_module_t ev_gatt_module =
    EV_MODULE_REGISTER("gatt", ev_gatt_init, ev_gatt_shutdown);
