#include "fdatalog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <sys/uio.h>

#define FDATALOG_FINALIZE_TIMEOUT_MS 8000
#define FDATALOG_ASYNC_WAIT_TIMEOUT_MS 20000
#define FDATALOG_FINALIZE_IDLE       0
#define FDATALOG_FINALIZE_REQUESTED  1
#define FDATALOG_FINALIZE_ACTIVE     2
#define FDATALOG_LAST_DIR            "/app_data/ble_tx"
#define FDATALOG_LAST_MARKER         FDATALOG_LAST_DIR "/.last"
#define FDATALOG_LAST_MARKER_NEW     FDATALOG_LAST_DIR "/.last.new"

// ================== 日志宏适配 ==================
#ifndef WLOGW
#define WLOGW(fmt, ...) printf("[FDATOLOG WARN] " fmt, ##__VA_ARGS__)
#endif
#ifndef WLOGI
#define WLOGI(fmt, ...) printf("[FDATOLOG INFO] " fmt, ##__VA_ARGS__)
#endif

// ================== 私有数据结构 ==================
typedef struct {
    uint32_t size;            
    fdatalog_pkt_t *pkt;      
    volatile uint32_t front;  
    volatile uint32_t tail;   
    
    char dir_path[256];       // 【新增】保存存储目录路径
    char filepath[FDATOLOG_FILENAME_MAX]; 
    int32_t fd;               
    pthread_t tid;            
    volatile uint32_t run;    
    volatile uint32_t is_recording; 
    volatile uint32_t accepting;
    volatile uint32_t enqueue_active;
    volatile uint32_t writer_busy;
    volatile uint32_t finalize_state;
    volatile int32_t finalize_result;
    volatile uint32_t data_counter;   
    volatile int32_t record_error;
    uint32_t file_crc32;
    uint8_t stop_signal;
    uint8_t finalized;
    pthread_mutex_t file_mtx;
#ifdef FNIRS_EV_IO
    uint8_t ev_mode;
#endif
} fdatalog_obj_t;

// 全局单例
static fdatalog_obj_t g_fdatalog_obj = {0};

// ================== 私有函数声明 ==================
static void* fdatalog_task(void *args);
static int32_t fdatalog_write_to_file(fdatalog_obj_t *obj, const uint8_t *data, uint32_t len);
static void fdatalog_generate_filename(const char *dir, char *out,
                                       uint32_t out_size, uint32_t suffix);
static int32_t fdatalog_finalize(fdatalog_obj_t *obj);
static int32_t fdatalog_wait_async_finalize(fdatalog_obj_t *obj);

