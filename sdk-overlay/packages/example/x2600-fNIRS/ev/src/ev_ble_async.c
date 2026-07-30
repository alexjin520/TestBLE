#include "ev_module.h"
#include "ev_app.h"
#include "ev_ble_async.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <event2/event.h>

#include "fnirs_ble_ipc.h"
#include "my_interface.h"

typedef struct ev_ble_job {
    uint8_t cmd;
    uint16_t len;
    uint8_t payload[512];
    int rsp_fd;
    struct ev_ble_job *next;
} ev_ble_job_t;

typedef struct {
    struct event_base *base;
    struct event *notify_ev;
    int notify_fd;
    pthread_mutex_t q_mtx;
    ev_ble_job_t *q_head;
    ev_ble_job_t *q_tail;
} ev_ble_async_ctx_t;

static ev_ble_async_ctx_t g_ble_async;
static pthread_t g_main_tid;

static void ev_ble_run_jobs_cb(evutil_socket_t fd, short what, void *arg);

static int ev_ble_cmd_can_run_direct(uint8_t cmd)
{
    /*
     * STREAM_START completes from timer callbacks, so it must not be run
     * synchronously on the main loop.  The other commands write their response
     * before returning.
     */
    return cmd != FNIRS_BLE_CMD_STREAM_START;
}

static void ev_ble_trace(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    int fd;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;

    fd = open("/tmp/ble_async.trace", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        (void)write(fd, line, (size_t)n);
        close(fd);
    }

    WLOGI("%s", line);
}

static ev_ble_job_t *ev_ble_job_dequeue(void)
{
    ev_ble_job_t *job = NULL;

    pthread_mutex_lock(&g_ble_async.q_mtx);
    job = g_ble_async.q_head;
    if (job) {
        g_ble_async.q_head = job->next;
        if (!g_ble_async.q_head)
            g_ble_async.q_tail = NULL;
        job->next = NULL;
    }
    pthread_mutex_unlock(&g_ble_async.q_mtx);

    return job;
}

static void ev_ble_process_queued_jobs(void)
{
    ev_ble_job_t *job;

    while ((job = ev_ble_job_dequeue()) != NULL) {
        ev_ble_trace("process job cmd=0x%02x fd=%d\r\n", job->cmd, job->rsp_fd);
        fnirs_ble_ev_process_job(job->cmd, job->payload, job->len,
                                 job->rsp_fd);
        free(job);
    }
}

static void ev_ble_kick_main_loop(void)
{
    struct timeval tv_zero = { 0, 0 };

    if (!g_ble_async.base)
        return;

    if (event_base_once(g_ble_async.base, -1, EV_TIMEOUT, ev_ble_run_jobs_cb,
                        NULL, &tv_zero) != 0) {
        ev_ble_trace("event_base_once failed\r\n");
    }

    if (g_ble_async.notify_fd >= 0) {
        uint64_t one = 1;

        (void)write(g_ble_async.notify_fd, &one, sizeof(one));
    }
}

static void ev_ble_run_jobs_cb(evutil_socket_t fd, short what, void *arg)
{
    (void)fd;
    (void)what;
    (void)arg;

    if (!pthread_equal(pthread_self(), g_main_tid)) {
        ev_ble_trace("run_jobs_cb defer (not main thread)\r\n");
        ev_ble_kick_main_loop();
        return;
    }

    ev_ble_trace("run_jobs_cb on main loop\r\n");
    ev_ble_process_queued_jobs();
}

static void ev_ble_job_enqueue(ev_ble_job_t *job)
{
    pthread_mutex_lock(&g_ble_async.q_mtx);
    if (g_ble_async.q_tail)
        g_ble_async.q_tail->next = job;
    else
        g_ble_async.q_head = job;
    g_ble_async.q_tail = job;
    pthread_mutex_unlock(&g_ble_async.q_mtx);

    ev_ble_kick_main_loop();
}

static void ev_ble_async_drain(evutil_socket_t fd, short events, void *arg)
{
    uint64_t n;

    (void)events;
    (void)arg;

    if (fd >= 0) {
        for (;;) {
            ssize_t rc = read(fd, &n, sizeof(n));

            if (rc == (ssize_t)sizeof(n))
                continue;
            break;
        }
    }

    ev_ble_process_queued_jobs();
}

static void ev_ble_queue_init(struct event_base *base)
{
    memset(&g_ble_async, 0, sizeof(g_ble_async));
    pthread_mutex_init(&g_ble_async.q_mtx, NULL);
    g_ble_async.base = base;
    g_ble_async.notify_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (g_ble_async.notify_fd < 0)
        return;

    g_ble_async.notify_ev = event_new(base, g_ble_async.notify_fd,
                                      EV_READ | EV_PERSIST,
                                      ev_ble_async_drain, NULL);
    if (!g_ble_async.notify_ev) {
        close(g_ble_async.notify_fd);
        g_ble_async.notify_fd = -1;
        return;
    }

    if (event_add(g_ble_async.notify_ev, NULL) != 0) {
        event_free(g_ble_async.notify_ev);
        g_ble_async.notify_ev = NULL;
        close(g_ble_async.notify_fd);
        g_ble_async.notify_fd = -1;
    }
}

static void ev_ble_queue_shutdown(void)
{
    ev_ble_job_t *job;

    if (g_ble_async.notify_ev) {
        event_free(g_ble_async.notify_ev);
        g_ble_async.notify_ev = NULL;
    }

    if (g_ble_async.notify_fd >= 0) {
        close(g_ble_async.notify_fd);
        g_ble_async.notify_fd = -1;
    }

    while ((job = ev_ble_job_dequeue()) != NULL) {
        if (job->rsp_fd >= 0)
            close(job->rsp_fd);
        free(job);
    }

    pthread_mutex_destroy(&g_ble_async.q_mtx);
    memset(&g_ble_async, 0, sizeof(g_ble_async));
}

