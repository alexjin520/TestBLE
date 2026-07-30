#include <event2/event.h>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <uv.h>

#include "fnirs_uv_stage.h"

enum uv_event_kind {
    UV_EVENT_POLL,
    UV_EVENT_TIMER,
    UV_EVENT_SIGNAL,
};

struct event_base {
    uv_loop_t *loop;
    uv_async_t once_async;
    pthread_mutex_t once_lock;
    struct uv_once_req *once_head;
    struct uv_once_req *once_tail;
};

struct event {
    struct event_base *base;
    enum uv_event_kind kind;
    evutil_socket_t fd;
    short flags;
    event_callback_fn cb;
    void *arg;
    int active;
    union {
        uv_poll_t poll;
        uv_timer_t timer;
        uv_signal_t signal;
    } handle;
};

struct uv_once_req {
    evutil_socket_t fd;
    short events;
    event_callback_fn cb;
    void *arg;
    struct uv_once_req *next;
};

static void uv_event_close_free(uv_handle_t *handle)
{
    struct event *ev = handle->data;

    free(ev);
}

static void uv_event_poll_cb(uv_poll_t *handle, int status, int events)
{
    struct event *ev = handle->data;
    short what = 0;

    if (!ev || status < 0)
        return;
    if (events & UV_READABLE)
        what |= EV_READ;
    if (events & UV_WRITABLE)
        what |= EV_WRITE;
    ev->cb(ev->fd, what, ev->arg);
}

static void uv_event_timer_cb(uv_timer_t *handle)
{
    struct event *ev = handle->data;

    if (!ev)
        return;
    ev->cb(-1, EV_TIMEOUT, ev->arg);
    if (!(ev->flags & EV_PERSIST))
        ev->active = 0;
}

static void uv_event_signal_cb(uv_signal_t *handle, int signum)
{
    struct event *ev = handle->data;

    if (ev)
        ev->cb(signum, EV_SIGNAL, ev->arg);
}

static void uv_once_async_cb(uv_async_t *async)
{
    struct event_base *base = async->data;
    struct uv_once_req *head;

    pthread_mutex_lock(&base->once_lock);
    head = base->once_head;
    base->once_head = NULL;
    base->once_tail = NULL;
    pthread_mutex_unlock(&base->once_lock);

    while (head) {
        struct uv_once_req *next = head->next;

        head->cb(head->fd, head->events, head->arg);
        free(head);
        head = next;
    }
}

struct event_base *event_base_new(void)
{
    struct event_base *base;

    fnirs_uv_stage("base calloc enter");
    base = calloc(1, sizeof(*base));
    if (!base)
        return NULL;
    fnirs_uv_stage("base calloc ready");
    fnirs_uv_stage("default loop enter");
    base->loop = uv_default_loop();
    if (!base->loop)
        goto fail;
    fnirs_uv_stage("default loop ready");
    fnirs_uv_stage("mutex init enter");
    if (pthread_mutex_init(&base->once_lock, NULL) != 0)
        goto fail_loop;
    fnirs_uv_stage("mutex init ready");
    fnirs_uv_stage("async init enter");
    if (uv_async_init(base->loop, &base->once_async, uv_once_async_cb) != 0)
        goto fail_mutex;
    fnirs_uv_stage("async init ready");
    base->once_async.data = base;
    fnirs_uv_stage("base ready");
    return base;

fail_mutex:
    pthread_mutex_destroy(&base->once_lock);
fail_loop:
    uv_loop_close(base->loop);
fail:
    free(base);
    return NULL;
}

void event_base_free(struct event_base *base)
{
    struct uv_once_req *req;

    if (!base)
        return;

    uv_close((uv_handle_t *)&base->once_async, NULL);
    uv_run(base->loop, UV_RUN_DEFAULT);

    pthread_mutex_lock(&base->once_lock);
    req = base->once_head;
    base->once_head = NULL;
    base->once_tail = NULL;
    pthread_mutex_unlock(&base->once_lock);
    while (req) {
        struct uv_once_req *next = req->next;
        free(req);
        req = next;
    }

    pthread_mutex_destroy(&base->once_lock);
    uv_loop_close(base->loop);
    free(base);
}

int event_base_dispatch(struct event_base *base)
{
    if (!base)
        return -1;
    (void)uv_run(base->loop, UV_RUN_DEFAULT);
    return 0;
}

int event_base_loop(struct event_base *base, int flags)
{
    if (!base)
        return -1;
    return uv_run(base->loop,
                  (flags & EVLOOP_NONBLOCK) ? UV_RUN_NOWAIT : UV_RUN_DEFAULT);
}

int event_base_loopbreak(struct event_base *base)
{
    if (!base)
        return -1;
    uv_stop(base->loop);
    uv_async_send(&base->once_async);
    return 0;
}

