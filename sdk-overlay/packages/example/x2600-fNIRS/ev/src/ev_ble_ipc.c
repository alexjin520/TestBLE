#include "ev_module.h"
#include "ev_app.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/un.h>

#include <event2/listener.h>

#include "fnirs_ble_ipc.h"
#include "my_interface.h"

typedef struct ev_ble_ipc_job {
    int fd;
    struct ev_ble_ipc_job *next;
} ev_ble_ipc_job_t;

typedef struct {
    struct evconnlistener *listener;
    pthread_t worker_tid;
    pthread_mutex_t q_mtx;
    pthread_cond_t q_cond;
    ev_ble_ipc_job_t *q_head;
    ev_ble_ipc_job_t *q_tail;
    volatile int worker_run;
} ev_ble_ipc_ctx_t;

static void ev_ble_ipc_job_enqueue(ev_ble_ipc_ctx_t *ctx, int fd)
{
    ev_ble_ipc_job_t *job = calloc(1, sizeof(*job));

    if (!job) {
        close(fd);
        return;
    }

    job->fd = fd;

    pthread_mutex_lock(&ctx->q_mtx);
    if (ctx->q_tail)
        ctx->q_tail->next = job;
    else
        ctx->q_head = job;
    ctx->q_tail = job;
    pthread_cond_signal(&ctx->q_cond);
    pthread_mutex_unlock(&ctx->q_mtx);

    WLOGI("ble ipc client accepted (queued)\r\n");
}

static void *ev_ble_ipc_worker(void *arg)
{
    ev_ble_ipc_ctx_t *ctx = arg;

    while (1) {
        ev_ble_ipc_job_t *job = NULL;

        pthread_mutex_lock(&ctx->q_mtx);
        while (!ctx->q_head && ctx->worker_run)
            pthread_cond_wait(&ctx->q_cond, &ctx->q_mtx);

        if (!ctx->q_head && !ctx->worker_run) {
            pthread_mutex_unlock(&ctx->q_mtx);
            break;
        }

        job = ctx->q_head;
        if (job) {
            ctx->q_head = job->next;
            if (!ctx->q_head)
                ctx->q_tail = NULL;
        }
        pthread_mutex_unlock(&ctx->q_mtx);

        if (!job)
            continue;

        if (job->fd >= 0)
            fnirs_ble_handle_client(job->fd);

        if (job->fd >= 0)
            close(job->fd);

        free(job);
    }

    return NULL;
}

static void ev_ble_ipc_on_accept(struct evconnlistener *listener, evutil_socket_t fd,
                                 struct sockaddr *sa, int socklen, void *arg)
{
    ev_ble_ipc_ctx_t *ctx = arg;

    (void)listener;
    (void)sa;
    (void)socklen;

    if (!ctx || !ctx->worker_run) {
        close(fd);
        return;
    }

    /*
     * Serialize IPC like the original fnirs_ble_ipc_thread accept loop.
     * Per-connection threads let STREAM_STOP (ABORT) race STREAM_START.
     */
    ev_ble_ipc_job_enqueue(ctx, fd);
}

static int ev_ble_ipc_init(ev_app_t *app, ev_module_t *mod)
{
    ev_ble_ipc_ctx_t *ctx = calloc(1, sizeof(*ctx));
    struct sockaddr_un addr;

    if (!ctx)
        return -1;

    mod->ctx = ctx;
    ctx->worker_run = 1;

    if (pthread_mutex_init(&ctx->q_mtx, NULL) != 0 ||
        pthread_cond_init(&ctx->q_cond, NULL) != 0) {
        free(ctx);
        mod->ctx = NULL;
        return -1;
    }

    if (pthread_create(&ctx->worker_tid, NULL, ev_ble_ipc_worker, ctx) != 0) {
        WLOGE("ble ipc worker thread failed\r\n");
        pthread_cond_destroy(&ctx->q_cond);
        pthread_mutex_destroy(&ctx->q_mtx);
        free(ctx);
        mod->ctx = NULL;
        return -1;
    }

    unlink(FNIRS_BLE_SOCK_PATH);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, FNIRS_BLE_SOCK_PATH, sizeof(addr.sun_path) - 1);

    ctx->listener = evconnlistener_new_bind(
        app->base, ev_ble_ipc_on_accept, ctx,
        LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE,
        -1, (struct sockaddr *)&addr, sizeof(addr));

    if (!ctx->listener) {
        WLOGE("ble ipc listen %s failed\r\n", FNIRS_BLE_SOCK_PATH);
        ctx->worker_run = 0;
        pthread_cond_signal(&ctx->q_cond);
        pthread_join(ctx->worker_tid, NULL);
        pthread_cond_destroy(&ctx->q_cond);
        pthread_mutex_destroy(&ctx->q_mtx);
        free(ctx);
        mod->ctx = NULL;
        return -1;
    }

    WLOGI("ble ipc listening: %s (libevent)\r\n", FNIRS_BLE_SOCK_PATH);
    return 0;
}

static void ev_ble_ipc_shutdown(ev_app_t *app, ev_module_t *mod)
{
    ev_ble_ipc_ctx_t *ctx = mod->ctx;
    ev_ble_ipc_job_t *job;

    (void)app;

    if (!ctx)
        return;

    if (ctx->listener) {
        evconnlistener_free(ctx->listener);
        ctx->listener = NULL;
    }

    unlink(FNIRS_BLE_SOCK_PATH);

    ctx->worker_run = 0;
    pthread_cond_signal(&ctx->q_cond);
    pthread_join(ctx->worker_tid, NULL);

    pthread_mutex_lock(&ctx->q_mtx);
    while (ctx->q_head) {
        job = ctx->q_head;
        ctx->q_head = job->next;
        if (job->fd >= 0)
            close(job->fd);
        free(job);
    }
    ctx->q_tail = NULL;
    pthread_mutex_unlock(&ctx->q_mtx);

    pthread_cond_destroy(&ctx->q_cond);
    pthread_mutex_destroy(&ctx->q_mtx);
    free(ctx);
    mod->ctx = NULL;
}

ev_module_t ev_ble_ipc_module =
    EV_MODULE_REGISTER("ble_ipc", ev_ble_ipc_init, ev_ble_ipc_shutdown);