static int ev_ble_wait_response(int rsp_fd, int poll_ms, uint8_t *rhdr,
                                uint8_t *rsp, uint16_t rsp_max,
                                uint16_t *rsp_len_out, uint8_t *status_out)
{
    struct pollfd pfd;
    ssize_t n;
    uint16_t rsp_len = 0;
    int waited = 0;

    pfd.fd = rsp_fd;
    pfd.events = POLLIN;

    while (waited < poll_ms) {
        int slice = poll_ms - waited;

        if (slice > 20)
            slice = 20;

        if (poll(&pfd, 1, slice) > 0)
            break;

        waited += slice;
    }

    if (waited >= poll_ms) {
        ev_ble_trace("poll timeout fd=%d waited=%dms\r\n", rsp_fd, poll_ms);
        return -1;
    }

    n = read(rsp_fd, rhdr, 6);
    if (n != 6 || rhdr[0] != FNIRS_BLE_RSP_MAGIC0 ||
        rhdr[1] != FNIRS_BLE_RSP_MAGIC1) {
        ev_ble_trace("bad rsp hdr n=%zd\r\n", n);
        return -1;
    }

    if (status_out)
        *status_out = rhdr[3];

    rsp_len = (uint16_t)rhdr[4] | ((uint16_t)rhdr[5] << 8);
    if (rsp_len > 0 && rsp && rsp_max > 0) {
        if (rsp_len > rsp_max)
            rsp_len = rsp_max;
        n = read(rsp_fd, rsp, rsp_len);
        if (n != (ssize_t)rsp_len) {
            ev_ble_trace("bad rsp body n=%zd want=%u\r\n", n, rsp_len);
            return -1;
        }
        if (rsp_len_out)
            *rsp_len_out = rsp_len;
    }

    return 0;
}

int32_t ev_ble_async_dispatch(uint8_t cmd, const uint8_t *req, uint16_t req_len,
	                              uint8_t *status_out, uint8_t *rsp, uint16_t rsp_max,
	                              uint16_t *rsp_len_out)
{
	    int pair[2];
	    ev_ble_job_t *job;
	    uint8_t rhdr[6];
	    int poll_ms;

    if (status_out)
        *status_out = 0xFF;
    if (rsp_len_out)
        *rsp_len_out = 0;

    if (!g_ble_async.base) {
        ev_ble_trace("dispatch not ready (no base)\r\n");
        return -1;
    }

    if (req_len > 512)
        return -1;

	    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
	        ev_ble_trace("socketpair failed\r\n");
	        return -1;
	    }

	    switch (cmd) {
	    case FNIRS_BLE_CMD_SCAN:
	        poll_ms = 12000;
	        break;
	    case FNIRS_BLE_CMD_STREAM_START:
	        poll_ms = 8000;
	        break;
	    default:
	        poll_ms = 5000;
	        break;
	    }

	    if (pthread_equal(pthread_self(), g_main_tid) &&
	        ev_ble_cmd_can_run_direct(cmd)) {
	        ev_ble_trace("dispatch cmd=0x%02x direct on main thread\r\n", cmd);
	        fnirs_ble_ev_process_job(cmd, req, req_len, pair[0]);

	        if (ev_ble_wait_response(pair[1], poll_ms, rhdr, rsp, rsp_max,
	                                 rsp_len_out, status_out) != 0) {
	            close(pair[1]);
	            return -1;
	        }

	        ev_ble_trace("dispatch cmd=0x%02x direct ok\r\n", cmd);
	        close(pair[1]);
	        return 0;
	    }

	    job = calloc(1, sizeof(*job));
	    if (!job) {
	        close(pair[0]);
	        close(pair[1]);
        return -1;
    }

    job->cmd = cmd;
    job->len = req_len;
    job->rsp_fd = pair[0];
    if (req_len > 0 && req)
        memcpy(job->payload, req, req_len);

	    ev_ble_trace("dispatch cmd=0x%02x\r\n", cmd);
	    ev_ble_job_enqueue(job);

	    if (ev_ble_wait_response(pair[1], poll_ms, rhdr, rsp, rsp_max,
                             rsp_len_out, status_out) != 0) {
        fnirs_ble_ev_cancel_async();
        close(pair[1]);
        return -1;
    }

    ev_ble_trace("dispatch cmd=0x%02x ok\r\n", cmd);
    close(pair[1]);
    return 0;
}

static int ev_ble_async_init(ev_app_t *app, ev_module_t *mod)
{
    (void)mod;

    g_main_tid = pthread_self();
    unlink("/tmp/ble_async.trace");

    fnirs_ble_ev_init(app->base);
    ev_ble_queue_init(app->base);
    if (g_ble_async.notify_fd < 0) {
        fnirs_ble_ev_shutdown();
        WLOGE("ev_ble_async: eventfd setup failed\r\n");
        return -1;
    }

    ev_ble_trace("GATT -> main loop dispatch ready\r\n");
    return 0;
}

static void ev_ble_async_shutdown(ev_app_t *app, ev_module_t *mod)
{
    (void)app;
    (void)mod;

    ev_ble_queue_shutdown();
    fnirs_ble_ev_shutdown();
}

ev_module_t ev_ble_async_module =
    EV_MODULE_REGISTER("ble_async", ev_ble_async_init, ev_ble_async_shutdown);