struct event *event_new(struct event_base *base, evutil_socket_t fd,
                        short events, event_callback_fn cb, void *arg)
{
    struct event *ev;

    if (!base || !cb)
        return NULL;

    ev = calloc(1, sizeof(*ev));
    if (!ev)
        return NULL;
    ev->base = base;
    ev->fd = fd;
    ev->flags = events;
    ev->cb = cb;
    ev->arg = arg;

    if (fd >= 0 && (events & (EV_READ | EV_WRITE))) {
        ev->kind = UV_EVENT_POLL;
        if (uv_poll_init(base->loop, &ev->handle.poll, fd) != 0)
            goto fail;
        ev->handle.poll.data = ev;
    } else {
        ev->kind = UV_EVENT_TIMER;
        if (uv_timer_init(base->loop, &ev->handle.timer) != 0)
            goto fail;
        ev->handle.timer.data = ev;
    }
    return ev;

fail:
    free(ev);
    return NULL;
}

struct event *evsignal_new(struct event_base *base, int sig,
                           event_callback_fn cb, void *arg)
{
    struct event *ev;

    if (!base || !cb)
        return NULL;
    ev = calloc(1, sizeof(*ev));
    if (!ev)
        return NULL;
    ev->base = base;
    ev->kind = UV_EVENT_SIGNAL;
    ev->fd = sig;
    ev->flags = EV_SIGNAL | EV_PERSIST;
    ev->cb = cb;
    ev->arg = arg;
    if (uv_signal_init(base->loop, &ev->handle.signal) != 0) {
        free(ev);
        return NULL;
    }
    ev->handle.signal.data = ev;
    return ev;
}

struct event *evtimer_new(struct event_base *base, event_callback_fn cb,
                          void *arg)
{
    return event_new(base, -1, EV_TIMEOUT, cb, arg);
}

static uint64_t uv_event_timeout_ms(const struct timeval *tv)
{
    uint64_t ms;

    if (!tv)
        return 0;
    ms = (uint64_t)tv->tv_sec * 1000ULL;
    ms += ((uint64_t)tv->tv_usec + 999ULL) / 1000ULL;
    return ms;
}

int event_add(struct event *ev, const struct timeval *tv)
{
    int rc;

    if (!ev)
        return -1;

    switch (ev->kind) {
    case UV_EVENT_POLL: {
        int uv_events = 0;
        if (ev->flags & EV_READ)
            uv_events |= UV_READABLE;
        if (ev->flags & EV_WRITE)
            uv_events |= UV_WRITABLE;
        rc = uv_poll_start(&ev->handle.poll, uv_events, uv_event_poll_cb);
        break;
    }
    case UV_EVENT_TIMER: {
        uint64_t timeout = uv_event_timeout_ms(tv);
        uint64_t repeat = (ev->flags & EV_PERSIST) ? timeout : 0;
        rc = uv_timer_start(&ev->handle.timer, uv_event_timer_cb, timeout, repeat);
        break;
    }
    case UV_EVENT_SIGNAL:
        rc = uv_signal_start(&ev->handle.signal, uv_event_signal_cb, ev->fd);
        break;
    default:
        return -1;
    }
    if (rc == 0)
        ev->active = 1;
    return rc == 0 ? 0 : -1;
}

int evtimer_add(struct event *ev, const struct timeval *tv)
{
    return event_add(ev, tv);
}

int event_del(struct event *ev)
{
    if (!ev || uv_is_closing((uv_handle_t *)&ev->handle))
        return -1;

    ev->active = 0;
    switch (ev->kind) {
    case UV_EVENT_POLL:
        return uv_poll_stop(&ev->handle.poll) == 0 ? 0 : -1;
    case UV_EVENT_TIMER:
        return uv_timer_stop(&ev->handle.timer) == 0 ? 0 : -1;
    case UV_EVENT_SIGNAL:
        return uv_signal_stop(&ev->handle.signal) == 0 ? 0 : -1;
    default:
        return -1;
    }
}

void event_free(struct event *ev)
{
    if (!ev)
        return;
    event_del(ev);
    if (!uv_is_closing((uv_handle_t *)&ev->handle))
        uv_close((uv_handle_t *)&ev->handle, uv_event_close_free);
}

int event_base_once(struct event_base *base, evutil_socket_t fd, short events,
                    event_callback_fn cb, void *arg,
                    const struct timeval *tv)
{
    struct uv_once_req *req;

    (void)tv;
    if (!base || !cb)
        return -1;

    req = calloc(1, sizeof(*req));
    if (!req)
        return -1;
    req->fd = fd;
    req->events = events;
    req->cb = cb;
    req->arg = arg;

    pthread_mutex_lock(&base->once_lock);
    if (base->once_tail)
        base->once_tail->next = req;
    else
        base->once_head = req;
    base->once_tail = req;
    pthread_mutex_unlock(&base->once_lock);

    return uv_async_send(&base->once_async) == 0 ? 0 : -1;
}