static uint32_t fdatalog_atomic_load(volatile uint32_t *value)
{
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static void fdatalog_atomic_store(volatile uint32_t *value, uint32_t new_value)
{
    __atomic_store_n(value, new_value, __ATOMIC_RELEASE);
}

static void fdatalog_set_record_error(fdatalog_obj_t *obj, int32_t error)
{
    int32_t expected = 0;

    if (error >= 0)
        error = -EIO;
    (void)__atomic_compare_exchange_n(&obj->record_error, &expected, error,
                                      0, __ATOMIC_ACQ_REL,
                                      __ATOMIC_ACQUIRE);
}

static void fdatalog_publish_last_marker(const char *path)
{
    const char *base;
    size_t len;
    int fd;

    if (!path || !path[0])
        return;

    base = strrchr(path, '/');
    base = base ? base + 1 : path;
    len = strlen(base);
    if (len == 0)
        return;

    (void)mkdir(FDATALOG_LAST_DIR, 0755);
    fd = open(FDATALOG_LAST_MARKER_NEW,
              O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return;

    if (write(fd, base, len) != (ssize_t)len ||
        write(fd, "\n", 1) != 1 ||
        fsync(fd) != 0) {
        close(fd);
        unlink(FDATALOG_LAST_MARKER_NEW);
        return;
    }
    if (close(fd) != 0) {
        unlink(FDATALOG_LAST_MARKER_NEW);
        return;
    }
    if (rename(FDATALOG_LAST_MARKER_NEW, FDATALOG_LAST_MARKER) != 0) {
        unlink(FDATALOG_LAST_MARKER_NEW);
        return;
    }

    WLOGI("last recording marker: %s\r\n", base);
}

static uint32_t fdatalog_crc32_update(uint32_t crc, const uint8_t *buf,
                                     uint32_t len)
{
    uint32_t i;
    int bit;

    crc = ~crc;
    for (i = 0; i < len; i++) {
        crc ^= buf[i];
        for (bit = 0; bit < 8; bit++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
    }
    return ~crc;
}

// ================== 接口实现 ==================
int32_t fdatalog_init(const char* path_prefix) {
    fdatalog_obj_t *obj = &g_fdatalog_obj;

    WLOGW("Start fdatalog_init()\r\n");
    if (path_prefix == NULL) {
        path_prefix = "/app_data";
        WLOGI("No path provided, using default path: %s\n", path_prefix);
    }

    memset(obj, 0, sizeof(fdatalog_obj_t));
    obj->run = 1;
    obj->is_recording = 0; 
    obj->accepting = 0;
    obj->stop_signal = 0;
    obj->fd = -1;

    // 【关键修改】保存目录路径，供后续 start 时使用
    strncpy(obj->dir_path, path_prefix, sizeof(obj->dir_path) - 1);

    mkdir(path_prefix, 0755);

    pthread_mutex_init(&obj->file_mtx, NULL);

    obj->pkt = (fdatalog_pkt_t *)malloc(sizeof(fdatalog_pkt_t) * FDATALOG_PKT_BUFSIZE);
    if (NULL == obj->pkt) {
        WLOGW("Failed to allocate pkt buffer\r\n");
        obj->run = 0;
        pthread_mutex_destroy(&obj->file_mtx);
        return -ENOMEM;
    }

    // 初始化时不再打开文件，等待第一次 fdatalog_start() 被调用

#ifdef FNIRS_EV_IO
    obj->ev_mode = 1;
    WLOGI("fdatalog_init: event mode with background writer\r\n");
#endif

    // 启动写入线程
    int ret = pthread_create(&obj->tid, NULL, fdatalog_task, obj);
    if (ret != 0) {
        WLOGW("Failed to create writer thread\n");
        obj->run = 0;
        free(obj->pkt);
        obj->pkt = NULL;
        pthread_mutex_destroy(&obj->file_mtx);
        return -1;
    }

    pthread_setname_np(obj->tid, "fdatalog_wr");
    WLOGW("fdatalog_init() is started. Waiting for fdatalog_start()...\r\n");
    return 0;
}





// 生产者接口：入队数据
int32_t fdatalog_ringbuf_enqueue(uint8_t *spdata, uint32_t size, uint8_t nodes) {
    fdatalog_obj_t *obj = &g_fdatalog_obj;
    uint32_t tail;
    uint32_t front;
    uint32_t next_tail;

    if (!spdata || size > sizeof(obj->pkt[0].spdata)) {
        WLOGW("Invalid fdatalog frame: data=%p size=%u max=%zu\r\n",
              spdata, size, sizeof(obj->pkt[0].spdata));
        if (fdatalog_atomic_load(&obj->accepting))
            fdatalog_set_record_error(obj, -EINVAL);
        return -EINVAL;
    }

    /*
     * Sampling can still deliver a callback while SAMPLE_OFF is taking
     * effect. accepting is closed before finalization; enqueue_active lets
     * the finalizer wait for an enqueue that was already in flight.
     */
    if (!fdatalog_atomic_load(&obj->accepting))
        return 0;

    __atomic_add_fetch(&obj->enqueue_active, 1, __ATOMIC_ACQ_REL);
    if (!fdatalog_atomic_load(&obj->accepting)) {
        __atomic_sub_fetch(&obj->enqueue_active, 1, __ATOMIC_ACQ_REL);
        return 0;
    }

    tail = fdatalog_atomic_load(&obj->tail);
    front = fdatalog_atomic_load(&obj->front);
    next_tail = (tail + 1) % FDATALOG_PKT_BUFSIZE;

    // 判满
    if (next_tail == front) {
        __atomic_sub_fetch(&obj->enqueue_active, 1, __ATOMIC_ACQ_REL);
        fdatalog_set_record_error(obj, -ENOBUFS);
        WLOGW("fdatalog buf full\r\n");
        return -1;
    }
    
    fdatalog_pkt_t *pkt = &obj->pkt[tail];
    pkt->nodes = nodes;
    pkt->size = size;
    memcpy(pkt->spdata, spdata, size);

    //打印调试信息
    //WLOGW("enqueue [%d]B, at [%d]\r\n", size, tail);

    // 更新写指针（SPSC下，只有生产者写 tail，无需加锁）
    fdatalog_atomic_store(&obj->tail, next_tail);
    __atomic_sub_fetch(&obj->enqueue_active, 1, __ATOMIC_ACQ_REL);
    return 0;
}




// 消费者接口：出队数据
static fdatalog_pkt_t* fdatalog_ringbuf_dequeue(fdatalog_obj_t *obj) {
    fdatalog_pkt_t *pkt = NULL;
    uint32_t front = fdatalog_atomic_load(&obj->front);
    uint32_t tail = fdatalog_atomic_load(&obj->tail);

    if (front == tail) {
        return NULL; // 空
    }
    
    pkt = &obj->pkt[front];
    fdatalog_atomic_store(&obj->front,
                          (front + 1) % FDATALOG_PKT_BUFSIZE);
    return pkt;
}

// 通知结束采样
void fdatalog_exit(void) {
    fdatalog_obj_t *obj = &g_fdatalog_obj;

    if (!obj->run)
        return;

    (void)fdatalog_stop();
    obj->stop_signal = 1;
    pthread_join(obj->tid, NULL);
    pthread_mutex_destroy(&obj->file_mtx);
    free(obj->pkt);
    obj->pkt = NULL;
    WLOGI("Finish signal sent. Flushing remaining data...\n");
}



// ================== 私有函数实现 ==================

// 生成文件名
static void fdatalog_generate_filename(const char *dir, char *out,
                                       uint32_t out_size, uint32_t suffix) {
    time_t now = time(NULL);
    now += TIMEZONE_OFFSET_SEC; 
    struct tm *tm_info = gmtime(&now); 

    char time_str[32];
    strftime(time_str, sizeof(time_str), "%Y%m%d_%H%M%S", tm_info);

    if (dir[strlen(dir) - 1] != '/') {
        if (suffix == 0)
            snprintf(out, out_size, "%s/%s_Hangzhou.bin", dir, time_str);
        else
            snprintf(out, out_size, "%s/%s_%03u_Hangzhou.bin",
                     dir, time_str, suffix);
    } else {
        if (suffix == 0)
            snprintf(out, out_size, "%s%s_Hangzhou.bin", dir, time_str);
        else
            snprintf(out, out_size, "%s%s_%03u_Hangzhou.bin",
                     dir, time_str, suffix);
    }
}






/**
 * @brief 写入线程主循环 (核心修复：补全了 while 循环和阻塞等待逻辑)
 */
#ifdef FNIRS_EV_IO
void fdatalog_ev_poll(void)
{
    /* File writes run on fdatalog_wr so the event loop remains responsive. */
}
#endif

static void* fdatalog_task(void *args) {
    fdatalog_obj_t *obj = (fdatalog_obj_t*)args;

    while (obj->run) {
        fdatalog_pkt_t *pkt;

        if (fdatalog_atomic_load(&obj->front) ==
            fdatalog_atomic_load(&obj->tail)) {
            uint32_t expected = FDATALOG_FINALIZE_REQUESTED;

            /*
             * SAMPLE_OFF only queues this request. Run the potentially slow
             * drain/fsync/close barrier here so BLE/UV remains responsive and
             * LIVE FILE can continue transferring the tail.
             */
            /*
             * A producer that entered just before accepting was closed may
             * still publish one final queue item. Only claim finalization
             * after that producer has left and a second queue check is empty;
             * otherwise this writer would wait for data that only it can
             * drain.
             */
            if (fdatalog_atomic_load(&obj->enqueue_active) == 0 &&
                fdatalog_atomic_load(&obj->front) ==
                    fdatalog_atomic_load(&obj->tail) &&
                __atomic_compare_exchange_n(
                    &obj->finalize_state, &expected,
                    FDATALOG_FINALIZE_ACTIVE, 0,
                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                int32_t rc = fdatalog_finalize(obj);

                __atomic_store_n(&obj->finalize_result, rc,
                                 __ATOMIC_RELEASE);
                fdatalog_atomic_store(&obj->finalize_state,
                                      FDATALOG_FINALIZE_IDLE);
                continue;
            }

            if (obj->stop_signal) {
                WLOGW("fdatalog_task while is out\n");
                break;
            }
            usleep(1000);
            continue;
        }

        fdatalog_atomic_store(&obj->writer_busy, 1);
        pkt = fdatalog_ringbuf_dequeue(obj);

        if (pkt != NULL) {
            // accepting=0 时仍需排空停止前已入队的数据。
            if (fdatalog_atomic_load(&obj->is_recording)) {
                /*
                 * fdatalog_pkt_t is the fixed-size in-memory queue slot.
                 * Persist only the acquired payload; serializing the whole
                 * slot pads a 912-byte three-node frame to about 14 KB.
                 */
                fdatalog_write_to_file(obj, pkt->spdata, pkt->size);
            }
            fdatalog_atomic_store(&obj->writer_busy, 0);
        } else {
            fdatalog_atomic_store(&obj->writer_busy, 0);
            usleep(1000);
        }
    }

    // 退出前把缓冲区里可能残留的数据写完
    while (fdatalog_atomic_load(&obj->front) !=
           fdatalog_atomic_load(&obj->tail)) {
        fdatalog_pkt_t *pkt = fdatalog_ringbuf_dequeue(obj);
        if (pkt && fdatalog_atomic_load(&obj->is_recording))
            fdatalog_write_to_file(obj, pkt->spdata, pkt->size);
    }

    // 退出清理
    pthread_mutex_lock(&obj->file_mtx);
    if (obj->fd >= 0) {
        fsync(obj->fd);
        close(obj->fd);
        obj->fd = -1;
    }
    pthread_mutex_unlock(&obj->file_mtx);
    
    obj->run = 0;
    WLOGW("fdatalog_task exit\n");
    pthread_exit(NULL);
}


/**
 * @brief 开始记录数据
 */
/**
 * @brief 开始记录数据 (每次调用都会生成一个全新的文件)
 */
int32_t fdatalog_start(void) {
    fdatalog_obj_t *obj = &g_fdatalog_obj;
    int32_t rc;
    uint32_t suffix;
    
    if (!obj->run) {
        WLOGW("Writer thread is not running, cannot start recording!\n");
        return -ESHUTDOWN;
    }

    rc = fdatalog_wait_async_finalize(obj);
    if (rc != 0)
        return rc;

    /*
     * A repeated START finalizes the previous session first. This prevents a
     * writer that already dequeued an old frame from writing it into the new
     * file after the fd is switched.
     */
    if (fdatalog_atomic_load(&obj->is_recording) || obj->fd >= 0) {
        rc = fdatalog_finalize(obj);
        if (rc != 0)
            return rc;
    }

    pthread_mutex_lock(&obj->file_mtx);

    /*
     * Data is synced once at the download barrier. O_DSYNC on every frame
     * makes queue drain latency unpredictable and can overflow the ring.
     * O_EXCL plus a suffix keeps two STARTs in the same second from
     * truncating the previous completed recording.
     */
    obj->fd = -1;
    for (suffix = 0; suffix < 1000; suffix++) {
        fdatalog_generate_filename(obj->dir_path, obj->filepath,
                                   sizeof(obj->filepath), suffix);
        obj->fd = open(obj->filepath, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (obj->fd >= 0 || errno != EEXIST)
            break;
    }
    if (obj->fd < 0) {
        int saved_errno = errno;

        WLOGW("Failed to open new file: %s (err: %s)\n", obj->filepath, strerror(errno));
        obj->filepath[0] = '\0';
        pthread_mutex_unlock(&obj->file_mtx);
        return -saved_errno;
    }

    fdatalog_atomic_store(&obj->front, 0);
    fdatalog_atomic_store(&obj->tail, 0);
    obj->data_counter = 0;
    obj->record_error = 0;
    obj->file_crc32 = 0;
    obj->finalized = 0;
    fdatalog_atomic_store(&obj->is_recording, 1);
    fdatalog_atomic_store(&obj->accepting, 1);

    WLOGI("Data recording STARTED. New file: %s\n", obj->filepath);
    pthread_mutex_unlock(&obj->file_mtx);
    return 0;
}




/**
 * @brief 停止记录数据
 */
int32_t fdatalog_stop(void) {
    fdatalog_obj_t *obj = &g_fdatalog_obj;

    if (fdatalog_atomic_load(&obj->finalize_state) !=
        FDATALOG_FINALIZE_IDLE)
        return fdatalog_wait_async_finalize(obj);

    return fdatalog_finalize(obj);
}

int32_t fdatalog_stop_async(void)
{
    fdatalog_obj_t *obj = &g_fdatalog_obj;
    uint32_t expected = FDATALOG_FINALIZE_IDLE;

    if (!obj->run)
        return -ESHUTDOWN;

    if (!fdatalog_atomic_load(&obj->is_recording) && obj->fd < 0) {
        int32_t previous_error =
            __atomic_load_n(&obj->record_error, __ATOMIC_ACQUIRE);

        if (obj->finalized)
            return 0;
        return previous_error ? previous_error : -ENOENT;
    }

    /* Freeze the producer before publishing the background finalize request. */
    fdatalog_atomic_store(&obj->accepting, 0);
    __atomic_store_n(&obj->finalize_result, -EINPROGRESS,
                     __ATOMIC_RELEASE);
    if (!__atomic_compare_exchange_n(
            &obj->finalize_state, &expected,
            FDATALOG_FINALIZE_REQUESTED, 0,
            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        if (expected == FDATALOG_FINALIZE_REQUESTED ||
            expected == FDATALOG_FINALIZE_ACTIVE)
            return 0;
        return -EBUSY;
    }

    WLOGI("Recording finalize queued\r\n");
    return 0;
}

static int32_t fdatalog_wait_async_finalize(fdatalog_obj_t *obj)
{
    int waited_ms = 0;
    int had_pending;

    had_pending = fdatalog_atomic_load(&obj->finalize_state) !=
                  FDATALOG_FINALIZE_IDLE;
    while (fdatalog_atomic_load(&obj->finalize_state) !=
               FDATALOG_FINALIZE_IDLE &&
           waited_ms < FDATALOG_ASYNC_WAIT_TIMEOUT_MS) {
        usleep(1000);
        waited_ms++;
    }

    if (fdatalog_atomic_load(&obj->finalize_state) !=
        FDATALOG_FINALIZE_IDLE)
        return -ETIMEDOUT;

    if (!had_pending)
        return 0;
    return __atomic_load_n(&obj->finalize_result, __ATOMIC_ACQUIRE);
}

static int32_t fdatalog_finalize(fdatalog_obj_t *obj)
{
    int waited_ms = 0;
    int saved_errno = 0;
    int32_t record_error;
    uint32_t frames;
    off_t bytes = 0;

    if (!obj || !obj->run)
        return -ESHUTDOWN;

    if (!fdatalog_atomic_load(&obj->is_recording) && obj->fd < 0) {
        int32_t previous_error =
            __atomic_load_n(&obj->record_error, __ATOMIC_ACQUIRE);

        if (obj->finalized)
            return 0;
        return previous_error ? previous_error : -ENOENT;
    }

    /* Close the producer gate, then drain everything accepted before it. */
    fdatalog_atomic_store(&obj->accepting, 0);

    while (fdatalog_atomic_load(&obj->enqueue_active) != 0 &&
           waited_ms < FDATALOG_FINALIZE_TIMEOUT_MS) {
        usleep(1000);
        waited_ms++;
    }

    while ((fdatalog_atomic_load(&obj->front) !=
            fdatalog_atomic_load(&obj->tail) ||
            fdatalog_atomic_load(&obj->writer_busy)) &&
           waited_ms < FDATALOG_FINALIZE_TIMEOUT_MS) {
        usleep(1000);
        waited_ms++;
    }

    if (fdatalog_atomic_load(&obj->enqueue_active) != 0 ||
        fdatalog_atomic_load(&obj->front) !=
        fdatalog_atomic_load(&obj->tail) ||
        fdatalog_atomic_load(&obj->writer_busy)) {
        WLOGW("Recording finalize timeout: queued=%u writer=%u enqueue=%u\r\n",
              (obj->tail + FDATALOG_PKT_BUFSIZE - obj->front) %
                  FDATALOG_PKT_BUFSIZE,
              fdatalog_atomic_load(&obj->writer_busy),
              fdatalog_atomic_load(&obj->enqueue_active));
        return -ETIMEDOUT;
    }

    fdatalog_atomic_store(&obj->is_recording, 0);
    frames = obj->data_counter;

    /*
     * fsync can take hundreds of milliseconds on a busy flash device. Do
     * not hold file_mtx while it runs: the UV GATT worker queries growing
     * file progress through fdatalog_get_sync_info(), and blocking that
     * worker here can make the BLE connection time out during SAMPLE_OFF.
     *
     * The producer gate is closed and the writer queue is empty, so no more
     * writes can reach this fd. Keep the short fstat/close transition under
     * file_mtx so readers never race a descriptor being closed.
     */
    if (obj->fd >= 0 && fsync(obj->fd) != 0)
        saved_errno = errno;

    pthread_mutex_lock(&obj->file_mtx);
    if (obj->fd >= 0) {
        struct stat st;

        if (fstat(obj->fd, &st) == 0)
            bytes = st.st_size;
        else if (!saved_errno)
            saved_errno = errno;
        if (close(obj->fd) != 0 && !saved_errno)
            saved_errno = errno;
        obj->fd = -1;
    }

    record_error = __atomic_load_n(&obj->record_error, __ATOMIC_ACQUIRE);
    if (record_error != 0 && !saved_errno)
        saved_errno = -record_error;

    obj->finalized = saved_errno == 0 && obj->filepath[0] != '\0';
    pthread_mutex_unlock(&obj->file_mtx);

    if (saved_errno) {
        WLOGW("Recording finalize failed: %s\r\n", strerror(saved_errno));
        return -saved_errno;
    }

    WLOGI("Data recording FINALIZED: %s bytes=%lld frames=%u\r\n",
          obj->filepath, (long long)bytes, frames);
    fdatalog_publish_last_marker(obj->filepath);
    return 0;
}

int32_t fdatalog_get_latest_path(char *out, uint32_t out_size) {
    fdatalog_obj_t *obj = &g_fdatalog_obj;
    int32_t rc = 0;

    if (!out || out_size == 0)
        return -1;

    pthread_mutex_lock(&obj->file_mtx);
    if (!obj->finalized || obj->filepath[0] == '\0') {
        rc = -1;
        goto out;
    }

    strncpy(out, obj->filepath, out_size - 1);
    out[out_size - 1] = '\0';
out:
    pthread_mutex_unlock(&obj->file_mtx);
    return rc;
}

int32_t fdatalog_get_recording_stat(uint32_t *bytes_out, uint32_t *frames_out)
{
    fdatalog_obj_t *obj = &g_fdatalog_obj;
    struct stat st;

    if (!fdatalog_atomic_load(&obj->is_recording) ||
        obj->fd < 0 || obj->filepath[0] == '\0')
        return -1;

    pthread_mutex_lock(&obj->file_mtx);
    if (obj->fd < 0 || fstat(obj->fd, &st) != 0 ||
        !S_ISREG(st.st_mode)) {
        pthread_mutex_unlock(&obj->file_mtx);
        return -1;
    }

    if (bytes_out)
        *bytes_out = (uint32_t)st.st_size;
    if (frames_out)
        *frames_out = obj->data_counter;
    pthread_mutex_unlock(&obj->file_mtx);
    return 0;
}

int32_t fdatalog_get_sync_info(char *path_out, uint32_t path_size,
                               uint32_t *bytes_out, uint32_t *crc32_out,
                               fdatalog_sync_state_t *state_out)
{
    fdatalog_obj_t *obj = &g_fdatalog_obj;
    struct stat st;
    fdatalog_sync_state_t state = FDATALOG_SYNC_NONE;
    int32_t rc = 0;

    if (!path_out || path_size == 0 || !bytes_out || !crc32_out || !state_out)
        return -EINVAL;

    pthread_mutex_lock(&obj->file_mtx);
    if (obj->filepath[0] == '\0') {
        rc = -ENOENT;
        goto out;
    }

    if (obj->fd >= 0) {
        if (fstat(obj->fd, &st) != 0) {
            rc = -errno;
            goto out;
        }
    } else if (stat(obj->filepath, &st) != 0) {
        rc = -errno;
        goto out;
    }

    if (obj->finalized)
        state = FDATALOG_SYNC_FINALIZED;
    else if (__atomic_load_n(&obj->record_error, __ATOMIC_ACQUIRE) != 0)
        state = FDATALOG_SYNC_ERROR;
    else if (fdatalog_atomic_load(&obj->is_recording) || obj->fd >= 0)
        state = FDATALOG_SYNC_ACTIVE;
    else
        state = FDATALOG_SYNC_ERROR;

    strncpy(path_out, obj->filepath, path_size - 1);
    path_out[path_size - 1] = '\0';
    *bytes_out = st.st_size > UINT32_MAX ? UINT32_MAX : (uint32_t)st.st_size;
    *crc32_out = obj->file_crc32;
    *state_out = state;
out:
    pthread_mutex_unlock(&obj->file_mtx);
    return rc;
}

int32_t fdatalog_write_led_array(const uint8_t *entries, uint8_t led_count, uint8_t from_host) {
    fdatalog_obj_t *obj = &g_fdatalog_obj;
    uint8_t payload[4 + FDATALOG_LED_ARRAY_MAX * 4];
    uint32_t payload_len;

    if (!fdatalog_atomic_load(&obj->is_recording) || obj->fd < 0) {
        return 0;
    }
    if (entries == NULL || led_count == 0) {
        return -1;
    }

    payload[0] = FDATALOG_LED_ARRAY_MAGIC0;
    payload[1] = FDATALOG_LED_ARRAY_MAGIC1;
    payload[2] = led_count;
    payload[3] = from_host;
    memcpy(payload + 4, entries, led_count * 4);

    payload_len = 4 + led_count * 4;
    WLOGI("LED array metadata: count=%u, from_host=%u, size=%u\n",
          led_count, from_host, payload_len);

    return fdatalog_write_to_file(obj, payload, payload_len);
}

/**
 * @brief 底层文件写入（带自定义包头）
 */
static int32_t fdatalog_write_to_file(fdatalog_obj_t *obj, const uint8_t *data, uint32_t len) {
    pthread_mutex_lock(&obj->file_mtx);
    if (!fdatalog_atomic_load(&obj->is_recording) || obj->fd < 0) {
        pthread_mutex_unlock(&obj->file_mtx);
        return -1;
    }

    // 1. 构建包头结构体
    // 格式：0xAA 0x55 + 4字节计数器 + 0xA5 0x5A
    uint8_t header[8];
    header[0] = 0xAA;
    header[1] = 0x55;
    
    // 将 data_counter 按大端序（或根据你上位机的解析习惯调整字节序）填入
    uint32_t counter = obj->data_counter;
    header[2] = (counter >> 24) & 0xFF;
    header[3] = (counter >> 16) & 0xFF;
    header[4] = (counter >> 8)  & 0xFF;
    header[5] = (counter)       & 0xFF;
    
    header[6] = 0xA5;
    header[7] = 0x5A;

    // 2. 使用 writev 将包头和数据一次性写入文件
    struct iovec iov[2];
    
    // 第一块：包头 (8字节)
    iov[0].iov_base = header;
    iov[0].iov_len = sizeof(header);
    
    // 第二块：原始采样数据
    iov[1].iov_base = (void *)data;
    iov[1].iov_len = len;

    // 期望写入的总长度
    ssize_t expected_len = sizeof(header) + len;
    
    // 执行向量写入
    ssize_t rc = writev(obj->fd, iov, 2);
    
    if (rc != expected_len) {
        int saved_errno = (rc < 0 && errno != 0) ? errno : EIO;

        fdatalog_set_record_error(obj, -saved_errno);
        WLOGW("Write failed! Expected %zd, Got %zd (err: %s)\n",
              expected_len, rc, strerror(saved_errno));
        pthread_mutex_unlock(&obj->file_mtx);
        return -1;
    }

    // 3. 写入成功后，计数器自增
    obj->file_crc32 = fdatalog_crc32_update(obj->file_crc32,
                                            header, sizeof(header));
    obj->file_crc32 = fdatalog_crc32_update(obj->file_crc32, data, len);
    obj->data_counter++;

    pthread_mutex_unlock(&obj->file_mtx);
    return 0;
}
