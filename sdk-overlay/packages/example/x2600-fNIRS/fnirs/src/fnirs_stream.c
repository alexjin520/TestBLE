
#include "stdio.h"
#include "string.h"
#include "errno.h"
#include "unistd.h"
#include "fcntl.h"
#include "sys/stat.h"
#include "pthread.h"

#include "fnirs_stream.h"
#include "fnirs_ble_ipc.h"
#include "my_interface.h"

static volatile int g_stream_active;
static int g_data_fd = -1;

#ifdef FNIRS_EMBEDDED
#define FNIRS_STREAM_RING_CAP 65536
static int16_t g_stream_ring[FNIRS_STREAM_RING_CAP];
static uint32_t g_stream_ring_r;
static uint32_t g_stream_ring_w;
static uint32_t g_stream_ring_drops;
static pthread_mutex_t g_stream_ring_mtx = PTHREAD_MUTEX_INITIALIZER;

void fnirs_stream_ring_reset(void)
{
    pthread_mutex_lock(&g_stream_ring_mtx);
    g_stream_ring_r = 0;
    g_stream_ring_w = 0;
    g_stream_ring_drops = 0;
    pthread_mutex_unlock(&g_stream_ring_mtx);
}

static void fnirs_stream_ring_push(int16_t v)
{
    uint32_t next;

    pthread_mutex_lock(&g_stream_ring_mtx);
    next = (g_stream_ring_w + 1) % FNIRS_STREAM_RING_CAP;
    if (next != g_stream_ring_r) {
        g_stream_ring[g_stream_ring_w] = v;
        g_stream_ring_w = next;
    } else {
        g_stream_ring_drops++;
        if ((g_stream_ring_drops % 200) == 1)
            WLOGW("fnirs_stream ring full, drops=%u\r\n", g_stream_ring_drops);
    }
    pthread_mutex_unlock(&g_stream_ring_mtx);
}

int fnirs_stream_ring_read(int16_t *out, int max)
{
    int n = 0;

    if (!out || max <= 0)
        return 0;

    pthread_mutex_lock(&g_stream_ring_mtx);
    while (n < max && g_stream_ring_r != g_stream_ring_w) {
        out[n++] = g_stream_ring[g_stream_ring_r];
        g_stream_ring_r = (g_stream_ring_r + 1) % FNIRS_STREAM_RING_CAP;
    }
    pthread_mutex_unlock(&g_stream_ring_mtx);
    return n;
}
#endif

static int32_t fnirs_stream_mkfifo_path(const char *path)
{
    struct stat st;

    if (stat(path, &st) == 0) {
        if (!S_ISFIFO(st.st_mode))
            unlink(path);
        else
            return 0;
    }

    if (mkfifo(path, 0666) != 0 && errno != EEXIST) {
        WLOGE("mkfifo %s failed: %s\r\n", path, strerror(errno));
        return -1;
    }

    return 0;
}

static void fnirs_stream_open_writer(void)
{
#ifdef FNIRS_EV_IO
    int i;

    if (g_data_fd >= 0)
        return;

    /*
     * O_RDWR on the writer side opens without a reader; my-server then
     * connects with O_RDONLY.  (O_RDWR reader + O_WRONLY writer on MIPS
     * wrote ok but read() stayed empty; dual O_RDWR loops back locally.)
     */
    for (i = 0; i < 500; i++) {
        g_data_fd = open(FNIRS_STREAM_DATA_PATH, O_RDWR | O_NONBLOCK);
        if (g_data_fd >= 0) {
            static int connected;

            if (!connected++) {
                WLOGI("fnirs_stream writer connected to fifo\r\n");
            }
            return;
        }
        if (errno != ENXIO)
            break;
        usleep(10000);
    }
#else
    if (g_data_fd >= 0)
        return;

    g_data_fd = open(FNIRS_STREAM_DATA_PATH, O_WRONLY | O_NONBLOCK);
#endif
    if (g_data_fd < 0 && errno != ENXIO) {
        static int warned;

        if (!warned++) {
            WLOGW("fnirs_stream writer open: %s\r\n", strerror(errno));
        }
    }
}

