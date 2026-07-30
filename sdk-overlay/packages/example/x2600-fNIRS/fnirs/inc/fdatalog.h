#ifndef _FDATALOG_H_
#define _FDATALOG_H_

#include <stdint.h>
#include <pthread.h>
#include "fnode.h"
#include "my_interface.h"

#ifdef __cplusplus
extern "C" {
#endif

// ========================================
// 用户配置区
// ========================================



typedef enum {
    FDATALOG_SUCCESS = 0,
    FDATALOG_FAILED = -1,
}fdatalog_errcode_t;

typedef enum {
    FDATALOG_SYNC_NONE = 0,
    FDATALOG_SYNC_ACTIVE = 1,
    FDATALOG_SYNC_FINALIZED = 2,
    FDATALOG_SYNC_ERROR = 3,
} fdatalog_sync_state_t;


#define FDATALOG_PKT_BUFSIZE        100

#define FDATALOG_RWBUFF_SIZE        (0x10000 + 16)

typedef struct {
    uint8_t nodes;
    uint8_t spdata[FNODE_TIME_STAMP_SIZE
        + FNODE_NID_MAX * FNODE_NID_MAX * FNODE_DATA_SIZE
		+ FNODE_NID_MAX * sizeof(fnode_sensor_t)]; // 内存队列最大容量
    uint32_t size; // spdata 中的有效字节数；落盘时仅写这些字节
}fdatalog_pkt_t;



// 2. 时区偏移定义 (杭州位于东八区 UTC+8)
// 如果系统时间已经是 localtime (带时区)，则无需偏移；如果是 UTC，则需 +8。
// 这里假设系统使用 UTC，为了生成正确的“杭州时间”文件名，我们加 8 小时。
#define TIMEZONE_OFFSET_SEC (8 * 3600) 

// 3. 文件名最大长度
#define FDATOLOG_FILENAME_MAX 64

#define FDATALOG_LED_ARRAY_MAGIC0   0xBB
#define FDATALOG_LED_ARRAY_MAGIC1   0x66
#define FDATALOG_LED_ARRAY_MAX      36

// ========================================
// 接口函数声明
// ========================================

/**
 * @brief 初始化数据记录器 (自动基于当前时间生成文件名)
 * 
 * @param path_prefix 文件存储的基础路径 (例如: "/mnt/emmc/")
 * @return int32_t 0 成功, -1 失败
 * 
 * @note 生成的文件名格式: YYYYMMDD_HHMMSS_Hangzhou.bin
 *       例如: 20260604_153022_Hangzhou.bin
 */
int32_t fdatalog_init(const char* path_prefix);

/**
 * @brief 接收数据接口 (生产者调用)
 * 
 * @param data 采样数据指针
 * @param len  数据长度
 * @return int32_t 0 成功, -1 失败(缓冲区满)
 */
int32_t fdatalog_ringbuf_enqueue(uint8_t *spdata, uint32_t size, uint8_t nodes);

/**
 * @brief 通知结束采样 (生产者调用)
 * 
 * @note 调用此接口后，写入线程在处理完剩余数据后会自动退出。
 *       内部会触发文件关闭和资源释放。
 */
void fdatalog_exit(void);
/**
 * Start a new recording. Returns only after the new file is open and ready.
 */
int32_t fdatalog_start(void);

/**
 * Finalize the current recording.
 *
 * Success is the board-to-phone download barrier: all queued acquisition
 * frames have been written, the file has been synced, and its fd is closed.
 */
int32_t fdatalog_stop(void);

/**
 * Stop accepting new frames immediately and let the writer thread drain,
 * fsync, and close the recording in the background.
 */
int32_t fdatalog_stop_async(void);

/**
 * Return the most recently finalized recording path.
 *
 * An active or not-yet-synced file is deliberately not exposed for download.
 */
int32_t fdatalog_get_latest_path(char *out, uint32_t out_size);

/** Current recording file size (bytes) and frame counter while recording. */
int32_t fdatalog_get_recording_stat(uint32_t *bytes_out, uint32_t *frames_out);

/**
 * Snapshot the file currently being recorded/synchronized.
 *
 * crc32_out is authoritative only in FDATALOG_SYNC_FINALIZED. The path stays
 * stable from START through FINALIZED so the BLE tail reader can follow the
 * growing file and finish the same recording after SAMPLE_OFF.
 */
int32_t fdatalog_get_sync_info(char *path_out, uint32_t path_size,
                               uint32_t *bytes_out, uint32_t *crc32_out,
                               fdatalog_sync_state_t *state_out);

/**
 * @brief 采样开始前写入一次发光序列元数据
 *
 * 文件内格式（跟在 8 字节帧头后）:
 *   0xBB 0x66 | led_count(1B) | from_host(1B) | led_count * 4B
 *   每项 4 字节: node_id, node_led_id, power_735, power_850
 */
int32_t fdatalog_write_led_array(const uint8_t *entries, uint8_t led_count, uint8_t from_host);

#ifdef FNIRS_EV_IO
void fdatalog_ev_poll(void);
#endif

#ifdef __cplusplus
}
#endif

#endif // _FDATOLOG_H_