static void fnirs_stream_push_sample(int16_t v)
{
#ifdef FNIRS_EMBEDDED
    if (!g_stream_active && !fnirs_ble_ipc_stream_active())
        return;

    fnirs_stream_ring_push(v);
    return;
#endif

    uint8_t out[2];
    ssize_t n;

    if (!g_stream_active && !fnirs_ble_ipc_stream_active())
        return;

    fnirs_stream_open_writer();
    if (g_data_fd < 0)
        return;

    out[0] = (uint8_t)(v & 0xff);
    out[1] = (uint8_t)((v >> 8) & 0xff);

    n = write(g_data_fd, out, sizeof(out));
    if (n == (ssize_t)sizeof(out)) {
#ifdef FNIRS_EV_IO
        static int first_write;

        if (!first_write++) {
            WLOGI("fnirs_stream first sample written to fifo\r\n");
        }
#endif
        return;
    }

    if (errno == ENXIO || errno == EPIPE) {
        close(g_data_fd);
        g_data_fd = -1;
        return;
    }

    if (errno != EAGAIN && errno != EWOULDBLOCK)
        WLOGW("fnirs_stream data write: %s\r\n", strerror(errno));
}

static void fnirs_stream_push_cb(int16_t v, void *ctx)
{
    (void)ctx;
    fnirs_stream_push_sample(v);
}

void fnirs_stream_ctl_local_start(void)
{
    g_stream_active = 1;
}

void fnirs_stream_ctl_local_stop(void)
{
    g_stream_active = 0;
#ifdef FNIRS_EMBEDDED
    fnirs_stream_ring_reset();
#elif defined(FNIRS_EV_IO)
    if (g_data_fd >= 0) {
        close(g_data_fd);
        g_data_fd = -1;
    }
#endif
}

void fnirs_stream_arm_writer(void)
{
#if defined(FNIRS_EV_IO) && !defined(FNIRS_EMBEDDED)
    fnirs_stream_open_writer();
#endif
}

int32_t fnirs_stream_init(void)
{
#ifdef FNIRS_EMBEDDED
    g_data_fd = -1;
    g_stream_active = 0;
    fnirs_stream_ring_reset();
    WLOGI("fnirs_stream embedded ring ready\r\n");
    return 0;
#else
    if (fnirs_stream_mkfifo_path(FNIRS_STREAM_DATA_PATH) != 0)
        return -1;

    g_data_fd = -1;
#ifndef FNIRS_EV_IO
    g_data_fd = open(FNIRS_STREAM_DATA_PATH, O_WRONLY | O_NONBLOCK);
    if (g_data_fd < 0)
        WLOGI("data fifo not connected yet (%s)\r\n", strerror(errno));
#endif

    g_stream_active = 0;
    WLOGI("fnirs_stream data fifo ready\r\n");
    return 0;
#endif
}

void fnirs_stream_exit(void)
{
    g_stream_active = 0;

    if (g_data_fd >= 0) {
        close(g_data_fd);
        g_data_fd = -1;
    }
}

void fnirs_stream_push_frame(
    uint32_t spdata[FNODE_SAMPLE_SRC_MAX][FNODE_SAMPLE_DET_MAX],
    fnode_desc_t desc[FNODE_NID_MAX])
{
    fnirs_ble_stream_cfg_t cfg;

    if (!g_stream_active && !fnirs_ble_ipc_stream_active())
        return;

#ifndef FNIRS_EMBEDDED
    fnirs_stream_open_writer();
#endif

    fnirs_ble_ipc_get_stream_cfg(&cfg);
    if (cfg.count == 0) {
#ifdef FNIRS_EV_IO
        static int warned;

        if (!warned++)
            WLOGW("fnirs_stream push_frame: no stream channels\r\n");
#endif
        return;
    }

#ifdef FNIRS_EV_IO
    {
        static uint32_t frames;

        if ((++frames % 80) == 1)
            WLOGI("fnirs_stream push_frame #%u ch=%u fd=%d\r\n",
                  frames, cfg.count, g_data_fd);
    }
#endif

    fnode_stream_push_channels(desc, spdata, cfg.ch, cfg.count,
                               fnirs_stream_push_cb, NULL);
}
