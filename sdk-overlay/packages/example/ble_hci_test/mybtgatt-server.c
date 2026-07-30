// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2014  Google Inc.
 *
 *
 */

 #ifdef HAVE_CONFIG_H
 #include <config.h>
 #endif
 
 #include <sys/epoll.h>  // 解决 EPOLLIN, EPOLLRDHUP, EPOLLHUP, EPOLLERR 未定义
 #include <signal.h>     // 解决 SIGINT, SIGTERM 未定义
 #include <stdlib.h> // 提供 system() 函数
 
 #define _GNU_SOURCE
 #include <stdio.h>
 #include <stdbool.h>
 #include <stdint.h>
 #include <time.h>
 #include <sys/time.h>
 #include <stdlib.h>
 #include <getopt.h>
 #include <unistd.h>
 #include <errno.h>
 #include <sys/stat.h>
 #include <fcntl.h>
 #include <termios.h>
 #include <string.h>
 #include <math.h>
 #include <dirent.h>
 #include <sys/socket.h>
 #include <sys/un.h>

#ifdef MY_SERVER_EMBEDDED
#include "my_server_embedded.h"
#endif

#ifdef FNIRS_EMBEDDED
#include "fnirs_ble_ipc.h"
#include "fnirs_stream.h"
#include "fdatalog.h"
#endif
 
 #include "lib/bluetooth.h"
 #include "lib/hci.h"
 #include "lib/hci_lib.h"
 #include "lib/l2cap.h"
 #include "lib/uuid.h"
 
 
 #include "src/shared/mainloop.h"
 #include "src/shared/util.h"
 #include "src/shared/att.h"
 #include "src/shared/queue.h"
 #include "src/shared/timeout.h"
 #include "src/shared/gatt-db.h"
 #include "src/shared/gatt-server.h"
 
 
 #define UUID_GAP    0x1800
 #define UUID_GATT   0x1801
 #define UUID_HEART_RATE         0x180d
 #define UUID_HEART_RATE_MSRMT   0x2a37
 #define UUID_HEART_RATE_BODY    0x2a38
 #define UUID_HEART_RATE_CTRL    0x2a39
 
 
 
 #define COLOR_OFF	"\x1B[0m"
 #define COLOR_RED	"\x1B[0;91m"
 #define COLOR_GREEN	"\x1B[0;92m"
 #define COLOR_YELLOW	"\x1B[0;93m"
 #define COLOR_BLUE	"\x1B[0;94m"
 #define COLOR_MAGENTA	"\x1B[0;95m"
 #define COLOR_BOLDGRAY	"\x1B[1;30m"
 #define COLOR_BOLDWHITE	"\x1B[1;37m"
 
 
 static const char test_device_name[] = "TestBLE";
 
 // 自定义服务与特征 UUID (128-bit, Little-Endian)
 static uint8_t custom_svc_uuid_bytes[16] = {
	 0x78, 0x56, 0x34, 0x12, 0x34, 0x12, 0x78, 0x56,
	 0x12, 0x34, 0x56, 0x78, 0x12, 0x34, 0x56, 0x78
 };
 
 static uint8_t custom_data_uuid_bytes[16] = {
	 0x79, 0x56, 0x34, 0x12, 0x34, 0x12, 0x78, 0x56,
	 0x12, 0x34, 0x56, 0x78, 0x12, 0x34, 0x56, 0x78
 };
 
 #define ATT_CID 4
 
 /* Wake blocking accept() on SIGINT/SIGTERM (registered before accept). */
static volatile int g_listen_sk = -1;
static volatile int g_exit_requested = 0;
#ifdef MY_SERVER_EMBEDDED
static volatile int g_session_ended = 0;
#endif
 
 static void accept_interrupt_sig(int signo)
 {
	 (void)signo;
	 g_exit_requested = 1;
	 if (g_listen_sk >= 0) {
		 close(g_listen_sk);
		 g_listen_sk = -1;
	 }
 }
 
 // 精简为纯日志宏，受 verbose 控制，移除 print_prompt 依赖
 #define PRLOG(...) \
	 do { \
		 if (verbose) \
			 printf(__VA_ARGS__); \
	 } while (0)
 
 #ifndef MIN
 #define MIN(a, b) ((a) < (b) ? (a) : (b))
 #endif
 
 static bool verbose = false;
 
 /*
 uint8_t initial_data[]={1,2,3};
 uint16_t initial_len=3;*/
 
 #define MY_SERVER_BUILD_ID "20260728-live-tail-uv-epoll-safe"
#define CUSTOM_DATA_LEN 20
#define FILE_RX_DIR_DEFAULT "/app_data/ble_rx"
#define FILE_TX_DIR_DEFAULT "/app_data/ble_tx"
/* window*delay_ms should stay >= ~100ms so the radio can drain before ACK stall */
#define FILE_TX_FC_WINDOW 14
#define FILE_TX_PACKET_DELAY_MS 6
#define FILE_TX_PACKET_DELAY_SMALL_MTU_MS 10
#define FILE_TX_POST_ACK_DELAY_MS 3
#define FILE_TX_START_DELAY_MS 100
#define FILE_TX_MAX_CHUNK 235
#define FILE_TX_CHUNK_MTU_MARGIN 2
#define FILE_TX_KICK_DELAY_MS 1
#define FILE_RX_DATA_CRC_LEN 2
#define FILE_RX_IO_BUF_SIZE (64 * 1024)
#define FILE_RX_STATUS_NOTIFY_PKTS 8
#define FILE_RX_STATUS_NOTIFY_BYTES (16 * 1024)
 #define BRIDGE_UART_DEFAULT "/dev/ttyS1"
 #define BRIDGE_UART_BAUD 2000000
 #define BRIDGE_TX_BUF_SIZE 8192
 #define BRIDGE_FLUSH_MS 20
 #define BRIDGE_OP_PASSTHROUGH 0x20
 
 #define FILE_RX_ERR_FRAME_CRC 0xE1
 #define FILE_RX_ERR_SEQ       0xE2
 #define FILE_RX_ERR_FRAME_LEN 0xE3
 #define FILE_RX_OP_START 0x01
 #define FILE_RX_OP_DATA  0x02
 #define FILE_RX_OP_END   0x03
 #define FILE_RX_OP_ABORT 0x11

 #define FILE_TX_OP_REQ   0x21
 #define FILE_TX_OP_START 0x22
 #define FILE_TX_OP_DATA  0x23
 #define FILE_TX_OP_END   0x24
 #define FILE_TX_OP_ABORT 0x25
 #define FILE_TX_OP_ACK   0x26
 #define FILE_TX_LAST_REQ 0x27
 #define FILE_TX_LAST_RSP 0x28

 #define FILE_LIVE_OP_REQ   0x29
 #define FILE_LIVE_OP_START 0x2A
 #define FILE_LIVE_OP_DATA  0x2B
 #define FILE_LIVE_OP_FINAL 0x2C
 #define FILE_LIVE_OP_ACK   0x2D
 #define FILE_LIVE_OP_ABORT 0x2E

 #define FILE_LIVE_FINAL_SEALED 0
 #define FILE_LIVE_FINAL_DONE   1
 #define FILE_LIVE_POLL_MS      40
 #define FILE_LIVE_PACKET_DELAY_MS 8
 #define FILE_LIVE_RETRY_MS     240
 #define FILE_LIVE_EOF_RETRY_POLLS \
	 (FILE_LIVE_RETRY_MS / FILE_LIVE_POLL_MS)
 #define FILE_LIVE_REC_ACTIVE    1
 #define FILE_LIVE_REC_FINALIZED 2
 #define FILE_LIVE_REC_ERROR     3

 #define FILE_TX_LAST_MARKER "/app_data/ble_tx/.last"

 #define STREAM_OP_START 0x31
 #define STREAM_OP_DATA  0x32
 #define STREAM_OP_STOP  0x33
 #define STREAM_OP_ABORT 0x34
 #define TIME_OP_SET     0x35
 //#define STREAM_RATE_HZ_DEFAULT 5120
 #define STREAM_RATE_HZ_DEFAULT 6000
 #define STREAM_PERIOD_MS_DEFAULT 12
 #define STREAM_MAX_SAMPLES_PER_PKT 64
 #define STREAM_RATE_HZ_MAX 8000
 #define STREAM_PERIOD_MS_MIN 5
#define FNIRS_STREAM_CTL_PATH   "/tmp/fnirs_stream.ctl"
#define FNIRS_STREAM_DATA_PATH  "/tmp/fnirs_stream.data"
#define FNIRS_STREAM_CTL_START  'S'
#define FNIRS_STREAM_CTL_STOP   'T'
#define FNIRS_STREAM_CTL_ABORT  'A'
#define STREAM_RATE_HZ_REAL     80
#define STREAM_PERIOD_MS_REAL   10
#define FNIRS_BLE_SOCK_PATH     "/tmp/fnirs_ble.sock"
#define FNIRS_BLE_REQ_MAGIC0    0xFB
#define FNIRS_BLE_REQ_MAGIC1    0x10
#define FNIRS_BLE_RSP_MAGIC0    0xFB
#define FNIRS_BLE_RSP_MAGIC1    0x11
#define FNIRS_BLE_CMD_SCAN          0x01
#define FNIRS_BLE_CMD_SAMPLE_ON     0x02
#define FNIRS_BLE_CMD_SAMPLE_OFF    0x03
#define FNIRS_BLE_CMD_SET_GAIN      0x04
#define FNIRS_BLE_CMD_SET_LED_ARRAY 0x05
#define FNIRS_BLE_CMD_SET_STREAM_CH 0x06
#define FNIRS_BLE_CMD_STREAM_START  0x07
#define FNIRS_BLE_CMD_STREAM_STOP   0x08
#define FNIRS_BLE_CMD_HANGZHOU_PATH 0x09
#define FNIRS_BLE_CMD_RECORD_STAT   0x0A
#define FNIRS_BLE_STATUS_OK         0
#define FNIRS_OP_SCAN           0x40
#define FNIRS_OP_SAMPLE_ON      0x41
#define FNIRS_OP_SAMPLE_OFF     0x42
#define FNIRS_OP_GAIN           0x43
#define FNIRS_OP_LED_ARRAY      0x44
#define FNIRS_OP_STREAM_CH      0x45
#define FNIRS_OP_HANGZHOU       0x46
#define FNIRS_OP_RECORD_STAT    0x47
#define FNIRS_OP_RSP            0x48
/* Demo: occasional acquisition pause (set 0 to disable). */
#ifndef STREAM_DEMO_GLITCH
#define STREAM_DEMO_GLITCH 1
#endif
#define STREAM_GLITCH_INTERVAL_SEC 20
#define STREAM_GLITCH_PAUSE_MS 450
 
 #define FILE_RX_STATE_IDLE   0x00
 #define FILE_RX_STATE_ACTIVE 0x01
 #define FILE_RX_STATE_DONE   0x02
 #define FILE_RX_STATE_ERROR  0x03
 
 
 /*
  * struct server: GATT 服务器核心上下文结构体
  */
 struct server {
	 /* 1. 基础连接与数据库句柄 */
	 int fd;
	 struct bt_att *att;
	 struct gatt_db *db;
	 struct bt_gatt_server *gatt;
 
	 /* 2. 设备名称 (GAP 服务强制要求) */
	 uint8_t *device_name;
	 size_t name_len;
 
	 /* 3. GATT Service Changed 状态 */
	 uint16_t gatt_svc_chngd_handle;
	 bool svc_chngd_enabled;
 
	 /* 4. 自定义服务状态 */
	 uint16_t custom_svc_handle;
	 uint16_t custom_data_handle;
	 bool custom_notify_enabled;
	 bool custom_indicate_enabled;
 
	 /* 5. 周期性发送定时器 ID */
	 unsigned int custom_timeout_id;
 
	 /* 6. 外部数据缓冲 (运行时动态分配并填充) */
	 uint8_t *external_data_ptr;
	 size_t external_data_len;
 
	 /* 7. Heart Rate Service 状态 */
	 uint16_t hr_handle;
	 uint16_t hr_msrmt_handle;
	 uint16_t hr_energy_expended;
	 bool hr_visible;
	 bool hr_msrmt_enabled;
	 int hr_ee_count;
	 unsigned int hr_timeout_id;
 
	 /* 8. File RX state (custom characteristic write path) */
	 bool rx_active;
	 FILE *rx_fp;
	 char rx_path[256];
	 uint32_t rx_expected_size;
	 uint32_t rx_received_size;
	 uint32_t rx_expected_crc;
	 uint32_t rx_running_crc;
	 uint32_t rx_next_seq;
	 uint8_t rx_state;
	 uint8_t rx_last_error;
	 uint8_t *rx_io_buf;
	 uint32_t rx_last_status_seq;
	 uint32_t rx_last_status_bytes;

	 /* 9. File TX state (board -> phone via Notify) */
	 bool tx_active;
	 FILE *tx_fp;
	 char tx_name[128];
	 char tx_path[256];
	 uint32_t tx_file_size;
	 uint32_t tx_file_crc;
	 uint32_t tx_sent_size;
	 uint32_t tx_next_seq;
	 unsigned int tx_timer_id;
	 unsigned int tx_kick_id;
	 bool tx_start_pending;
	 bool tx_fc_ready;
	 uint32_t tx_fc_ack_seq;
	 uint8_t tx_start_pkt[256];
	 size_t tx_start_len;

	 /* 10. Growing recording -> phone, concurrent with preview STREAM. */
	 bool live_active;
	 FILE *live_fp;
	 char live_name[128];
	 char live_path[256];
	 uint32_t live_sent_offset;
	 uint32_t live_next_seq;
	 uint32_t live_ack_offset;
	 uint32_t live_ack_seq;
	 uint32_t live_high_offset;
	 uint32_t live_high_seq;
	 uint32_t live_final_size;
	 uint32_t live_final_crc;
	 unsigned int live_eof_polls;
	 unsigned int live_timer_id;
	 unsigned int live_kick_id;
	 bool live_start_pending;
	 bool live_fc_ready;
	 bool live_sealed;
	 bool live_done_sent;
	 uint8_t live_start_pkt[256];
	 size_t live_start_len;

	 /* 11. Live sample stream (board -> phone Notify, sim fNIRS waveform) */
	 bool stream_active;
	 uint32_t stream_seq;
	 uint32_t stream_sample_idx;
	 uint32_t stream_rate_hz;
	 uint16_t stream_period_ms;
	 unsigned int stream_timer_id;
	 FILE *stream_rec_fp;
	 char stream_rec_path[256];
	 uint32_t stream_glitch_next_idx;
	 unsigned int bridge_flush_id;
 };
 
 
 
 
 /*--------------------------------------------------------------------------
  * 2. ATT 连接断开回调 (核心生命周期管理)
  *--------------------------------------------------------------------------*/
 /*
  * att_disconnect_cb: ATT 底层连接断开时的回调函数
  * @err: 错误码，表示断开连接的原因
  * @user_data: 用户数据指针（在此处未使用）
  */
 static void att_disconnect_cb(int err, void *user_data)
 {
	 struct server *server = user_data;
 
	 if (server && server->rx_active && server->rx_fp) {
		 printf("FILE RX disconnect: recv=%u expected=%u path=%s err=%d\n",
				server->rx_received_size, server->rx_expected_size,
				server->rx_path, err);
 
		 if (server->rx_received_size == server->rx_expected_size &&
			 server->rx_running_crc == server->rx_expected_crc) {
			 fflush(server->rx_fp);
			 fclose(server->rx_fp);
			 server->rx_fp = NULL;
			 free(server->rx_io_buf);
			 server->rx_io_buf = NULL;
			 server->rx_active = false;
			 server->rx_state = FILE_RX_STATE_DONE;
			 printf("FILE RX DONE (on disconnect): path=%s size=%u crc=0x%08x\n",
					server->rx_path, server->rx_received_size,
					server->rx_running_crc);
		 }
	 }
	 PRLOG("Device disconnected (err=%d)\n", err);
#ifdef MY_SERVER_EMBEDDED
	 g_session_ended = 1;
#else
	 mainloop_quit();
#endif
 }
 
 
 
 /*--------------------------------------------------------------------------
  * 3. ATT 调试日志回调 (开发调试工具)
  *--------------------------------------------------------------------------*/
 /*
  * att_debug_cb: ATT 协议栈内部事件的调试打印回调
  *               当 verbose 模式开启时，该函数会被注册到 BlueZ 库中，
  *               用于实时打印底层的 ATT PDU 收发和状态变化。
  * @str: 由蓝牙协议栈生成的具体调试信息字符串
  * @user_data: 用户传入的上下文数据，这里被用作日志的前缀标识
  */
 static void att_debug_cb(const char *str, void *user_data)
 {
	 const char *prefix = user_data;
 
	 // 颜色宏已移除，改为纯文本格式；PRLOG 内部已包含 verbose 判断
	 PRLOG("%s%s\n", prefix, str);
 }
 
 
 
 
 /*--------------------------------------------------------------------------
  * 1. GATT 调试日志回调 (开发调试工具)
  *--------------------------------------------------------------------------*/
 /*
  * gatt_debug_cb: GATT 协议栈内部事件的调试打印回调
  *                与 att_debug_cb 类似，但专门用于打印更上层的 GATT 事件日志。
  * @str: 由蓝牙协议栈生成的具体调试信息字符串
  * @user_data: 用户传入的上下文数据，这里被用作日志的前缀标识（如 "GATT:"）
  */
 static void gatt_debug_cb(const char *str, void *user_data)
 {
	 const char *prefix = user_data;
 
	 // 颜色宏已移除，改为纯文本格式；PRLOG 内部已包含 verbose 判断
	 PRLOG("%s%s\n", prefix, str);
 }
 
 
 
 
 
 /*--------------------------------------------------------------------------
  * 2. GAP 设备名称读取回调 (处理客户端读请求)
  *--------------------------------------------------------------------------*/
 /*
  * gap_device_name_read_cb: 当中心设备（如手机）尝试读取本设备的“设备名称”时触发
  * 
  * @attrib:   当前被读取的 GATT 属性句柄
  * @id:       本次读取操作的唯一事务 ID（Transaction ID），用于匹配后续的响应
  * @offset:   读取数据的偏移量（支持长属性的分片读取）
  * @opcode:   触发此次回调的 ATT 操作码（如 Read Request 或 Read Blob Request）
  * @att:      底层的 ATT 传输对象
  * @user_data: 指向核心结构体 struct server 的指针，包含设备名称的实际数据
  */
 static void gap_device_name_read_cb(struct gatt_db_attribute *attrib,
					 unsigned int id, uint16_t offset,
					 uint8_t opcode, struct bt_att *att,
					 void *user_data)
 {
	 // 提取全局服务器上下文，获取设备名称等状态信息
	 struct server *server = user_data;
	 
	 uint8_t error = 0;          // 错误码，默认为 0 (无错误)
	 size_t len = 0;             // 实际需要返回的数据长度
	 const uint8_t *value = NULL;// 指向实际要返回的数据缓冲区指针
 
	 // 打印调试信息，表明该回调已被触发
	 PRLOG("GAP Device Name Read called\n");
 
	 // 获取当前设备名称的总长度
	 len = server->name_len;
 
	 // 边界检查：如果客户端请求的偏移量超出了名称的实际长度，则报错
	 if (offset > len) {
		 error = BT_ATT_ERROR_INVALID_OFFSET; // 设置标准 ATT 无效偏移量错误码
		 goto done;                           // 跳转到结果返回处
	 }
 
	 // 计算剩余可读取的长度（总长度 - 已跳过的偏移量）
	 len -= offset;
	 
	 // 如果还有剩余数据需要读取，将 value 指针偏移到对应位置；否则置空
	 value = len ? &server->device_name[offset] : NULL;
 
 done:
	 // 【关键步骤】向协议栈提交读取结果
	 // 无论成功还是失败，都必须调用此函数来回应客户端的读取请求。
	 // 参数说明：属性句柄、事务ID、错误码、数据指针、数据长度
	 gatt_db_attribute_read_result(attrib, id, error, value, len);
 }
 
 
 // 定义一个确认回调
 static void indicate_conf_cb(void *user_data)
 {
	 (void)user_data;
	 printf("Indicate confirmed successfully!\n");
 }
 
 
 
 /* 
  * send_custom_notification: 发送自定义数据指示 (Indicate)
  */
 static bool send_custom_notification(void *user_data)
 {
	 struct server *server = user_data;
 
		 printf("send_custom_notification!\n"); 
	 
	 // 1. 前置检查：数据有效 + 客户端已使能
	 if (!server->external_data_ptr || server->external_data_len == 0) {
		 return true; // 数据未就绪，保持定时器等待
	 }
 
	 if (!server->custom_notify_enabled) {
		 printf("Custom Indicate skipped: client not subscribed\n");
		 return true;
	 }
 
	 // 2. MTU 安全检查（Indicate 最大载荷 = MTU - 3）
	 uint16_t mtu = bt_att_get_mtu(server->att);
	 uint16_t max_payload = (mtu > 3) ? (mtu - 3) : 0;
	 uint16_t send_len = server->external_data_len;
 
	 if (send_len > max_payload) {
		 printf("Warning: truncating indicate from %d to %d bytes (MTU=%d)\n",
			   send_len, max_payload, mtu);
		 send_len = max_payload;
	 }
 
	 // 3. 【核心修改】使用专门的 Indicate 发送函数
	 // bt_gatt_server_send_indicate 不需要 is_indication 参数，
	 // 因为它天生就是用来发 Indicate 的。
	 bool success = bt_gatt_server_send_indication(
		 server->gatt,
		 server->custom_data_handle,
		 server->external_data_ptr,
		 send_len,
		 indicate_conf_cb, // 传入确认回调
		 NULL, 
		 NULL
	 );
 
	 if (success) {
		 printf("Sent Custom Indicate: %d bytes\n", send_len);
	 } else {
		 printf("Failed to send Custom Indicate (len=%d, MTU=%d)\n", send_len, mtu);
	 }
 
	 return true; // 保持定时器持续运行
 }
 
 
 
 
 /* 自定义，huang */
 static uint32_t get_le32_local(const uint8_t *p)
 {
	 return ((uint32_t)p[0]) |
			((uint32_t)p[1] << 8) |
			((uint32_t)p[2] << 16) |
			((uint32_t)p[3] << 24);
 }
 
 static uint16_t get_le16_local(const uint8_t *p)
 {
	 return ((uint16_t)p[0]) | ((uint16_t)p[1] << 8);
 }
 
 static void put_le32_local(uint8_t *p, uint32_t v)
 {
	 p[0] = v & 0xff;
	 p[1] = (v >> 8) & 0xff;
	 p[2] = (v >> 16) & 0xff;
	 p[3] = (v >> 24) & 0xff;
 }

 static void put_le16_local(uint8_t *p, uint16_t v)
 {
	 p[0] = v & 0xff;
	 p[1] = (v >> 8) & 0xff;
 }
 
 static uint32_t crc32_update_local(uint32_t crc, const uint8_t *buf, size_t len)
 {
	 size_t i;
	 int b;
 
	 crc = ~crc;
	 for (i = 0; i < len; i++) {
		 crc ^= buf[i];
		 for (b = 0; b < 8; b++)
			 crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
	 }
	 return ~crc;
 }
 
 static const uint16_t crc16_table[256] = {
	 0x0000,0x1021,0x2042,0x3063,0x4084,0x50a5,0x60c6,0x70e7,
	 0x8108,0x9129,0xa14a,0xb16b,0xc18c,0xd1ad,0xe1ce,0xf1ef,
	 0x1231,0x0210,0x3273,0x2252,0x52b5,0x4294,0x72f7,0x62d6,
	 0x9339,0x8318,0xb37b,0xa35a,0xd3bd,0xc39c,0xf3ff,0xe3de,
	 0x2462,0x3443,0x0420,0x1401,0x64e6,0x74c7,0x44a4,0x5485,
	 0xa56a,0xb54b,0x8528,0x9509,0xe5ee,0xf5cf,0xc5ac,0xd58d,
	 0x3653,0x2672,0x1611,0x0630,0x76d7,0x66f6,0x5695,0x46b4,
	 0xb75b,0xa77a,0x9719,0x8738,0xf7df,0xe7fe,0xd79d,0xc7bc,
	 0x48c4,0x58e5,0x6886,0x78a7,0x0840,0x1861,0x2802,0x3823,
	 0xc9cc,0xd9ed,0xe98e,0xf9af,0x8948,0x9969,0xa90a,0xb92b,
	 0x5af5,0x4ad4,0x7ab7,0x6a96,0x1a71,0x0a50,0x3a33,0x2a12,
	 0xdbfd,0xcbdc,0xfbbf,0xeb9e,0x9b79,0x8b58,0xbb3b,0xab1a,
	 0x6ca6,0x7c87,0x4ce4,0x5cc5,0x2c22,0x3c03,0x0c60,0x1c41,
	 0xedae,0xfd8f,0xcdec,0xddcd,0xad2a,0xbd0b,0x8d68,0x9d49,
	 0x7e97,0x6eb6,0x5ed5,0x4ef4,0x3e13,0x2e32,0x1e51,0x0e70,
	 0xff9f,0xefbe,0xdfdd,0xcffc,0xbf1b,0xaf3a,0x9f59,0x8f78,
	 0x9188,0x81a9,0xb1ca,0xa1eb,0xd10c,0xc12d,0xf14e,0xe16f,
	 0x1080,0x00a1,0x30c2,0x20e3,0x5004,0x4025,0x7046,0x6067,
	 0x83b9,0x9398,0xa3fb,0xb3da,0xc33d,0xd31c,0xe37f,0xf35e,
	 0x02b1,0x1290,0x22f3,0x32d2,0x4235,0x5214,0x6277,0x7256,
	 0xb5ea,0xa5cb,0x95a8,0x8589,0xf56e,0xe54f,0xd52c,0xc50d,
	 0x34e2,0x24c3,0x14a0,0x0481,0x7466,0x6447,0x5424,0x4405,
	 0xa7db,0xb7fa,0x8799,0x97b8,0xe75f,0xf77e,0xc71d,0xd73c,
	 0x26d3,0x36f2,0x0691,0x16b0,0x6657,0x7676,0x4615,0x5634,
	 0xd94c,0xc96d,0xf90e,0xe92f,0x99c8,0x89e9,0xb98a,0xa9ab,
	 0x5844,0x4865,0x7806,0x6827,0x18c0,0x08e1,0x3882,0x28a3,
	 0xcb7d,0xdb5c,0xeb3f,0xfb1e,0x8bf9,0x9bd8,0xabbb,0xbb9a,
	 0x4a75,0x5a54,0x6a37,0x7a16,0x0af1,0x1ad0,0x2ab3,0x3a92,
	 0xfd2e,0xed0f,0xdd6c,0xcd4d,0xbdaa,0xad8b,0x9de8,0x8dc9,
	 0x7c26,0x6c07,0x5c64,0x4c45,0x3ca2,0x2c83,0x1ce0,0x0cc1,
	 0xef1f,0xff3e,0xcf5d,0xdf7c,0xaf9b,0xbfba,0x8fd9,0x9ff8,
	 0x6e17,0x7e36,0x4e55,0x5e74,0x2e93,0x3eb2,0x0ed1,0x1ef0
 };
 
 static uint16_t crc16_update_local(uint16_t crc, const uint8_t *buf, size_t len)
 {
	 size_t i;
 
	 for (i = 0; i < len; i++)
		 crc = (uint16_t)((crc << 8) ^ crc16_table[((crc >> 8) ^ buf[i]) & 0xff]);
	 return crc;
 }
 
 static uint16_t crc16_local(const uint8_t *buf, size_t len)
 {
	 return crc16_update_local(0xffff, buf, len);
 }
 
 /* --- Serial <-> BLE bridge (global UART + TX ring buffer) --- */
 static int g_bridge_uart_fd = -1;
 static struct server *g_active_server = NULL;
 static uint8_t g_bridge_tx_buf[BRIDGE_TX_BUF_SIZE];
 static size_t g_bridge_tx_r;
 static size_t g_bridge_tx_w;
 static size_t g_bridge_tx_used;
 static void bridge_schedule_flush(struct server *server);
 
 static bool bridge_is_file_opcode(uint8_t op)
 {
	 return op == FILE_RX_OP_START || op == FILE_RX_OP_DATA ||
			op == FILE_RX_OP_END || op == FILE_RX_OP_ABORT;
 }

 static bool bridge_is_stream_opcode(uint8_t op)
 {
	 return op == STREAM_OP_START || op == STREAM_OP_ABORT;
 }

 static bool bridge_is_time_opcode(uint8_t op)
 {
	 return op == TIME_OP_SET;
 }

static bool bridge_is_fnirs_opcode(uint8_t op)
{
	 return op >= FNIRS_OP_SCAN && op <= FNIRS_OP_RSP;
}

static bool bridge_is_live_file_opcode(uint8_t op)
{
	 return op >= FILE_LIVE_OP_REQ && op <= FILE_LIVE_OP_ABORT;
}

 static bool bridge_is_ble_file_opcode(uint8_t op)
 {
	 return bridge_is_file_opcode(op) ||
			op == FILE_TX_OP_REQ || op == FILE_TX_OP_ABORT ||
			op == FILE_TX_OP_ACK || op == FILE_TX_LAST_REQ ||
			bridge_is_live_file_opcode(op) ||
			bridge_is_stream_opcode(op) || bridge_is_time_opcode(op) ||
			bridge_is_fnirs_opcode(op);
 }
 
 static void bridge_tx_enqueue(const uint8_t *data, size_t len)
 {
	 size_t i;
 
	 if (!data || len == 0)
		 return;
 
	 for (i = 0; i < len; i++) {
		 if (g_bridge_tx_used >= BRIDGE_TX_BUF_SIZE)
			 break;
		 g_bridge_tx_buf[g_bridge_tx_w] = data[i];
		 g_bridge_tx_w = (g_bridge_tx_w + 1) % BRIDGE_TX_BUF_SIZE;
		 g_bridge_tx_used++;
	 }
 }
 
 static size_t bridge_tx_peek(uint8_t *out, size_t max_len)
 {
	 size_t i, n = MIN(g_bridge_tx_used, max_len);
 
	 for (i = 0; i < n; i++)
		 out[i] = g_bridge_tx_buf[(g_bridge_tx_r + i) % BRIDGE_TX_BUF_SIZE];
	 return n;
 }
 
 static void bridge_tx_consume(size_t n)
 {
	 if (n > g_bridge_tx_used)
		 n = g_bridge_tx_used;
	 g_bridge_tx_r = (g_bridge_tx_r + n) % BRIDGE_TX_BUF_SIZE;
	 g_bridge_tx_used -= n;
 }
 
 static bool bridge_flush_cb(void *user_data)
 {
	 struct server *server = user_data;
	 uint8_t chunk[512];
	 size_t chunk_len, max_payload;
	 uint16_t mtu;
 
	 if (server)
		 server->bridge_flush_id = 0;
 
	 if (!server || !server->custom_notify_enabled || g_bridge_tx_used == 0)
		 return false;

	 if (server->rx_active || server->tx_active || server->live_active ||
	     server->stream_active)
		 return false;
 
	 mtu = bt_att_get_mtu(server->att);
	 max_payload = (mtu > 3) ? (mtu - 3) : 20;
 
	 chunk_len = bridge_tx_peek(chunk, max_payload);
	 if (chunk_len == 0)
		 return false;
 
	 if (bt_gatt_server_send_notification(server->gatt,
										  server->custom_data_handle,
										  chunk,
										  chunk_len,
										  false)) {
		 bridge_tx_consume(chunk_len);
		 PRLOG("BRIDGE BLE TX: %zu bytes (queued=%zu)\n",
			   chunk_len, g_bridge_tx_used);
	 } else {
		 printf("BRIDGE BLE TX failed: len=%zu queued=%zu\n",
				chunk_len, g_bridge_tx_used);
	 }
 
	 if (g_bridge_tx_used > 0)
		 bridge_schedule_flush(server);
 
	 return false;
 }
 
 static void bridge_schedule_flush(struct server *server)
 {
	 if (!server || !server->custom_notify_enabled || g_bridge_tx_used == 0)
		 return;
 
	 if (!server->bridge_flush_id)
		 server->bridge_flush_id = timeout_add(BRIDGE_FLUSH_MS,
											   bridge_flush_cb,
											   server,
											   NULL);
 }
 
 enum bridge_src {
	 BRIDGE_SRC_UART = 0,
	 BRIDGE_SRC_BLE = 1,
 };
 
 static const char *bridge_src_name(enum bridge_src src)
 {
	 return src == BRIDGE_SRC_BLE ? "BLE" : "UART";
 }
 
 static void bridge_dispatch_input(struct server *server, enum bridge_src src,
				   const uint8_t *data, size_t len)
 {
	 ssize_t n;
	 const char *src_name = bridge_src_name(src);
 
	 if (!data || len == 0)
		 return;
 
	 /* Requirement: whichever side receives data, UART side should also output. */
	 if (g_bridge_uart_fd >= 0) {
		 n = write(g_bridge_uart_fd, data, len);
		 if (n < 0)
			 perror("bridge uart write");
	 }
 
	 /* Requirement: whichever side receives data, BLE side should also output. */
	 bridge_tx_enqueue(data, len);
	 bridge_schedule_flush(server);
 
	 if (verbose)
		 printf("BRIDGE %s RX: %zu bytes (ble_q=%zu)\n",
				src_name, len, g_bridge_tx_used);
 }
 
 static void bridge_uart_cb(int fd, uint32_t events, void *user_data)
 {
	 uint8_t buf[256];
	 ssize_t n;
 
	 (void)user_data;
 
	 if (!(events & EPOLLIN))
		 return;
 
	 n = read(fd, buf, sizeof(buf));
	 if (n <= 0)
		 return;
 
	 bridge_dispatch_input(g_active_server, BRIDGE_SRC_UART, buf, (size_t)n);
 }
 
 static int bridge_uart_open(const char *dev_path, speed_t baud)
 {
	 struct termios tio;
	 int fd;
 
	 fd = open(dev_path, O_RDWR | O_NOCTTY | O_NONBLOCK);
	 if (fd < 0) {
		 perror(dev_path);
		 return -1;
	 }
 
	 if (tcgetattr(fd, &tio) < 0) {
		 perror("tcgetattr");
		 close(fd);
		 return -1;
	 }
 
	 cfmakeraw(&tio);
	 cfsetispeed(&tio, baud);
	 cfsetospeed(&tio, baud);
	 tio.c_cflag |= (CLOCAL | CREAD);
	 tio.c_cflag &= ~CRTSCTS;
	 tio.c_cc[VMIN] = 0;
	 tio.c_cc[VTIME] = 0;
 
	 if (tcsetattr(fd, TCSANOW, &tio) < 0) {
		 perror("tcsetattr");
		 close(fd);
		 return -1;
	 }
 
	 if (mainloop_add_fd(fd, EPOLLIN, bridge_uart_cb, NULL, NULL) < 0) {
		 fprintf(stderr, "Failed to watch bridge UART fd\n");
		 close(fd);
		 return -1;
	 }
 
	 printf("BRIDGE UART open: %s @ %d\n", dev_path, (int)BRIDGE_UART_BAUD);
	 return fd;
 }
 
 static void bridge_uart_close(void)
 {
	 if (g_bridge_uart_fd < 0)
		 return;
 
	 mainloop_remove_fd(g_bridge_uart_fd);
	 close(g_bridge_uart_fd);
	 g_bridge_uart_fd = -1;
 }
 
 static const char *file_rx_dir(void)
 {
	 const char *dir = getenv("BLE_RX_DIR");
 
	 if (dir && dir[0])
		 return dir;
	 return FILE_RX_DIR_DEFAULT;
 }

 static const char *file_tx_dir(void)
 {
	 const char *dir = getenv("BLE_TX_DIR");

	 if (dir && dir[0])
		 return dir;
	 return FILE_TX_DIR_DEFAULT;
 }
 
 static void file_rx_update_status(struct server *server)
 {
	 /* [0]=0xA5, [1]=state, [2]=last_error, [3]=reserved
	  * [4..7]=received, [8..11]=expected, [12..15]=next_seq, [16..19]=running_crc
	  */
	 if (!server->external_data_ptr || server->external_data_len < CUSTOM_DATA_LEN)
		 return;

	 server->external_data_ptr[0] = 0xA5;
	 server->external_data_ptr[1] = server->rx_state;
	 server->external_data_ptr[2] = server->rx_last_error;
	 server->external_data_ptr[3] = 0x00;
	 put_le32_local(server->external_data_ptr + 4, server->rx_received_size);
	 put_le32_local(server->external_data_ptr + 8, server->rx_expected_size);
	 put_le32_local(server->external_data_ptr + 12, server->rx_next_seq);
	 put_le32_local(server->external_data_ptr + 16, server->rx_running_crc);
	 server->external_data_len = CUSTOM_DATA_LEN;
 }

 static void file_rx_notify_status(struct server *server, bool force)
 {
	 uint16_t mtu;
	 uint16_t max_payload;
	 uint16_t send_len;
	 bool should_send = force;

	 if (!server)
		 return;

	 if (!server->custom_notify_enabled || !server->gatt ||
		 !server->external_data_ptr || server->external_data_len == 0) {
		 if (force)
			 printf("FILE RX status notify skipped (CCCD not enabled?)\n");
		 return;
	 }

	 if (!should_send && server->rx_state == FILE_RX_STATE_ACTIVE) {
		 if ((server->rx_next_seq - server->rx_last_status_seq) >= FILE_RX_STATUS_NOTIFY_PKTS ||
		     (server->rx_received_size - server->rx_last_status_bytes) >= FILE_RX_STATUS_NOTIFY_BYTES)
			 should_send = true;
	 }
	 if (!should_send)
		 return;

	 /* Pack status only when we are about to push it over BLE. */
	 file_rx_update_status(server);

	 mtu = bt_att_get_mtu(server->att);
	 max_payload = (mtu > 3) ? (mtu - 3) : CUSTOM_DATA_LEN;
	 send_len = MIN((uint16_t)server->external_data_len, max_payload);

	 if (bt_gatt_server_send_notification(server->gatt,
						 server->custom_data_handle,
						 server->external_data_ptr, send_len,
						 false)) {
		 server->rx_last_status_seq = server->rx_next_seq;
		 server->rx_last_status_bytes = server->rx_received_size;
	 } else if (force) {
		 printf("FILE RX status notify send failed (mtu=%u len=%u)\n",
				mtu, send_len);
	 }
 }

 static void file_rx_abort(struct server *server)
 {
	 if (server->rx_fp) {
		 fclose(server->rx_fp);
		 server->rx_fp = NULL;
	 }
	 free(server->rx_io_buf);
	 server->rx_io_buf = NULL;
	 if (server->rx_received_size == 0 && server->rx_path[0])
		 unlink(server->rx_path);
	 server->rx_active = false;
	 server->rx_expected_size = 0;
	 server->rx_received_size = 0;
	 server->rx_expected_crc = 0;
	 server->rx_running_crc = 0;
	 server->rx_next_seq = 0;
	 server->rx_last_status_seq = 0;
	 server->rx_last_status_bytes = 0;
	 server->rx_state = FILE_RX_STATE_IDLE;
	 server->rx_last_error = 0;
	 server->rx_path[0] = '\0';
	 file_rx_update_status(server);
 }

 static bool file_tx_send_notify(struct server *server, const uint8_t *data, size_t len);

 static void file_tx_write_last_marker(const char *basename)
 {
	 FILE *fp;

	 if (!basename || !basename[0])
		 return;

	 mkdir(file_tx_dir(), 0755);
	 fp = fopen(FILE_TX_LAST_MARKER, "w");
	 if (!fp) {
		 printf("FILE TX LAST marker write failed\n");
		 return;
	 }
	 fprintf(fp, "%s\n", basename);
	 fclose(fp);
	 printf("FILE TX LAST marker: %s\n", basename);
 }

 static bool file_tx_read_last_marker(char *name_out, size_t name_sz)
 {
	 FILE *fp;
	 char *nl;

	 if (!name_out || name_sz == 0)
		 return false;

	 name_out[0] = '\0';
	 fp = fopen(FILE_TX_LAST_MARKER, "r");
	 if (!fp)
		 return false;

	 if (!fgets(name_out, name_sz, fp)) {
		 fclose(fp);
		 return false;
	 }
	 fclose(fp);

	 nl = strchr(name_out, '\n');
	 if (nl)
		 *nl = '\0';
	 nl = strchr(name_out, '\r');
	 if (nl)
		 *nl = '\0';

	 return name_out[0] != '\0';
 }

 static bool file_tx_find_newest_recording(char *name_out, size_t name_sz)
 {
	 DIR *dir;
	 struct dirent *ent;
	 struct stat st;
	 char path[512];
	 time_t best_mtime = 0;
	 off_t best_size = 0;

	 if (!name_out || name_sz == 0)
		 return false;

	 name_out[0] = '\0';
	 dir = opendir(file_tx_dir());
	 if (!dir)
		 return false;

	 while ((ent = readdir(dir)) != NULL) {
		 const char *n = ent->d_name;
		 size_t len;

		 if (n[0] == '.')
			 continue;
		 len = strlen(n);
		 if (len < 5 || strcmp(n + len - 4, ".bin") != 0)
			 continue;

		 snprintf(path, sizeof(path), "%s/%s", file_tx_dir(), n);
		 if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
			 continue;

		 if (name_out[0] == '\0' || st.st_mtime > best_mtime ||
		     (st.st_mtime == best_mtime && st.st_size >= best_size)) {
			 best_mtime = st.st_mtime;
			 best_size = st.st_size;
			 strncpy(name_out, n, name_sz - 1);
			 name_out[name_sz - 1] = '\0';
		 }
	 }

	 closedir(dir);
	 return name_out[0] != '\0';
 }

 static bool file_tx_resolve_last_recording(char *name_out, size_t name_sz)
 {
	 if (file_tx_read_last_marker(name_out, name_sz))
		 return true;

	 if (file_tx_find_newest_recording(name_out, name_sz)) {
		 printf("FILE TX LAST fallback (newest .bin): %s\n", name_out);
		 return true;
	 }

	 return false;
 }

 #define STREAM_CLOCK_MIN_VALID 1577836800L /* 2020-01-01 UTC */

 static uint32_t stream_rec_next_id;

 static uint32_t stream_rec_load_session_id(void)
 {
	 FILE *fp;
	 uint32_t id = 0;
	 char path[512];

	 snprintf(path, sizeof(path), "%s/.session", file_tx_dir());
	 fp = fopen(path, "r");
	 if (!fp)
		 return 0;
	 if (fscanf(fp, "%u", &id) != 1)
		 id = 0;
	 fclose(fp);
	 return id;
 }

 static void stream_rec_save_session_id(uint32_t id)
 {
	 FILE *fp;
	 char path[512];

	 mkdir(file_tx_dir(), 0755);
	 snprintf(path, sizeof(path), "%s/.session", file_tx_dir());
	 fp = fopen(path, "w");
	 if (!fp)
		 return;
	 fprintf(fp, "%u\n", id);
	 fclose(fp);
 }

 #define BOARD_TIME_MIN_VALID 1577836800L /* 2020-01-01 UTC */

 static bool board_set_unix_time(uint32_t unix_sec)
 {
	 struct timeval tv;
	 time_t now;

	 if (unix_sec < BOARD_TIME_MIN_VALID)
		 return false;

	 tv.tv_sec = (time_t)unix_sec;
	 tv.tv_usec = 0;
	 if (settimeofday(&tv, NULL) != 0) {
		 perror("settimeofday");
		 return false;
	 }

	 now = time(NULL);
	 printf("TIME SET from phone: %u -> %s", unix_sec, ctime(&now));

	 if (access("/dev/rtc0", F_OK) == 0)
		 system("hwclock -w -f /dev/rtc0 2>/dev/null");

	 return true;
 }

 static bool stream_open_rec_file(struct server *server)
 {
	 time_t now;
	 struct tm *tm_info;
	 char ts[32];
	 uint8_t hdr[24];

	 if (!server)
		 return false;

	 mkdir(file_tx_dir(), 0755);
	 now = time(NULL);
	 tm_info = localtime(&now);
	 if (now >= STREAM_CLOCK_MIN_VALID && tm_info) {
		 strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", tm_info);
	 } else {
		 if (stream_rec_next_id == 0)
			 stream_rec_next_id = stream_rec_load_session_id();
		 stream_rec_next_id++;
		 stream_rec_save_session_id(stream_rec_next_id);
		 snprintf(ts, sizeof(ts), "sess%05u", stream_rec_next_id);
		 printf("STREAM: system clock unset (now=%ld), use record_%s.bin\n",
			(long)now, ts);
	 }
	 snprintf(server->stream_rec_path, sizeof(server->stream_rec_path),
		  "%s/record_%s.bin", file_tx_dir(), ts);

	 server->stream_rec_fp = fopen(server->stream_rec_path, "wb");
	 if (!server->stream_rec_fp) {
		 perror("stream record fopen");
		 server->stream_rec_path[0] = '\0';
		 return false;
	 }

	 memset(hdr, 0, sizeof(hdr));
	 hdr[0] = 'f';
	 hdr[1] = 'S';
	 hdr[2] = 'I';
	 hdr[3] = 'M';
	 put_le16_local(hdr + 4, 1);
	 put_le32_local(hdr + 8, server->stream_rate_hz);
	 put_le32_local(hdr + 12, 0);
	 put_le16_local(hdr + 16, 1);
	 put_le16_local(hdr + 18, 16);
	 if (fwrite(hdr, 1, sizeof(hdr), server->stream_rec_fp) != sizeof(hdr)) {
		 fclose(server->stream_rec_fp);
		 server->stream_rec_fp = NULL;
		 server->stream_rec_path[0] = '\0';
		 return false;
	 }

	 printf("STREAM record open: %s\n", server->stream_rec_path);
	 return true;
 }

 static void stream_close_rec_file(struct server *server)
 {
	 uint32_t total;
	 uint8_t cnt[4];
	 const char *base;

	 if (!server || !server->stream_rec_fp)
		 return;

	 total = server->stream_sample_idx;
	 fseek(server->stream_rec_fp, 12, SEEK_SET);
	 put_le32_local(cnt, total);
	 if (fwrite(cnt, 1, 4, server->stream_rec_fp) != 4)
		 printf("STREAM record header rewrite failed\n");
	 fflush(server->stream_rec_fp);
	 fclose(server->stream_rec_fp);
	 server->stream_rec_fp = NULL;

	 base = strrchr(server->stream_rec_path, '/');
	 base = base ? base + 1 : server->stream_rec_path;
	 file_tx_write_last_marker(base);
	 printf("STREAM record saved: %s (%u samples)\n",
			server->stream_rec_path, total);
	 server->stream_rec_path[0] = '\0';
 }

 static uint32_t stream_mix32(uint32_t x)
 {
	 x ^= x << 13;
	 x ^= x >> 17;
	 x ^= x << 5;
	 return x;
 }

 /* Rest/task blocks: amplitude and hemodynamic response change over time. */
 static double stream_task_envelope(double t_sec)
 {
	 double cycle = 30.0;
	 double p = fmod(t_sec, cycle);

	 if (p < 18.0) {
		 double u = p / 18.0;

		 return 0.35 + 0.65 * (0.5 + 0.5 * sin(M_PI * u));
	 }

	 return 0.18 + 0.14 * (1.0 - (p - 18.0) / 12.0);
 }

static bool fnirs_stream_use_sim(void)
{
	const char *v = getenv("FNIRS_STREAM_SIM");

	return v && (v[0] == '1' || v[0] == 'y' || v[0] == 'Y');
}

static int g_fnirs_stream_data_fd = -1;
static int16_t g_fnirs_last_sample;

static bool fnirs_ble_ipc_request(uint8_t cmd, const uint8_t *req, uint16_t req_len,
				  uint8_t *status_out, uint8_t *rsp, uint16_t rsp_max,
				  uint16_t *rsp_len_out)
{
#ifdef FNIRS_EMBEDDED
	return fnirs_ble_dispatch_request(cmd, req, req_len, status_out, rsp,
					  rsp_max, rsp_len_out) == 0;
#else
	struct sockaddr_un addr;
	uint8_t hdr[5];
	uint8_t rhdr[6];
	ssize_t n;
	int fd;
	uint16_t rsp_len;

	if (status_out)
		*status_out = 0xFF;
	if (rsp_len_out)
		*rsp_len_out = 0;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return false;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, FNIRS_BLE_SOCK_PATH, sizeof(addr.sun_path) - 1);

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		printf("fnirs_ble_ipc connect %s failed: %s\n",
		       FNIRS_BLE_SOCK_PATH, strerror(errno));
		close(fd);
		return false;
	}

	hdr[0] = FNIRS_BLE_REQ_MAGIC0;
	hdr[1] = FNIRS_BLE_REQ_MAGIC1;
	hdr[2] = cmd;
	hdr[3] = (uint8_t)(req_len & 0xff);
	hdr[4] = (uint8_t)((req_len >> 8) & 0xff);
	if (write(fd, hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) {
		close(fd);
		return false;
	}
	if (req_len > 0 && req) {
		if (write(fd, req, req_len) != (ssize_t)req_len) {
			close(fd);
			return false;
		}
	}

	n = read(fd, rhdr, sizeof(rhdr));
	if (n != (ssize_t)sizeof(rhdr) || rhdr[0] != FNIRS_BLE_RSP_MAGIC0 ||
	    rhdr[1] != FNIRS_BLE_RSP_MAGIC1) {
		close(fd);
		return false;
	}

	if (status_out)
		*status_out = rhdr[3];
	rsp_len = (uint16_t)rhdr[4] | ((uint16_t)rhdr[5] << 8);
	if (rsp_len > 0 && rsp && rsp_max > 0) {
		if (rsp_len > rsp_max)
			rsp_len = rsp_max;
		n = read(fd, rsp, rsp_len);
		if (n != (ssize_t)rsp_len) {
			close(fd);
			return false;
		}
		if (rsp_len_out)
			*rsp_len_out = rsp_len;
	}

	close(fd);
	return true;
#endif
}

static bool fnirs_ble_notify_rsp(struct server *server, uint8_t echo_op,
				 uint8_t status, const uint8_t *payload,
				 uint16_t len)
{
	uint8_t pkt[140];
	uint16_t max_payload = sizeof(pkt) - 3;

	if (!server || !server->custom_notify_enabled) {
		printf("FNIRS RSP notify skipped: op=0x%02x status=%u len=%u notify_enabled=%d\n",
		       echo_op, status, len,
		       server ? server->custom_notify_enabled : 0);
		fflush(stdout);
		return false;
	}
	if (len > max_payload)
		len = max_payload;

	pkt[0] = FNIRS_OP_RSP;
	pkt[1] = echo_op;
	pkt[2] = status;
	if (len > 0 && payload)
		memcpy(pkt + 3, payload, len);
	if (!file_tx_send_notify(server, pkt, 3 + len)) {
		printf("FNIRS RSP notify failed: op=0x%02x status=%u len=%u\n",
		       echo_op, status, len);
		fflush(stdout);
		return false;
	}

	printf("FNIRS RSP notify sent: op=0x%02x status=%u len=%u\n",
	       echo_op, status, len);
	fflush(stdout);
	return true;
}

static void fnirs_stream_ctl_send(char cmd)
{
	int fd = open(FNIRS_STREAM_CTL_PATH, O_WRONLY | O_NONBLOCK);

	if (fd < 0) {
		printf("fnirs_stream ctl open failed: %s\n", strerror(errno));
		return;
	}

	if (write(fd, &cmd, 1) != 1)
		printf("fnirs_stream ctl write failed: %s\n", strerror(errno));

	close(fd);
}

static void fnirs_stream_data_close(void)
{
	if (g_fnirs_stream_data_fd >= 0) {
		close(g_fnirs_stream_data_fd);
		g_fnirs_stream_data_fd = -1;
	}
}

static int fnirs_stream_data_open(void)
{
	int i;

	fnirs_stream_data_close();

	/* O_RDONLY reads from my-fnirs-ev O_RDWR writer (not O_RDWR here). */
	for (i = 0; i < 500; i++) {
		g_fnirs_stream_data_fd = open(FNIRS_STREAM_DATA_PATH,
					      O_RDONLY | O_NONBLOCK);
		if (g_fnirs_stream_data_fd >= 0) {
			printf("fnirs_stream fifo reader ready\n");
			return 0;
		}
		if (errno != ENXIO)
			break;
		usleep(10000);
	}

	printf("fnirs_stream data open failed: %s\n", strerror(errno));
	return -1;
}

static int fnirs_stream_read_samples(int16_t *out, uint16_t max)
{
	uint16_t n = 0;

#ifdef FNIRS_EMBEDDED
	int got;

	got = fnirs_stream_ring_read(out, max);
	if (got > 0 && out)
		g_fnirs_last_sample = out[got - 1];
	return got;
#endif

	if (g_fnirs_stream_data_fd < 0 || !out || max == 0)
		return 0;

	while (n < max) {
		uint8_t raw[2];
		ssize_t r = read(g_fnirs_stream_data_fd, raw, sizeof(raw));

		if (r != (ssize_t)sizeof(raw))
			break;

		out[n] = (int16_t)(raw[0] | (raw[1] << 8));
		g_fnirs_last_sample = out[n];
		n++;
	}

	return (int)n;
}

static void fnirs_stream_ipc_start(void)
{
	uint8_t st = 0xFF;
	uint8_t rc = 0xFF;
	uint16_t rsp_len = 0;

	/* Same order as my-fnirs: IPC START first, then open fifo reader. */
	if (!fnirs_ble_ipc_request(FNIRS_BLE_CMD_STREAM_START, NULL, 0, &st,
				   &rc, 1, &rsp_len)) {
		printf("FNIRS STREAM_START: ipc failed (my-fnirs-ev socket?)\n");
		return;
	}

	if (st != 0)
		printf("FNIRS STREAM_START ipc status=%u\n", st);
	if (rsp_len >= 1) {
		if (rc != 0)
			printf("FNIRS STREAM_START: fNIRS_on rc=%u\n", rc);
		else
			printf("FNIRS STREAM_START: fNIRS_on ok (embedded)\n");
	}

#ifndef FNIRS_EMBEDDED
	if (fnirs_stream_data_open() != 0)
		printf("FNIRS stream: fifo reader not ready\n");
#endif
}

static void fnirs_stream_ipc_stop(char cmd)
{
	uint8_t ble_cmd = FNIRS_BLE_CMD_STREAM_STOP;

	(void)cmd;
	fnirs_ble_ipc_request(ble_cmd, NULL, 0, NULL, NULL, 0, NULL);
#ifndef FNIRS_EMBEDDED
	fnirs_stream_data_close();
#endif
}

 static int16_t stream_fnirs_sim_sample(uint32_t idx, uint32_t rate_hz)
 {
	 double t, env, baseline, hemo, cardiac, resp, mayer, noise, motion, v;
	 uint32_t h;

	 if (rate_hz < 1)
		 rate_hz = STREAM_RATE_HZ_DEFAULT;

	 t = (double)idx / (double)rate_hz;
	 env = stream_task_envelope(t);

	 baseline = 12500.0 + 350.0 * sin(2.0 * M_PI * t / 95.0);
	 hemo = env * 2200.0 * sin(2.0 * M_PI * 0.038 * t + 0.7);
	 hemo += env * 900.0 * sin(2.0 * M_PI * 0.011 * t);
	 cardiac = 280.0 * sin(2.0 * M_PI * 1.07 * t);
	 resp = 190.0 * sin(2.0 * M_PI * 0.26 * t + 1.1);
	 mayer = 140.0 * sin(2.0 * M_PI * 0.12 * t);

	 h = stream_mix32(idx ^ 0xA531u);
	 noise = (double)((int)(h % 1001) - 500) * (0.8 + env);

	 motion = 0.0;
	 if (rate_hz > 0 && (idx / (rate_hz * 14)) % 4 == 1 &&
	     (idx % (rate_hz * 14)) < rate_hz / 2) {
		 double m = (double)(idx % (rate_hz / 2));

		 motion = 3800.0 * env * exp(-m / 8.0) * ((idx & 8) ? 1.0 : -1.0);
	 }

	 v = baseline + hemo + cardiac + resp + mayer + noise + motion;
	 if (v > 32767.0)
		 v = 32767.0;
	 if (v < -32768.0)
		 v = -32768.0;
	 return (int16_t)v;
 }

 static uint16_t stream_pick_sample_count(struct server *server, uint16_t mtu)
 {
	 uint16_t max_payload, nominal, count;

	 max_payload = (mtu > 14) ? (uint16_t)(mtu - 3 - 11) : 8;
	 nominal = (uint16_t)(server->stream_rate_hz * server->stream_period_ms / 1000);
	 if (nominal < 1)
		 nominal = 1;
	 count = nominal;
	 if ((size_t)count * 2 > max_payload)
		 count = max_payload / 2;
	 if (count > STREAM_MAX_SAMPLES_PER_PKT)
		 count = STREAM_MAX_SAMPLES_PER_PKT;
	 if (count < 1)
		 count = 1;
	 return count;
 }

 static void stream_abort(struct server *server)
 {
	 uint8_t stop_pkt[5];

	 if (!server)
		 return;

	 if (server->stream_timer_id) {
		 timeout_remove(server->stream_timer_id);
		 server->stream_timer_id = 0;
	 }

	 if (server->stream_active && server->custom_notify_enabled && server->gatt) {
		 stop_pkt[0] = STREAM_OP_STOP;
		 put_le32_local(stop_pkt + 1, server->stream_sample_idx);
		 file_tx_send_notify(server, stop_pkt, sizeof(stop_pkt));
	 }

	 stream_close_rec_file(server);

	 if (!fnirs_stream_use_sim() && server->stream_active)
		 fnirs_stream_ipc_stop(FNIRS_STREAM_CTL_STOP);

	 server->stream_active = false;
	 server->stream_seq = 0;
	 server->stream_sample_idx = 0;
	 server->stream_rate_hz = 0;
	 server->stream_period_ms = 0;
	 server->stream_glitch_next_idx = 0;
 }

 static bool stream_pump_cb(void *user_data)
 {
	 struct server *server = user_data;
	 uint8_t pkt[11 + STREAM_MAX_SAMPLES_PER_PKT * 2];
	 uint16_t mtu, count, i;
	 size_t pkt_len;

	 server->stream_timer_id = 0;

	 if (!server || !server->stream_active || !server->custom_notify_enabled)
		 return false;

#if STREAM_DEMO_GLITCH
	 if (fnirs_stream_use_sim() &&
	     server->stream_rate_hz > 0 &&
	     server->stream_sample_idx >= server->stream_glitch_next_idx) {
		 server->stream_glitch_next_idx +=
			 server->stream_rate_hz * STREAM_GLITCH_INTERVAL_SEC;
		 printf("STREAM demo glitch: pause %u ms at sample %u "
			"(no file/notify until resume)\n",
			STREAM_GLITCH_PAUSE_MS, server->stream_sample_idx);
		 server->stream_timer_id = timeout_add(STREAM_GLITCH_PAUSE_MS,
						       stream_pump_cb, server, NULL);
		 return false;
	 }
#endif

	 mtu = bt_att_get_mtu(server->att);
	 count = stream_pick_sample_count(server, mtu);

	 if (!fnirs_stream_use_sim()) {
		 int16_t batch[STREAM_MAX_SAMPLES_PER_PKT];
		 int got;

		 count = STREAM_MAX_SAMPLES_PER_PKT;
		 got = fnirs_stream_read_samples(batch, count);

		 if (got <= 0) {
			 static uint32_t empty_pumps;

			 if (++empty_pumps == 1 || (empty_pumps % 200) == 0)
				 printf("STREAM pump: fifo empty (x%u)\n",
					empty_pumps);
			 server->stream_timer_id = timeout_add(server->stream_period_ms,
							       stream_pump_cb, server, NULL);
			 return false;
		 }

		 count = (uint16_t)got;
		 pkt[0] = STREAM_OP_DATA;
		 put_le32_local(pkt + 1, server->stream_seq++);
		 put_le32_local(pkt + 5, server->stream_sample_idx);
		 put_le16_local(pkt + 9, count);

		 for (i = 0; i < count; i++)
			 put_le16_local(pkt + 11 + i * 2, (uint16_t)batch[i]);

		 server->stream_sample_idx += count;
		 pkt_len = 11 + count * 2;

		 if (server->stream_rec_fp) {
			 if (fwrite(pkt + 11, 1, (size_t)count * 2,
				    server->stream_rec_fp) != (size_t)count * 2)
				 printf("STREAM record fwrite failed\n");
		 }

		 if (!file_tx_send_notify(server, pkt, pkt_len)) {
			 printf("STREAM notify failed seq=%u\n",
				server->stream_seq - 1);
			 server->stream_timer_id = timeout_add(
				 server->stream_period_ms, stream_pump_cb,
				 server, NULL);
			 return false;
		 }

		 server->stream_timer_id = timeout_add(server->stream_period_ms,
						       stream_pump_cb, server, NULL);
		 return false;
	 }

	 pkt[0] = STREAM_OP_DATA;
	 put_le32_local(pkt + 1, server->stream_seq++);
	 put_le32_local(pkt + 5, server->stream_sample_idx);
	 put_le16_local(pkt + 9, count);

	 for (i = 0; i < count; i++) {
		 int16_t v = stream_fnirs_sim_sample(
			 server->stream_sample_idx + i,
			 server->stream_rate_hz);

		 put_le16_local(pkt + 11 + i * 2, (uint16_t)v);
	 }
	 server->stream_sample_idx += count;
	 pkt_len = 11 + count * 2;

	 if (server->stream_rec_fp) {
		 if (fwrite(pkt + 11, 1, (size_t)count * 2, server->stream_rec_fp) !=
		     (size_t)count * 2)
			 printf("STREAM record fwrite failed\n");
	 }

	 if (!file_tx_send_notify(server, pkt, pkt_len)) {
		 printf("STREAM notify failed seq=%u\n", server->stream_seq - 1);
		 server->stream_timer_id = timeout_add(server->stream_period_ms,
						       stream_pump_cb, server, NULL);
		 return false;
	 }

	 server->stream_timer_id = timeout_add(server->stream_period_ms,
					       stream_pump_cb, server, NULL);
	 return false;
 }

 static void file_tx_abort(struct server *server)
 {
	 if (server->tx_kick_id) {
		 timeout_remove(server->tx_kick_id);
		 server->tx_kick_id = 0;
	 }
	 if (server->tx_timer_id) {
		 timeout_remove(server->tx_timer_id);
		 server->tx_timer_id = 0;
	 }
	 if (server->tx_fp) {
		 fclose(server->tx_fp);
		 server->tx_fp = NULL;
	 }
	 server->tx_active = false;
	 server->tx_start_pending = false;
	 server->tx_start_len = 0;
	 server->tx_file_size = 0;
	 server->tx_file_crc = 0;
	 server->tx_sent_size = 0;
	 server->tx_next_seq = 0;
	 server->tx_name[0] = '\0';
	 server->tx_path[0] = '\0';
	 server->tx_fc_ready = false;
	 server->tx_fc_ack_seq = 0;
 }

 static void file_tx_indicate_conf_cb(void *user_data);

 static bool file_tx_send_notify(struct server *server, const uint8_t *data, size_t len)
 {
	 uint16_t mtu;
	 uint16_t max_payload;

	 if (!server || !server->custom_notify_enabled || !server->gatt || !data || len == 0)
		 return false;

	 mtu = bt_att_get_mtu(server->att);
	 max_payload = (mtu > 3) ? (mtu - 3) : 20;
	 if (len > max_payload)
		 len = max_payload;

	 return bt_gatt_server_send_notification(server->gatt,
						 server->custom_data_handle,
						 data, len, false) != 0;
 }

 static bool file_tx_send_indicate(struct server *server, const uint8_t *data, size_t len,
				   bool with_conf)
 {
	 uint16_t mtu;
	 uint16_t max_payload;

	 if (!server || !server->custom_notify_enabled || !server->gatt || !data || len == 0)
		 return false;

	 mtu = bt_att_get_mtu(server->att);
	 max_payload = (mtu > 3) ? (mtu - 3) : 20;
	 if (len > max_payload)
		 len = max_payload;

	 /*
	  * Indication: the client's BLE stack auto-confirms at the ATT layer,
	  * giving free, reliable, per-packet flow control (never send seq N+1
	  * before seq N is confirmed). Avoids the Android write/notify race
	  * seen when the phone writes an app-level ACK on the same
	  * characteristic while DATA notifications are in flight.
	  */
	 return bt_gatt_server_send_indication(server->gatt,
						server->custom_data_handle,
						data, len,
						with_conf ? file_tx_indicate_conf_cb : NULL,
						server, NULL);
 }

 /* FILE TX always uses Notify; phone ACK (0x26) provides flow control. */
 static bool file_tx_send_pkt(struct server *server, const uint8_t *data, size_t len)
 {
	 (void)len;
	 return file_tx_send_notify(server, data, len);
 }

 static bool file_tx_resolve_path(const char *name, char *out, size_t out_sz)
 {
	 struct stat st;

	 if (!name || !out || out_sz == 0)
		 return false;

	 if (name[0] == '/') {
		 snprintf(out, out_sz, "%s", name);
		 if (stat(out, &st) == 0 && S_ISREG(st.st_mode))
			 return true;
		 return false;
	 }

	 snprintf(out, out_sz, "%s/%s", file_tx_dir(), name);
	 if (stat(out, &st) == 0 && S_ISREG(st.st_mode))
		 return true;

	 snprintf(out, out_sz, "%s/%s", file_rx_dir(), name);
	 if (stat(out, &st) == 0 && S_ISREG(st.st_mode))
		 return true;

	 snprintf(out, out_sz, "/app_data/%s", name);
	 if (stat(out, &st) == 0 && S_ISREG(st.st_mode))
		 return true;

	 return false;
 }

 static bool file_tx_compute_meta(const char *path, uint32_t *size_out, uint32_t *crc_out)
 {
	 FILE *fp;
	 uint8_t buf[4096];
	 size_t n;
	 uint32_t size = 0;
	 uint32_t crc = 0;

	 fp = fopen(path, "rb");
	 if (!fp)
		 return false;

	 while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
		 size += n;
		 crc = crc32_update_local(crc, buf, n);
	 }

	 fclose(fp);
	 *size_out = size;
	 *crc_out = crc;
	 return true;
 }

 static void file_tx_finish(struct server *server, bool success)
 {
	 if (server->tx_timer_id) {
		 timeout_remove(server->tx_timer_id);
		 server->tx_timer_id = 0;
	 }
	 if (server->tx_fp) {
		 fclose(server->tx_fp);
		 server->tx_fp = NULL;
	 }
	 server->tx_active = false;
	 if (success) {
		 printf("FILE TX DONE: path=%s name=%s size=%u crc=0x%08x\n",
				server->tx_path, server->tx_name, server->tx_file_size,
				server->tx_file_crc);
	 }
 }

 static unsigned int file_tx_packet_delay_ms(struct server *server)
 {
	 uint16_t mtu;

	 if (!server || !server->att)
		 return FILE_TX_PACKET_DELAY_MS;

	 mtu = bt_att_get_mtu(server->att);
	 if (mtu > 0 && mtu <= 23)
		 return FILE_TX_PACKET_DELAY_SMALL_MTU_MS;

	 return FILE_TX_PACKET_DELAY_MS;
 }

static void file_tx_schedule_pump(struct server *server);
static void file_tx_schedule_pump_after(struct server *server, unsigned int delay_ms);
static bool file_tx_kick_cb(void *user_data);

static bool file_tx_pump_cb(void *user_data)
{
	struct server *server = user_data;
	uint8_t pkt[520];
	uint8_t chunk[512];
	size_t chunk_len;
	uint16_t mtu;
	uint16_t max_chunk;
	size_t hdr_off = 1;
	uint16_t frame_crc;

	server->tx_timer_id = 0;

	if (!server->tx_active || !server->tx_fp || !server->custom_notify_enabled)
		return false;

	if (!server->tx_fc_ready)
		return false;

	if (server->tx_next_seq >= server->tx_fc_ack_seq + FILE_TX_FC_WINDOW)
		return false;

	if (server->tx_sent_size >= server->tx_file_size) {
		uint8_t end_pkt[1] = { FILE_TX_OP_END };

		if (!file_tx_send_notify(server, end_pkt, 1)) {
			printf("FILE TX END notify failed, retry\n");
			server->tx_timer_id = timeout_add(file_tx_packet_delay_ms(server),
							  file_tx_pump_cb, server, NULL);
			return false;
		}
		file_tx_finish(server, true);
		return false;
	}

	mtu = bt_att_get_mtu(server->att);
	/* opcode(1) + seq(4) + len(2) + crc(2) + ATT hdr(3) = 12 bytes overhead */
	max_chunk = (mtu > 12 + FILE_TX_CHUNK_MTU_MARGIN) ?
		    (mtu - 3 - 9 - FILE_RX_DATA_CRC_LEN - FILE_TX_CHUNK_MTU_MARGIN) : 8;
	if (max_chunk < 1)
		max_chunk = 1;
	if (max_chunk > FILE_TX_MAX_CHUNK)
		max_chunk = FILE_TX_MAX_CHUNK;
	if (max_chunk > sizeof(chunk))
		max_chunk = sizeof(chunk);

	chunk_len = fread(chunk, 1, max_chunk, server->tx_fp);
	if (chunk_len == 0) {
		printf("FILE TX read error: path=%s sent=%u expected=%u\n",
			   server->tx_path, server->tx_sent_size, server->tx_file_size);
		file_tx_abort(server);
		return false;
	}

	pkt[0] = FILE_TX_OP_DATA;
	put_le32_local(pkt + hdr_off, server->tx_next_seq);
	put_le16_local(pkt + hdr_off + 4, (uint16_t)chunk_len);
	memcpy(pkt + hdr_off + 6, chunk, chunk_len);
	frame_crc = crc16_local(pkt + hdr_off, 6 + chunk_len);
	put_le16_local(pkt + hdr_off + 6 + chunk_len, frame_crc);

	/*
	 * Notify window + phone ACK(0x26). Board pauses between windows so the
	 * phone can drain its queue and write ACK without notify/write races.
	 */
	if (!file_tx_send_pkt(server, pkt, hdr_off + 6 + chunk_len + FILE_RX_DATA_CRC_LEN)) {
		printf("FILE TX notify failed seq=%u\n", server->tx_next_seq);
		server->tx_timer_id = timeout_add(file_tx_packet_delay_ms(server),
						  file_tx_pump_cb, server, NULL);
		return false;
	}

	server->tx_sent_size += chunk_len;
	server->tx_next_seq++;

	if (server->tx_sent_size == chunk_len) {
		size_t pkt_len = hdr_off + 6 + chunk_len + FILE_RX_DATA_CRC_LEN;
		printf("FILE TX DATA: first chunk %zu bytes (pkt=%zu mtu=%u max_chunk=%u)\n",
		       chunk_len, pkt_len, mtu, max_chunk);
	} else if ((server->tx_sent_size / (64 * 1024)) !=
		 ((server->tx_sent_size - chunk_len) / (64 * 1024)))
		printf("FILE TX DATA: %u / %u bytes\n",
			   server->tx_sent_size, server->tx_file_size);

	if (server->tx_sent_size < server->tx_file_size &&
	    server->tx_next_seq < server->tx_fc_ack_seq + FILE_TX_FC_WINDOW)
		file_tx_schedule_pump(server);

	return false;
}

static void file_tx_indicate_conf_cb(void *user_data)
{
	(void)user_data;
}

static void file_tx_schedule_pump(struct server *server)
{
	if (server->tx_timer_id)
		timeout_remove(server->tx_timer_id);
	server->tx_timer_id = timeout_add(file_tx_packet_delay_ms(server),
					  file_tx_pump_cb, server, NULL);
}

static void file_tx_schedule_pump_after(struct server *server, unsigned int delay_ms)
{
	if (server->tx_timer_id)
		timeout_remove(server->tx_timer_id);
	server->tx_timer_id = timeout_add(delay_ms, file_tx_pump_cb, server, NULL);
}

static void file_tx_schedule_kick(struct server *server)
{
	if (server->tx_kick_id)
		timeout_remove(server->tx_kick_id);
	server->tx_kick_id = timeout_add(FILE_TX_KICK_DELAY_MS,
				   file_tx_kick_cb, server, NULL);
}

/* Defer START indicate out of ATT write_cb (sync send often fails). */
static bool file_tx_kick_cb(void *user_data)
{
	struct server *server = user_data;

	server->tx_kick_id = 0;

	if (!server->tx_active || !server->custom_notify_enabled || !server->tx_fp)
		return false;

	if (server->tx_start_pending) {
		bool ok;

		ok = file_tx_send_notify(server, server->tx_start_pkt,
					 server->tx_start_len);

		if (!ok) {
			printf("FILE TX START notify failed mtu=%u len=%zu\n",
			       bt_att_get_mtu(server->att), server->tx_start_len);
			file_tx_abort(server);
			return false;
		}

		server->tx_start_pending = false;
		server->tx_fc_ready = false;
		server->tx_fc_ack_seq = 0;
		printf("FILE TX START: path=%s size=%u crc=0x%08x att_mtu=%u "
		       "(window=%u wait ACK 0)\n",
		       server->tx_path, server->tx_file_size, server->tx_file_crc,
		       bt_att_get_mtu(server->att), FILE_TX_FC_WINDOW);
	}

	return false;
}

static bool file_live_query_record(char *path, size_t path_size,
				   uint32_t *bytes, uint32_t *crc32, int *state)
{
#ifdef FNIRS_EMBEDDED
	fdatalog_sync_state_t sync_state = FDATALOG_SYNC_NONE;

	if (fdatalog_get_sync_info(path, (uint32_t)path_size, bytes, crc32,
				   &sync_state) != 0)
		return false;
	*state = (int)sync_state;
	return true;
#else
	(void)path;
	(void)path_size;
	(void)bytes;
	(void)crc32;
	(void)state;
	return false;
#endif
}

static void file_live_abort(struct server *server)
{
	if (!server)
		return;
	if (server->live_kick_id) {
		timeout_remove(server->live_kick_id);
		server->live_kick_id = 0;
	}
	if (server->live_timer_id) {
		timeout_remove(server->live_timer_id);
		server->live_timer_id = 0;
	}
	if (server->live_fp) {
		fclose(server->live_fp);
		server->live_fp = NULL;
	}
	server->live_active = false;
	server->live_start_pending = false;
	server->live_fc_ready = false;
	server->live_sealed = false;
	server->live_done_sent = false;
	server->live_start_len = 0;
	server->live_sent_offset = 0;
	server->live_next_seq = 0;
	server->live_ack_offset = 0;
	server->live_ack_seq = 0;
	server->live_high_offset = 0;
	server->live_high_seq = 0;
	server->live_final_size = 0;
	server->live_final_crc = 0;
	server->live_eof_polls = 0;
	server->live_name[0] = '\0';
	server->live_path[0] = '\0';
}

static void file_live_finish(struct server *server)
{
	if (server->live_timer_id) {
		timeout_remove(server->live_timer_id);
		server->live_timer_id = 0;
	}
	if (server->live_fp) {
		fclose(server->live_fp);
		server->live_fp = NULL;
	}
	server->live_active = false;
	printf("FILE LIVE DONE: path=%s size=%u crc=0x%08x\n",
	       server->live_path, server->live_final_size,
	       server->live_final_crc);
}

static void file_live_schedule_after(struct server *server,
				     unsigned int delay_ms);

static bool file_live_rewind_to_ack(struct server *server, const char *reason)
{
	if (!server || !server->live_fp ||
	    (server->live_ack_seq == server->live_next_seq &&
	     server->live_ack_offset == server->live_sent_offset))
		return false;

	clearerr(server->live_fp);
	if (fseek(server->live_fp, (long)server->live_ack_offset, SEEK_SET) != 0) {
		perror("FILE LIVE retry seek");
		return false;
	}

	printf("FILE LIVE RETRY (%s): cursor=%u/%u -> ack=%u/%u high=%u/%u\n",
	       reason, server->live_next_seq, server->live_sent_offset,
	       server->live_ack_seq, server->live_ack_offset,
	       server->live_high_seq, server->live_high_offset);
	server->live_next_seq = server->live_ack_seq;
	server->live_sent_offset = server->live_ack_offset;
	server->live_eof_polls = 0;
	return true;
}

static bool file_live_send_final(struct server *server, uint8_t phase)
{
	uint8_t pkt[10];

	pkt[0] = FILE_LIVE_OP_FINAL;
	pkt[1] = phase;
	put_le32_local(pkt + 2, server->live_final_size);
	put_le32_local(pkt + 6, server->live_final_crc);
	return file_tx_send_notify(server, pkt, sizeof(pkt));
}

static bool file_live_pump_cb(void *user_data)
{
	struct server *server = user_data;
	uint8_t pkt[520];
	uint8_t chunk[512];
	char current_path[256];
	uint32_t available = 0;
	uint32_t current_crc = 0;
	uint32_t remaining;
	size_t chunk_len;
	uint16_t mtu;
	uint16_t max_chunk;
	uint16_t frame_crc;
	int rec_state = 0;

	server->live_timer_id = 0;

	if (!server->live_active || !server->live_fp ||
	    !server->custom_notify_enabled)
		return false;
	if (!server->live_fc_ready)
		return false;

	if (!file_live_query_record(current_path, sizeof(current_path),
				    &available, &current_crc, &rec_state) ||
	    strcmp(current_path, server->live_path) != 0) {
		printf("FILE LIVE record disappeared/changed: %s\n",
		       server->live_path);
		{
			uint8_t abort_pkt[2] = { FILE_LIVE_OP_ABORT, 1 };
			(void)file_tx_send_notify(server, abort_pkt,
						 sizeof(abort_pkt));
		}
		file_live_abort(server);
		return false;
	}

	if (rec_state == FILE_LIVE_REC_ERROR) {
		uint8_t abort_pkt[2] = { FILE_LIVE_OP_ABORT, 2 };

		printf("FILE LIVE recording failed: %s\n", server->live_path);
		(void)file_tx_send_notify(server, abort_pkt, sizeof(abort_pkt));
		file_live_abort(server);
		return false;
	}

	if (rec_state == FILE_LIVE_REC_FINALIZED && !server->live_sealed) {
		server->live_final_size = available;
		server->live_final_crc = current_crc;
		if (!file_live_send_final(server, FILE_LIVE_FINAL_SEALED)) {
			file_live_schedule_after(server, FILE_LIVE_POLL_MS);
			return false;
		}
		server->live_sealed = true;
		printf("FILE LIVE SEALED: size=%u crc=0x%08x sent=%u\n",
		       server->live_final_size, server->live_final_crc,
		       server->live_sent_offset);
	}

	if (server->live_sent_offset > available) {
		printf("FILE LIVE invalid shrink: sent=%u available=%u\n",
		       server->live_sent_offset, available);
		file_live_abort(server);
		return false;
	}

	if (server->live_done_sent) {
		/*
		 * DONE is a Notify too. Keep retransmitting until the phone sends
		 * one more ACK after validating the complete local file.
		 */
		if (!file_live_send_final(server, FILE_LIVE_FINAL_DONE)) {
			file_live_schedule_after(server, FILE_LIVE_POLL_MS);
			return false;
		}
		file_live_schedule_after(server, 500);
		return false;
	}

	if (server->live_sealed &&
	    server->live_ack_offset >= server->live_final_size) {
		if (!file_live_send_final(server, FILE_LIVE_FINAL_DONE)) {
			file_live_schedule_after(server, FILE_LIVE_POLL_MS);
			return false;
		}
		server->live_done_sent = true;
		file_live_schedule_after(server, 500);
		return false;
	}

	if (server->live_next_seq >=
	    server->live_ack_seq + FILE_TX_FC_WINDOW) {
		if (!file_live_rewind_to_ack(server, "window timeout")) {
			file_live_schedule_after(server, FILE_LIVE_RETRY_MS);
			return false;
		}
	}

	if (server->live_sent_offset >= available) {
		if (server->live_ack_offset < server->live_sent_offset) {
			server->live_eof_polls++;
			if (server->live_eof_polls >=
			    FILE_LIVE_EOF_RETRY_POLLS &&
			    file_live_rewind_to_ack(server, "EOF timeout")) {
				/* Continue below and retransmit from the cumulative ACK. */
			} else {
				file_live_schedule_after(server, FILE_LIVE_POLL_MS);
				return false;
			}
		} else {
			server->live_eof_polls = 0;
			file_live_schedule_after(server, FILE_LIVE_POLL_MS);
			return false;
		}
	}

	if (server->live_sent_offset >= available) {
		file_live_schedule_after(server, FILE_LIVE_POLL_MS);
		return false;
	}

	mtu = bt_att_get_mtu(server->att);
	/*
	 * op(1)+seq(4)+offset(4)+len(2)+crc(2)+ATT(3) = 16 bytes.
	 */
	max_chunk = (mtu > 16 + FILE_TX_CHUNK_MTU_MARGIN) ?
		    (mtu - 3 - 11 - FILE_RX_DATA_CRC_LEN -
		     FILE_TX_CHUNK_MTU_MARGIN) : 4;
	if (max_chunk < 1)
		max_chunk = 1;
	if (max_chunk > FILE_TX_MAX_CHUNK)
		max_chunk = FILE_TX_MAX_CHUNK;
	if (max_chunk > sizeof(chunk))
		max_chunk = sizeof(chunk);

	remaining = available - server->live_sent_offset;
	if (max_chunk > remaining)
		max_chunk = (uint16_t)remaining;

	clearerr(server->live_fp);
	if (fseek(server->live_fp, (long)server->live_sent_offset, SEEK_SET) != 0) {
		perror("FILE LIVE seek");
		file_live_abort(server);
		return false;
	}
	chunk_len = fread(chunk, 1, max_chunk, server->live_fp);
	if (chunk_len == 0) {
		clearerr(server->live_fp);
		file_live_schedule_after(server, FILE_LIVE_POLL_MS);
		return false;
	}

	pkt[0] = FILE_LIVE_OP_DATA;
	put_le32_local(pkt + 1, server->live_next_seq);
	put_le32_local(pkt + 5, server->live_sent_offset);
	put_le16_local(pkt + 9, (uint16_t)chunk_len);
	memcpy(pkt + 11, chunk, chunk_len);
	frame_crc = crc16_local(pkt + 1, 10 + chunk_len);
	put_le16_local(pkt + 11 + chunk_len, frame_crc);

	if (!file_tx_send_notify(server, pkt, 13 + chunk_len)) {
		file_live_schedule_after(server, FILE_LIVE_PACKET_DELAY_MS);
		return false;
	}

	server->live_sent_offset += (uint32_t)chunk_len;
	server->live_next_seq++;
	if (server->live_next_seq > server->live_high_seq) {
		server->live_high_seq = server->live_next_seq;
		server->live_high_offset = server->live_sent_offset;
	}

	if (server->live_sent_offset == (uint32_t)chunk_len ||
	    (server->live_sent_offset / (64 * 1024)) !=
	    ((server->live_sent_offset - chunk_len) / (64 * 1024))) {
		printf("FILE LIVE DATA: sent=%u produced=%u seq=%u\n",
		       server->live_sent_offset, available,
		       server->live_next_seq);
	}

	file_live_schedule_after(
		server,
		server->live_next_seq <
			server->live_ack_seq + FILE_TX_FC_WINDOW ?
			FILE_LIVE_PACKET_DELAY_MS : FILE_LIVE_RETRY_MS);

	return false;
}

static void file_live_schedule_after(struct server *server,
				     unsigned int delay_ms)
{
	if (!server || !server->live_active)
		return;
	if (server->live_timer_id)
		timeout_remove(server->live_timer_id);
	server->live_timer_id = timeout_add(delay_ms, file_live_pump_cb,
					    server, NULL);
}

static bool file_live_kick_cb(void *user_data)
{
	struct server *server = user_data;

	server->live_kick_id = 0;
	if (!server->live_active || !server->live_start_pending ||
	    !server->custom_notify_enabled || !server->live_fp)
		return false;

	if (!file_tx_send_notify(server, server->live_start_pkt,
				 server->live_start_len)) {
		printf("FILE LIVE START notify failed\n");
		file_live_abort(server);
		return false;
	}

	server->live_start_pending = false;
	server->live_fc_ready = false;
	printf("FILE LIVE START: path=%s (wait ACK 0/0)\n",
	       server->live_path);
	return false;
}

static void file_live_schedule_kick(struct server *server)
{
	if (server->live_kick_id)
		timeout_remove(server->live_kick_id);
	server->live_kick_id = timeout_add(FILE_TX_KICK_DELAY_MS,
					   file_live_kick_cb,
					   server, NULL);
}

/*
  * Custom write protocol on UUID 7956...:
  *   0x01 START: [opcode][name_len:1][name][size_le32][crc32_le32]
  *   0x02 DATA : [opcode][seq_le32][chunk_len_le16][chunk...][frame_crc16_le16]
  *               frame_crc16 = CRC16(seq+chunk_len+chunk), init 0xFFFF (same as fNIRS)
  *   0x03 END  : [opcode]
  *   0x11 ABORT: [opcode]
  */
static void custom_data_write_cb(struct gatt_db_attribute *attrib, unsigned int id,
				  uint16_t offset, const uint8_t *value, size_t len,
				  uint8_t opcode, struct bt_att *att, void *user_data)
{
	struct server *server = user_data;
	uint8_t ecode = 0;
	uint8_t status[5] = { 0 };
	static unsigned long write_cb_calls;

	(void)attrib;
	(void)opcode;
	(void)att;

	/* Diagnostic: prove whether ATT writes are reaching this callback at
	 * all. First 5 calls always logged, then every 200th, so a stall can
	 * be told apart from "callback never fires again" vs "fires but data
	 * rejected silently". Remove once the RX-stall issue is root-caused.
	 */
	write_cb_calls++;
	if (write_cb_calls <= 5 || (write_cb_calls % 200) == 0) {
		printf("WRITE_CB #%lu: len=%zu offset=%u op=0x%02x rx_active=%d next_seq=%u\n",
		       write_cb_calls, len, offset, (len > 0 && value) ? value[0] : 0xFF,
		       server->rx_active, server->rx_next_seq);
	}

	if (!value || len < 1 || offset != 0) {
		ecode = BT_ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LEN;
		goto done;
	 }
 
	 /* Bridge passthrough (only when file RX/TX/stream sessions are idle) */
	 if (!server->rx_active && !server->tx_active && !server->live_active &&
	     !server->stream_active) {
		 if (value[0] == BRIDGE_OP_PASSTHROUGH && len > 1) {
			 bridge_dispatch_input(server, BRIDGE_SRC_BLE, value + 1, len - 1);
			 goto done;
		 }
		 if (!bridge_is_ble_file_opcode(value[0])) {
			 bridge_dispatch_input(server, BRIDGE_SRC_BLE, value, len);
			 goto done;
		 }
	 }

	 if (server->tx_active && value[0] != FILE_TX_OP_ABORT &&
	     value[0] != FILE_TX_OP_ACK && value[0] != STREAM_OP_START &&
	     value[0] != TIME_OP_SET && value[0] < FNIRS_OP_SCAN) {
		 ecode = BT_ATT_ERROR_UNLIKELY;
		 goto done;
	 }

	 if (server->stream_active && value[0] != STREAM_OP_ABORT &&
	     value[0] != STREAM_OP_START && value[0] != TIME_OP_SET &&
	     !bridge_is_live_file_opcode(value[0]) &&
	     (value[0] < FNIRS_OP_SCAN || value[0] > FNIRS_OP_RECORD_STAT)) {
		 ecode = BT_ATT_ERROR_UNLIKELY;
		 goto done;
	 }

	 if (server->live_active &&
	     value[0] != FILE_LIVE_OP_ACK &&
	     value[0] != FILE_LIVE_OP_ABORT &&
	     value[0] != STREAM_OP_ABORT &&
	     value[0] != STREAM_OP_START &&
	     value[0] != TIME_OP_SET &&
	     (value[0] < FNIRS_OP_SCAN || value[0] > FNIRS_OP_RECORD_STAT)) {
		 ecode = BT_ATT_ERROR_UNLIKELY;
		 goto done;
	 }

	 if (server->rx_active && bridge_is_ble_file_opcode(value[0]) &&
	     value[0] != FILE_RX_OP_ABORT && value[0] != FILE_RX_OP_DATA &&
	     value[0] != FILE_RX_OP_END) {
		 if (value[0] == FILE_TX_OP_REQ || value[0] == FILE_TX_OP_ABORT) {
			 ecode = BT_ATT_ERROR_UNLIKELY;
			 goto done;
		 }
	 }
 
	 switch (value[0]) {
	 case FNIRS_OP_SCAN: {
		 uint8_t st, alive[12];
		 uint16_t rsp_len = 0;

		 printf("FNIRS SCAN request\n");
		 fflush(stdout);
		 if (!fnirs_ble_ipc_request(FNIRS_BLE_CMD_SCAN, NULL, 0, &st,
					    alive, sizeof(alive), &rsp_len)) {
			 printf("FNIRS SCAN: ipc failed (my-fnirs socket?)\n");
			 fnirs_ble_notify_rsp(server, FNIRS_OP_SCAN, 1, NULL, 0);
		 } else {
			 printf("FNIRS SCAN: ok, rsp_len=%u\n", rsp_len);
			 fnirs_ble_notify_rsp(server, FNIRS_OP_SCAN, st,
					      alive, rsp_len);
		 }
		 break;
	 }

	 case FNIRS_OP_SAMPLE_ON: {
		 uint8_t st, rc = 0;
		 uint16_t rsp_len = 0;

		 fnirs_ble_ipc_request(FNIRS_BLE_CMD_SAMPLE_ON, NULL, 0, &st,
				       &rc, 1, &rsp_len);
		 fnirs_ble_notify_rsp(server, FNIRS_OP_SAMPLE_ON, st, &rc,
				      rsp_len);
		 break;
	 }

	 case FNIRS_OP_SAMPLE_OFF: {
		 uint8_t st, rc = 0;
		 uint16_t rsp_len = 0;

		 fnirs_ble_ipc_request(FNIRS_BLE_CMD_SAMPLE_OFF, NULL, 0, &st,
				       &rc, 1, &rsp_len);
		 fnirs_ble_notify_rsp(server, FNIRS_OP_SAMPLE_OFF, st, &rc,
				      rsp_len);
		 break;
	 }

	 case FNIRS_OP_GAIN: {
		 uint8_t st, rc = 0;
		 uint16_t rsp_len = 0;

		 if (len < 2) {
			 ecode = BT_ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LEN;
			 break;
		 }
		 fnirs_ble_ipc_request(FNIRS_BLE_CMD_SET_GAIN, value + 1, 1, &st,
				       &rc, 1, &rsp_len);
		 fnirs_ble_notify_rsp(server, FNIRS_OP_GAIN, st, &rc, rsp_len);
		 break;
	 }

	 case FNIRS_OP_LED_ARRAY: {
		 uint8_t st, rc = 0;
		 uint16_t rsp_len = 0;

		 if (len < 2) {
			 ecode = BT_ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LEN;
			 break;
		 }
		 fnirs_ble_ipc_request(FNIRS_BLE_CMD_SET_LED_ARRAY, value + 1,
				       (uint16_t)(len - 1), &st, &rc, 1,
				       &rsp_len);
		 fnirs_ble_notify_rsp(server, FNIRS_OP_LED_ARRAY, st, &rc, rsp_len);
		 break;
	 }

	 case FNIRS_OP_STREAM_CH: {
		 uint8_t st, rc = 0;
		 uint16_t rsp_len = 0;

		 if (len < 2) {
			 ecode = BT_ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LEN;
			 break;
		 }
		 fnirs_ble_ipc_request(FNIRS_BLE_CMD_SET_STREAM_CH, value + 1,
				       (uint16_t)(len - 1), &st, &rc, 1,
				       &rsp_len);
		 fnirs_ble_notify_rsp(server, FNIRS_OP_STREAM_CH, st, &rc, rsp_len);
		 break;
	 }

	 case FNIRS_OP_HANGZHOU: {
		 uint8_t st;
		 char path[192];
		 uint16_t rsp_len = 0;

		 memset(path, 0, sizeof(path));
		 if (!fnirs_ble_ipc_request(FNIRS_BLE_CMD_HANGZHOU_PATH, NULL, 0,
					    &st, (uint8_t *)path, sizeof(path) - 1,
					    &rsp_len))
			 fnirs_ble_notify_rsp(server, FNIRS_OP_HANGZHOU, 1, NULL, 0);
		 else
			 fnirs_ble_notify_rsp(server, FNIRS_OP_HANGZHOU, st,
					      (uint8_t *)path, rsp_len);
		 break;
	 }

	 case FNIRS_OP_RECORD_STAT: {
		 uint8_t st;
		 uint8_t stat[8];
		 uint16_t rsp_len = 0;

		 if (!fnirs_ble_ipc_request(FNIRS_BLE_CMD_RECORD_STAT, NULL, 0,
					    &st, stat, sizeof(stat), &rsp_len))
			 fnirs_ble_notify_rsp(server, FNIRS_OP_RECORD_STAT, 1, NULL, 0);
		 else
			 fnirs_ble_notify_rsp(server, FNIRS_OP_RECORD_STAT, st,
					      stat, rsp_len);
		 break;
	 }

	 case STREAM_OP_START: {
		 uint32_t rate_hz;
		 uint16_t period_ms;

		 if (server->rx_active) {
			 ecode = BT_ATT_ERROR_UNLIKELY;
			 break;
		 }

		 if (!server->custom_notify_enabled) {
			 printf("STREAM START rejected: CCCD not enabled\n");
			 ecode = BT_ATT_ERROR_UNLIKELY;
			 break;
		 }

		 rate_hz = (len >= 5) ? get_le32_local(value + 1) :
			   STREAM_RATE_HZ_DEFAULT;
		 period_ms = (len >= 7) ? get_le16_local(value + 5) :
			     STREAM_PERIOD_MS_DEFAULT;
		 if (rate_hz < 1)
			 rate_hz = STREAM_RATE_HZ_DEFAULT;
		 if (rate_hz > STREAM_RATE_HZ_MAX)
			 rate_hz = STREAM_RATE_HZ_MAX;
		 if (period_ms < STREAM_PERIOD_MS_MIN)
			 period_ms = STREAM_PERIOD_MS_MIN;
		 if (period_ms > 500)
			 period_ms = 500;

		 if (server->tx_active)
			 file_tx_abort(server);
		 if (server->live_active)
			 file_live_abort(server);
		 stream_abort(server);
		 server->stream_seq = 0;
		 server->stream_sample_idx = 0;
		 server->stream_glitch_next_idx =
			 rate_hz * STREAM_GLITCH_INTERVAL_SEC;
		 server->stream_active = true;

		 if (!fnirs_stream_use_sim()) {
			 rate_hz = STREAM_RATE_HZ_REAL;
			 period_ms = STREAM_PERIOD_MS_REAL;
			 fnirs_stream_ipc_start();
		 }

		 server->stream_rate_hz = rate_hz;
		 server->stream_period_ms = period_ms;
		 if (!stream_open_rec_file(server))
			 printf("STREAM record file disabled (open failed)\n");
		 if (fnirs_stream_use_sim()) {
			 printf("STREAM START (sim): rate=%u Hz period=%u ms "
				"(~%u B/s, ~%u samples/pkt)\n",
				rate_hz, period_ms,
				(unsigned)(rate_hz * 2u),
				(unsigned)(rate_hz * period_ms / 1000u));
		 } else {
			 printf("STREAM START (real fNIRS): rate=%u Hz period=%u ms\n",
				rate_hz, period_ms);
		 }
		 server->stream_timer_id = timeout_add(server->stream_period_ms,
						       stream_pump_cb, server, NULL);
		 break;
	 }

	 case STREAM_OP_ABORT:
		 printf("STREAM ABORT: samples=%u\n", server->stream_sample_idx);
		 /* stream_abort() stops ipc only when stream_active was true */
		 stream_abort(server);
		 break;

	 case TIME_OP_SET: {
		 uint32_t unix_sec;

		 if (len < 5) {
			 ecode = BT_ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LEN;
			 break;
		 }
		 unix_sec = get_le32_local(value + 1);
		 if (!board_set_unix_time(unix_sec)) {
			 printf("TIME SET rejected: %u\n", unix_sec);
			 ecode = BT_ATT_ERROR_UNLIKELY;
			 break;
		 }
		 break;
	 }

	 case FILE_TX_LAST_REQ: {
		 char name_buf[128];
		 uint8_t rsp[130];
		 size_t name_len;

		 if (!server->custom_notify_enabled) {
			 printf("FILE TX LAST rejected: CCCD not enabled\n");
			 ecode = BT_ATT_ERROR_UNLIKELY;
			 break;
		 }

		 if (!file_tx_resolve_last_recording(name_buf, sizeof(name_buf))) {
			 printf("FILE TX LAST: no recording in %s\n", file_tx_dir());
			 ecode = BT_ATT_ERROR_UNLIKELY;
			 break;
		 }

		 name_len = strlen(name_buf);
		 if (name_len == 0 || name_len > 127) {
			 ecode = BT_ATT_ERROR_UNLIKELY;
			 break;
		 }

		 rsp[0] = FILE_TX_LAST_RSP;
		 rsp[1] = (uint8_t)name_len;
		 memcpy(rsp + 2, name_buf, name_len);
		 if (!file_tx_send_notify(server, rsp, 2 + name_len)) {
			 ecode = BT_ATT_ERROR_UNLIKELY;
			 break;
		 }
		 printf("FILE TX LAST rsp: %s\n", name_buf);
		 break;
	 }

	 case FILE_TX_OP_REQ: {
		 uint8_t name_len;
		 char name_buf[128] = { 0 };
		 size_t name_off = 2;
		 size_t i;

		 if (server->rx_active || server->tx_active ||
		     server->live_active || server->stream_active) {
			 ecode = BT_ATT_ERROR_UNLIKELY;
			 break;
		 }

		 if (len < 2) {
			 ecode = BT_ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LEN;
			 break;
		 }

		 if (!server->custom_notify_enabled) {
			 printf("FILE TX REQ rejected: CCCD not enabled\n");
			 ecode = BT_ATT_ERROR_UNLIKELY;
			 break;
		 }

		 name_len = value[1];
		 if ((size_t)name_len > sizeof(name_buf) - 1 || len < (size_t)name_len + 2) {
			 ecode = BT_ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LEN;
			 break;
		 }

		 memcpy(name_buf, value + 2, name_len);
		 for (i = 0; i < name_len; i++) {
			 if (name_buf[i] == '/' || name_buf[i] == '\\' || name_buf[i] == '\0')
				 name_buf[i] = '_';
		 }
		 if (name_len == 0) {
			 ecode = BT_ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LEN;
			 break;
		 }

		 file_tx_abort(server);

		 if (!file_tx_resolve_path(name_buf, server->tx_path, sizeof(server->tx_path))) {
			 printf("FILE TX REQ: file not found: %s\n", name_buf);
			 ecode = BT_ATT_ERROR_UNLIKELY;
			 break;
		 }

		 if (!file_tx_compute_meta(server->tx_path, &server->tx_file_size,
					   &server->tx_file_crc)) {
			 perror("FILE TX meta");
			 ecode = BT_ATT_ERROR_UNLIKELY;
			 break;
		 }

		 server->tx_fp = fopen(server->tx_path, "rb");
		 if (!server->tx_fp) {
			 perror("fopen file tx");
			 ecode = BT_ATT_ERROR_UNLIKELY;
			 break;
		 }

		 strncpy(server->tx_name, name_buf, sizeof(server->tx_name) - 1);
		 server->tx_sent_size = 0;
		 server->tx_next_seq = 0;
		 server->tx_active = true;
		 server->tx_start_pending = true;
		 server->tx_fc_ready = false;
		 server->tx_fc_ack_seq = 0;

		 server->tx_start_pkt[0] = FILE_TX_OP_START;
		 server->tx_start_pkt[1] = (uint8_t)strlen(server->tx_name);
		 memcpy(server->tx_start_pkt + name_off, server->tx_name,
				server->tx_start_pkt[1]);
		 server->tx_start_len = name_off + server->tx_start_pkt[1];
		 put_le32_local(server->tx_start_pkt + server->tx_start_len,
				server->tx_file_size);
		 put_le32_local(server->tx_start_pkt + server->tx_start_len + 4,
				server->tx_file_crc);
		 server->tx_start_len += 8;

		 printf("FILE TX REQ ok: path=%s size=%u (defer START notify)\n",
				server->tx_path, server->tx_file_size);
		 file_tx_schedule_kick(server);
		 break;
	 }

	 case FILE_TX_OP_ABORT:
		 printf("FILE TX ABORT: path=%s sent=%u/%u\n",
				server->tx_path, server->tx_sent_size, server->tx_file_size);
		 file_tx_abort(server);
		 break;

	 case FILE_TX_OP_ACK: {
		 uint32_t ack_seq;

		 if (!server->tx_active || len < 5)
			 break;

		 ack_seq = get_le32_local(value + 1);
		 if (ack_seq > server->tx_next_seq + FILE_TX_FC_WINDOW)
			 break;

		 server->tx_fc_ack_seq = ack_seq;
		 server->tx_fc_ready = true;
		 if (server->tx_timer_id)
			 timeout_remove(server->tx_timer_id);
		 server->tx_timer_id = timeout_add(FILE_TX_POST_ACK_DELAY_MS,
						   file_tx_pump_cb, server, NULL);
		 break;
	 }

	 case FILE_LIVE_OP_REQ: {
		 char path[256];
		 const char *base;
		 uint32_t bytes = 0;
		 uint32_t crc = 0;
		 size_t name_len;
		 int rec_state = 0;

		 if (server->rx_active || server->tx_active ||
		     !server->stream_active) {
			 ecode = BT_ATT_ERROR_UNLIKELY;
			 break;
		 }
		 if (!server->custom_notify_enabled) {
			 ecode = BT_ATT_ERROR_UNLIKELY;
			 break;
		 }
		 if (!file_live_query_record(path, sizeof(path), &bytes, &crc,
					     &rec_state) ||
		     rec_state != FILE_LIVE_REC_ACTIVE) {
			 printf("FILE LIVE REQ: active recording not ready\n");
			 ecode = BT_ATT_ERROR_UNLIKELY;
			 break;
		 }

		 file_live_abort(server);
		 server->live_fp = fopen(path, "rb");
		 if (!server->live_fp) {
			 perror("FILE LIVE fopen");
			 ecode = BT_ATT_ERROR_UNLIKELY;
			 break;
		 }

		 snprintf(server->live_path, sizeof(server->live_path), "%s", path);
		 base = strrchr(path, '/');
		 base = base ? base + 1 : path;
		 snprintf(server->live_name, sizeof(server->live_name), "%s", base);
		 name_len = strlen(server->live_name);
		 if (name_len == 0 || name_len > 127) {
			 file_live_abort(server);
			 ecode = BT_ATT_ERROR_UNLIKELY;
			 break;
		 }

		 server->live_sent_offset = 0;
		 server->live_next_seq = 0;
		 server->live_ack_offset = 0;
		 server->live_ack_seq = 0;
		 server->live_high_offset = 0;
		 server->live_high_seq = 0;
		 server->live_final_size = 0;
		 server->live_final_crc = 0;
		 server->live_eof_polls = 0;
		 server->live_fc_ready = false;
		 server->live_sealed = false;
		 server->live_done_sent = false;
		 server->live_active = true;
		 server->live_start_pending = true;

		 server->live_start_pkt[0] = FILE_LIVE_OP_START;
		 server->live_start_pkt[1] = (uint8_t)name_len;
		 memcpy(server->live_start_pkt + 2, server->live_name, name_len);
		 server->live_start_len = 2 + name_len;

		 printf("FILE LIVE REQ ok: path=%s produced=%u\n", path, bytes);
		 file_live_schedule_kick(server);
		 break;
	 }

	 case FILE_LIVE_OP_ACK: {
		 uint32_t ack_seq;
		 uint32_t ack_offset;
		 bool first_ack;
		 bool ack_advanced = false;

		 if (!server->live_active || len < 9)
			 break;
		 ack_seq = get_le32_local(value + 1);
		 ack_offset = get_le32_local(value + 5);
		 if (ack_seq > server->live_high_seq ||
		     ack_offset > server->live_high_offset) {
			 printf("FILE LIVE ACK beyond high water: ack=%u/%u high=%u/%u\n",
				ack_seq, ack_offset, server->live_high_seq,
				server->live_high_offset);
			 break;
		 }

		 /*
		  * ACK is cumulative. It is normally behind the send cursor while
		  * packets are in flight, so never rewind merely because ACK < sent.
		  * The old behavior rewound on every normal window ACK; a later ACK
		  * could then be rejected as "ahead", leaving the tail timer stopped.
		  * Retransmission is instead driven by the window/EOF timeout above.
		  */
		 if (ack_seq < server->live_ack_seq ||
		     ack_offset < server->live_ack_offset)
			 break;
		 if ((ack_seq == server->live_ack_seq) !=
		     (ack_offset == server->live_ack_offset))
			 break;

		 first_ack = !server->live_fc_ready;
		 if (ack_seq > server->live_ack_seq) {
			 server->live_ack_seq = ack_seq;
			 server->live_ack_offset = ack_offset;
			 server->live_eof_polls = 0;
			 ack_advanced = true;
		 }

		 /*
		  * A cumulative ACK can pass a retransmission cursor because packets
		  * from the previous high-water flight were still queued. Fast-forward
		  * to what the phone has proven instead of rejecting that valid ACK.
		  */
		 if (ack_seq > server->live_next_seq) {
			 clearerr(server->live_fp);
			 if (fseek(server->live_fp, (long)ack_offset, SEEK_SET) != 0)
				 break;
			 server->live_next_seq = ack_seq;
			 server->live_sent_offset = ack_offset;
		 }
		 server->live_fc_ready = true;
		 if (server->live_done_sent && server->live_sealed &&
		     ack_offset == server->live_final_size) {
			 file_live_finish(server);
			 break;
		 }
		 file_live_schedule_after(
			 server,
			 first_ack || ack_advanced ?
			 FILE_TX_POST_ACK_DELAY_MS : FILE_LIVE_RETRY_MS);
		 break;
	 }

	 case FILE_LIVE_OP_ABORT:
		 printf("FILE LIVE ABORT: sent=%u acked=%u path=%s\n",
			server->live_sent_offset, server->live_ack_offset,
			server->live_path);
		 file_live_abort(server);
		 break;

	 case FILE_RX_OP_START: {
		 uint8_t name_len;
		 const uint8_t *size_ptr;
		 char name_buf[128] = { 0 };
		 size_t i;

		 if (server->tx_active || server->live_active ||
		     server->stream_active) {
			 ecode = BT_ATT_ERROR_UNLIKELY;
			 break;
		 }
 
		 if (len < 10) {
			 ecode = BT_ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LEN;
			 break;
		 }
 
		 name_len = value[1];
		 if ((size_t)name_len > sizeof(name_buf) - 1 || len < (size_t)name_len + 10) {
			 ecode = BT_ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LEN;
			 break;
		 }
 
		 memcpy(name_buf, value + 2, name_len);
		 for (i = 0; i < name_len; i++) {
			 if (name_buf[i] == '/' || name_buf[i] == '\\')
				 name_buf[i] = '_';
		 }
 
		 size_ptr = value + 2 + name_len;
		 file_rx_abort(server);
 
		 mkdir(file_rx_dir(), 0755);
		 if (name_len > 0)
			 snprintf(server->rx_path, sizeof(server->rx_path), "%s/%s",
				  file_rx_dir(), name_buf);
		 else
			 snprintf(server->rx_path, sizeof(server->rx_path),
				  "%s/ble_rx_%ld.bin", file_rx_dir(), time(NULL));
 
		 server->rx_fp = fopen(server->rx_path, "wb");
		 if (!server->rx_fp) {
			 perror("fopen file rx");
			 ecode = BT_ATT_ERROR_UNLIKELY;
			 break;
		 }

		 free(server->rx_io_buf);
		 server->rx_io_buf = malloc(FILE_RX_IO_BUF_SIZE);
		 if (server->rx_io_buf)
			 setvbuf(server->rx_fp, (char *)server->rx_io_buf, _IOFBF, FILE_RX_IO_BUF_SIZE);
 
		 server->rx_expected_size = get_le32_local(size_ptr);
		 server->rx_expected_crc = get_le32_local(size_ptr + 4);
		 server->rx_received_size = 0;
		 server->rx_running_crc = 0;
		 server->rx_next_seq = 0;
		 server->rx_last_status_seq = 0;
		 server->rx_last_status_bytes = 0;
		 server->rx_last_error = 0;
		 server->rx_state = FILE_RX_STATE_ACTIVE;
		 server->rx_active = true;
		 file_rx_notify_status(server, true);
 
		 printf("FILE RX START: path=%s expected=%u crc=0x%08x att_mtu=%u\n",
				server->rx_path, server->rx_expected_size, server->rx_expected_crc,
				bt_att_get_mtu(server->att));
		 break;
	 }
 
	 case FILE_RX_OP_DATA: {
		 uint32_t seq;
		 uint16_t chunk_decl_len;
		 uint16_t frame_crc_rx;
		 uint16_t frame_crc_calc;
		 size_t chunk_len;
		 size_t n;
		 const uint8_t *payload;
		 bool frame_crc_ok = false;
 
		 if (len < 7) {
			 ecode = BT_ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LEN;
			 break;
		 }
 
		 if (!server->rx_active || !server->rx_fp) {
			 ecode = BT_ATT_ERROR_UNLIKELY;
			 break;
		 }
 
		 seq = get_le32_local(value + 1);
		 chunk_decl_len = get_le16_local(value + 5);
 
		 if (len >= 7 + FILE_RX_DATA_CRC_LEN) {
			 size_t payload_len = len - 7 - FILE_RX_DATA_CRC_LEN;
 
			 if (chunk_decl_len == payload_len) {
				 frame_crc_rx = get_le16_local(value + 7 + payload_len);
				 frame_crc_calc = crc16_local(value + 1, 6 + payload_len);
				 if (frame_crc_rx == frame_crc_calc) {
					 chunk_len = payload_len;
					 payload = value + 7;
					 frame_crc_ok = true;
				 }
			 }
		 }
 
		 if (!frame_crc_ok) {
			 if (len >= 7 + FILE_RX_DATA_CRC_LEN) {
				 size_t payload_len = len - 7 - FILE_RX_DATA_CRC_LEN;
 
				 if (chunk_decl_len == payload_len) {
					 printf("FILE RX FRAME CRC mismatch: seq=%u\n", seq);
					 server->rx_last_error = FILE_RX_ERR_FRAME_CRC;
					 ecode = BT_ATT_ERROR_UNLIKELY;
					 break;
				 }
			 }
 
			 chunk_len = len - 7;
			 payload = value + 7;
			 if (chunk_decl_len != chunk_len) {
				 printf("FILE RX LEN mismatch: seq=%u decl=%u actual=%zu len=%zu\n",
						seq, chunk_decl_len, chunk_len, len);
				 server->rx_last_error = FILE_RX_ERR_FRAME_LEN;
				 ecode = BT_ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LEN;
				 break;
			 }
		 }
 
		 if (seq != server->rx_next_seq) {
			 printf("FILE RX SEQ mismatch: got=%u expected=%u\n",
					seq, server->rx_next_seq);
			 server->rx_last_error = FILE_RX_ERR_SEQ;
			 ecode = BT_ATT_ERROR_UNLIKELY;
			 break;
		 }
 
		 if (server->rx_received_size + chunk_len > server->rx_expected_size) {
			 ecode = BT_ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LEN;
			 break;
		 }
 
		 n = fwrite(payload, 1, chunk_len, server->rx_fp);
		 if (n != chunk_len) {
			 perror("fwrite file rx");
			 ecode = BT_ATT_ERROR_UNLIKELY;
			 break;
		 }
 
		 server->rx_received_size += chunk_len;
		 server->rx_running_crc = crc32_update_local(server->rx_running_crc, payload, chunk_len);
		 server->rx_next_seq++;
 
		 if (server->rx_received_size == chunk_len)
			 printf("FILE RX DATA: first chunk %zu bytes\n", chunk_len);
		 else if ((server->rx_received_size / (64 * 1024)) !=
			  ((server->rx_received_size - chunk_len) / (64 * 1024)))
			 printf("FILE RX DATA: %u / %u bytes\n",
					server->rx_received_size, server->rx_expected_size);
 
		 /* Do not fflush() every BLE packet. Per-packet flush was the main
		  * throughput killer. stdio buffers the data; END/disconnect flushes it.
		  * Force a final ACTIVE status when the last DATA packet arrives; otherwise
		  * small files or the last partial 32-packet window can make the sender wait
		  * forever before sending END.
		  */
		 file_rx_notify_status(server,
				server->rx_received_size == server->rx_expected_size);
		 break;
	 }
 
	 case FILE_RX_OP_END:
		 if (!server->rx_active || !server->rx_fp) {
			 ecode = BT_ATT_ERROR_UNLIKELY;
			 break;
		 }
 
		 fflush(server->rx_fp);
		 fclose(server->rx_fp);
		 server->rx_fp = NULL;
		 free(server->rx_io_buf);
		 server->rx_io_buf = NULL;
		 server->rx_active = false;
 
		 if (server->rx_received_size != server->rx_expected_size) {
			 printf("FILE RX END mismatch: recv=%u expected=%u path=%s\n",
					server->rx_received_size, server->rx_expected_size,
					server->rx_path);
			 ecode = BT_ATT_ERROR_UNLIKELY;
			 file_rx_abort(server);
			 break;
		 }
 
		 if (server->rx_running_crc != server->rx_expected_crc) {
			 printf("FILE RX CRC mismatch: got=0x%08x expected=0x%08x path=%s\n",
					server->rx_running_crc, server->rx_expected_crc, server->rx_path);
			 ecode = BT_ATT_ERROR_UNLIKELY;
			 file_rx_abort(server);
			 break;
		 }
 
		 server->rx_state = FILE_RX_STATE_DONE;
		 server->rx_last_error = 0;
		 file_rx_notify_status(server, true);
 
		 printf("FILE RX DONE: path=%s size=%u crc=0x%08x\n",
				server->rx_path, server->rx_received_size, server->rx_running_crc);
		 break;
 
	 case FILE_RX_OP_ABORT:
		 printf("FILE RX ABORT: path=%s recv=%u expected=%u\n",
				server->rx_path, server->rx_received_size, server->rx_expected_size);
		 file_rx_abort(server);
		 break;
 
	 default:
		 ecode = BT_ATT_ERROR_VALUE_NOT_ALLOWED;
		 break;
	 }
 
 done:
	 if (ecode) {
		 if (value[0] == FILE_RX_OP_DATA || value[0] == FILE_RX_OP_START)
			 printf("FILE RX write error: op=0x%02x ecode=0x%02x len=%zu\n",
					value[0], ecode, len);
		 server->rx_state = FILE_RX_STATE_ERROR;
		 if (server->rx_last_error == 0)
			 server->rx_last_error = ecode;
		 file_rx_notify_status(server, true);
	 }
 
	 gatt_db_attribute_write_result(attrib, id, ecode);
 }
 
 static void custom_data_read_cb(struct gatt_db_attribute *attrib, unsigned int id,
								 uint16_t offset, uint8_t opcode, struct bt_att *att,
								 void *user_data)
 {
	 struct server *server = user_data;
	 uint8_t error = 0;
	 size_t len = 0;
	 const uint8_t *value = NULL;
 
	 // 1. 检查偏移量是否合法
	 if (offset > server->external_data_len) {
		 error = BT_ATT_ERROR_INVALID_OFFSET;
		 goto done;
	 }
 
	 // 2. 计算剩余长度并设置指针
	 len = server->external_data_len - offset;
	 // 安全保护：避免 NULL 指针算术（UB）
	 value = (len > 0 && server->external_data_ptr) 
			 ? server->external_data_ptr + offset 
			 : NULL;
 
	 printf("custom_data_read_cb!\n"); 
	 
 done:
	 // 3. 返回结果
	 gatt_db_attribute_read_result(attrib, id, error, value, len);
 }
 
 
 
 
 
 /*--------------------------------------------------------------------------
  * 3. GAP 设备名称写入回调 (处理客户端写请求)
  *--------------------------------------------------------------------------*/
 /*
  * gap_device_name_write_cb: 当中心设备尝试写入/修改本设备的“设备名称”时触发
  * 
  * @attrib:   当前被写入的 GATT 属性句柄
  * @id:       本次写入操作的唯一事务 ID（Transaction ID）
  * @offset:   写入数据的偏移量（支持长属性的分片写入）
  * @value:    指向客户端发送过来的新名称数据的指针
  * @len:      本次写入的数据长度
  * @opcode:   触发此次回调的 ATT 操作码（如 Write Request, Prepare Write 等）
  * @att:      底层的 ATT 传输对象
  * @user_data: 指向核心结构体 struct server 的指针，用于更新内部状态
  */
 static void gap_device_name_write_cb(struct gatt_db_attribute *attrib,
									  unsigned int id, uint16_t offset,
									  const uint8_t *value, size_t len,
									  uint8_t opcode, struct bt_att *att,
									  void *user_data)
 {
	 struct server *server = user_data;
	 uint8_t error = 0;
 
	 PRLOG("GAP Device Name Write: offset=%u, len=%zu\n", offset, len);
 
	 /* 【逻辑1】处理清空设备名称 */
	 // 修复：使用显式判断替代 !(offset+len)，避免 uint16_t 溢出误判
	 if (offset == 0 && len == 0) {
		 free(server->device_name);
		 server->device_name = NULL;
		 server->name_len = 0;
		 goto done;
	 }
 
	 /* 【逻辑2】边界检查与动态内存调整 */
	 if (offset > server->name_len) {
		 error = BT_ATT_ERROR_INVALID_OFFSET;
		 goto done;
	 }
 
	 size_t new_total = (size_t)offset + len;
	 if (new_total != server->name_len) {
		 uint8_t *name = realloc(server->device_name, new_total);
		 if (!name) {
			 error = BT_ATT_ERROR_INSUFFICIENT_RESOURCES;
			 goto done;
		 }
		 server->device_name = name;
		 server->name_len = new_total;
	 }
 
	 /* 【逻辑3】数据拷贝 */
	 if (value && len > 0) {
		 memcpy(server->device_name + offset, value, len);
	 }
 
 done:
	 gatt_db_attribute_write_result(attrib, id, error);
 }
 
 
 
 
 
 /*--------------------------------------------------------------------------
  * 3. GAP 设备名称扩展属性读取回调 (处理客户端读请求)
  *--------------------------------------------------------------------------*/
 /*
  * gap_device_name_ext_prop_read_cb: 当中心设备尝试读取“设备名称”特征的扩展属性时触发
  *                                   在 GATT 协议中，如果某个特征包含扩展属性描述符，
  *                                   客户端可以通过读取该描述符来获取额外信息。
  * 
  * @attrib:   当前被读取的 GATT 属性句柄（此处为 Extended Properties Descriptor）
  * @id:       本次读取操作的唯一事务 ID（Transaction ID）
  * @offset:   读取数据的偏移量（对于仅2字节的扩展属性，通常始终为0）
  * @opcode:   触发此次回调的 ATT 操作码
  * @att:      底层的 ATT 传输对象
  * @user_data: 用户数据指针（在此处未使用）
  */
 static void gap_device_name_ext_prop_read_cb(struct gatt_db_attribute *attrib,
											  unsigned int id, uint16_t offset,
											  uint8_t opcode, struct bt_att *att,
											  void *user_data)
 {
	 // Extended Properties 固定 2 字节 (Bluetooth Core Spec Vol 3, Part G, 3.3.3.1)
	 const uint8_t ext_prop[2] = {
		 BT_GATT_CHRC_EXT_PROP_RELIABLE_WRITE, // Byte 0: 支持可靠写入
		 0x00                                   // Byte 1: 保留
	 };
 
	 PRLOG("Device Name Extended Properties Read: offset=%u\n", offset);
 
	 uint8_t error = 0;
	 size_t len = sizeof(ext_prop);
	 const uint8_t *value = ext_prop;
 
	 // 偏移量边界检查（兼容 Read Blob Request）
	 if (offset > len) {
		 error = BT_ATT_ERROR_INVALID_OFFSET;
		 value = NULL;
		 len = 0;
	 } else if (offset > 0) {
		 value = &ext_prop[offset];
		 len -= offset;
	 }
 
	 gatt_db_attribute_read_result(attrib, id, error, value, len);
 }
 
 
 
 
 
 /*--------------------------------------------------------------------------
  * 5. Service Changed 特征值读取回调
  *--------------------------------------------------------------------------*/
 static void gatt_service_changed_cb(struct gatt_db_attribute *attrib,
									 unsigned int id, uint16_t offset,
									 uint8_t opcode, struct bt_att *att,
									 void *user_data)
 {
	 PRLOG("Service Changed Read called (not permitted)\n");
 
	 // 修复：返回标准 ATT 错误码，明确告知客户端此特征不可读
	 // Service Changed 仅支持 Indication，不支持 Read
	 gatt_db_attribute_read_result(attrib, id,
								   BT_ATT_ERROR_READ_NOT_PERMITTED,
								   NULL, 0);
 }
 
 /*--------------------------------------------------------------------------
  * 6. Service Changed CCC 描述符读取回调
  *--------------------------------------------------------------------------*/
 static void gatt_svc_chngd_ccc_read_cb(struct gatt_db_attribute *attrib,
										unsigned int id, uint16_t offset,
										uint8_t opcode, struct bt_att *att,
										void *user_data)
 {
	 struct server *server = user_data;
 
	 // CCCD 固定 2 字节
	 const uint8_t cccd[2] = {
		 server->svc_chngd_enabled ? 0x02 : 0x00,
		 0x00
	 };
 
	 PRLOG("Service Changed CCC Read: offset=%u\n", offset);
 
	 uint8_t error = 0;
	 size_t len = sizeof(cccd);
	 const uint8_t *value = cccd;
 
	 // 修复：增加偏移量边界检查，兼容 Read Blob Request
	 if (offset > len) {
		 error = BT_ATT_ERROR_INVALID_OFFSET;
		 value = NULL;
		 len = 0;
	 } else if (offset > 0) {
		 value = &cccd[offset];
		 len -= offset;
	 }
 
	 gatt_db_attribute_read_result(attrib, id, error, value, len);
 }
 
 /*--------------------------------------------------------------------------
  * 7. Service Changed CCC 描述符写入回调
  *--------------------------------------------------------------------------*/
 static void gatt_svc_chngd_ccc_write_cb(struct gatt_db_attribute *attrib,
										 unsigned int id, uint16_t offset,
										 const uint8_t *value, size_t len,
										 uint8_t opcode, struct bt_att *att,
										 void *user_data)
 {
	 struct server *server = user_data;
	 uint8_t ecode = 0;
 
	 PRLOG("Service Changed CCC Write: len=%zu\n", len);
 
	 // 参数合法性校验
	 if (!value || len != 2) {
		 ecode = BT_ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LEN;
		 goto done;
	 }
 
	 if (offset != 0) {
		 ecode = BT_ATT_ERROR_INVALID_OFFSET;
		 goto done;
	 }
 
	 // 修复：按位检测 Indication 标志，而非精确匹配
	 // 允许客户端同时设置 Notify(0x01) + Indicate(0x02) = 0x03
	 // 仅关心 bit1 (Indication)，忽略 bit0 (Notification)
	 uint8_t flags = value[0];
	 if (flags & ~0x03) {
		 // 保留了除 Notify/Indicate 以外的位，属于非法值
		 ecode = BT_ATT_ERROR_VALUE_NOT_ALLOWED;
		 goto done;
	 }
 
	 server->svc_chngd_enabled = (flags & 0x02) != 0;
 
	 PRLOG("Service Changed Enabled: %s (flags=0x%02x)\n",
		   server->svc_chngd_enabled ? "true" : "false", flags);
 
 done:
	 gatt_db_attribute_write_result(attrib, id, ecode);
 }
 
 
 // 专门用于读取 CCCD 状态的回调
 static void custom_data_ccc_read_cb(struct gatt_db_attribute *attrib, unsigned int id,
									 uint16_t offset, uint8_t opcode,
									 struct bt_att *att, void *user_data)
 {
	 struct server *server = user_data;
	 uint8_t ecode = 0;
	 uint8_t value[2] = {0x00, 0x00}; // 默认关闭状态
 
	 (void)opcode;
	 (void)att;
	 printf("custom_data_ccc_read_cb\n");
 
	 // start_notify() enables Notify (0x0001); indicate is 0x0002.
	 if (server->custom_notify_enabled) {
		 value[0] = 0x01;
		 if (server->custom_indicate_enabled)
			 value[0] |= 0x02;
	 }
 
	 // 将值返回给协议栈
	 gatt_db_attribute_read_result(attrib, id, ecode, value, sizeof(value));
 }
 
 
 static void custom_data_ccc_write_cb(struct gatt_db_attribute *attrib, unsigned int id,
									 uint16_t offset, const uint8_t *value, size_t len,
									 uint8_t opcode, struct bt_att *att, void *user_data)
 {
	 struct server *server = user_data;
	 uint8_t ecode = 0;
 
	 printf("custom_data_ccc_write_cb！！！\n");
	 // 1. 参数校验 (必须是2字节，且偏移为0)
	 if (!value || len != 2 || offset != 0) {
		 ecode = BT_ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LEN;
		 goto done;
	 }
 
	 // 解析 CCCD 的值
	 // value[0] == 0x00: 关闭所有通知/指示
	 // value[0] == 0x01: 开启 Notify
	 // value[0] == 0x02: 开启 Indicate (这是你现在的模式！)
 
	 if (value[0] & 0x03) { // Notify or Indicate enabled
		 server->custom_notify_enabled = true;
		 server->custom_indicate_enabled = (value[0] & 0x02) != 0;
		 printf("Custom Notify/Indicate: Enabled (Value: 0x%02X, %s)\n",
			value[0],
			server->custom_indicate_enabled ? "indicate" : "notify");
 
		 /* Disable legacy periodic test notify to avoid polluting bridge tests. */
		 if (server->custom_timeout_id) {
			 timeout_remove(server->custom_timeout_id);
			 server->custom_timeout_id = 0;
		 }
		 bridge_schedule_flush(server);
 
	 } else {
		 // 关闭
		 server->custom_notify_enabled = false;
		 server->custom_indicate_enabled = false;
		 printf("Custom Notify/Indicate: Disabled\n");
		 if (server->tx_active) {
			 printf("FILE TX warn: Notify disabled during transfer sent=%u/%u\n",
				server->tx_sent_size, server->tx_file_size);
		 }
		 if (server->live_active)
			 file_live_abort(server);
		 if (server->stream_active)
			 stream_abort(server);
 
		 // 停止定时器
		 if (server->custom_timeout_id) {
			 timeout_remove(server->custom_timeout_id);
			 server->custom_timeout_id = 0;
		 }
		 if (server->bridge_flush_id) {
			 timeout_remove(server->bridge_flush_id);
			 server->bridge_flush_id = 0;
		 }
	 }
 
 done:
	 gatt_db_attribute_write_result(attrib, id, ecode);
 }
 
 
 
 /*--------------------------------------------------------------------------
  * 13. 属性缓存写入确认回调 (通用错误处理)
  *--------------------------------------------------------------------------*/
 /*
  * confirm_write: GATT 数据库属性缓存写入后的确认回调函数
  *                通常在调用 gatt_db_attribute_set_cached_value() 等异步接口时使用，
  *                用于捕获底层数据库更新过程中的致命异常。
  * 
  * @attr:     被尝试写入或更新的 GATT 属性句柄指针
  * @err:      写入操作返回的错误码（0 表示成功，非 0 表示失败）
  * @user_data: 用户数据指针（在此处未使用）
  */
 static void confirm_write(struct gatt_db_attribute *attr, int err,
							 void *user_data)
 {
	 // 如果错误码为 0，说明属性缓存写入成功，直接返回即可
	 if (!err)
		 return;
 
	 // 如果发生错误，向标准错误流(stderr)打印详细的出错信息（包含属性地址和错误码）
	 fprintf(stderr, "Error caching attribute %p - err: %d\n", attr, err);
	 
	 // 退出整个程序。由于这是示例程序，数据库缓存写入失败属于不可恢复的致命错误，
	 // 因此直接调用 exit(1) 终止运行，防止后续逻辑在错误的状态下继续执行。
#ifdef MY_SERVER_EMBEDDED
	 mainloop_quit();
#else
	 exit(1);
#endif
 }
 
 /*--------------------------------------------------------------------------
  * 14. 构建 GAP 服务 (通用访问配置文件)
  *--------------------------------------------------------------------------*/
 /*
  * populate_gap_service: 向 GATT 数据库中填充并激活标准的 GAP 服务
  *                       GAP 服务包含了设备的名称、外观等基础信息，是所有 BLE 设备必须具备的服务。
  * 
  * @server: 指向核心结构体 struct server 的指针，包含 GATT 数据库句柄
  */
 static void populate_gap_service(struct server *server)
 {
	 bt_uuid_t uuid;              // 用于存放临时的 UUID
	 struct gatt_db_attribute *service, *tmp; // 服务和特征的属性句柄
	 uint16_t appearance;         // 用于存放设备外观代码的临时变量
 
	 // 【步骤1】添加 GAP 主服务 (UUID: 0x1800)
	 bt_uuid16_create(&uuid, UUID_GAP);
	 service = gatt_db_add_service(server->db, &uuid, true, 4);
 
	 /* Device Name: read-only, no extended-properties descriptor (WinRT friendly) */
	 bt_uuid16_create(&uuid, GATT_CHARAC_DEVICE_NAME);
	 gatt_db_service_add_characteristic(service, &uuid,
					 BT_ATT_PERM_READ,
					 BT_GATT_CHRC_PROP_READ,
					 gap_device_name_read_cb,
					 NULL,
					 server);
 
	 /*
	  * 【步骤4】添加“外观”特征 (Appearance Characteristic, UUID: 0x2A01)
	  * 注意：与设备名称不同，外观值是一个固定常量，直接从数据库读取，无需回调。
	  */
	 bt_uuid16_create(&uuid, GATT_CHARAC_APPEARANCE);
	 tmp = gatt_db_service_add_characteristic(service, &uuid,
							 BT_ATT_PERM_READ,   // 权限：只读
							 BT_GATT_CHRC_PROP_READ, // 属性：支持读
							 NULL, NULL, server);    // 不需要读写回调
 
	 /*
	  * 【步骤5】将固定的外观值写入数据库缓存
	  * 这里设置外观值为 128 (Generic Phone / 通用电话)，表示这是一个模拟手机类的设备。
	  */
	 put_le16(128, &appearance); // 将整数转换为小端序字节流
	 gatt_db_attribute_write(tmp, 0, (void *) &appearance,
							 sizeof(appearance),
							 BT_ATT_OP_WRITE_REQ, // 内部模拟一次写请求以存入数据库
							 NULL, confirm_write, // 使用 confirm_write 进行错误确认
							 NULL);
 
	 // 【最后一步】将 GAP 服务标记为活跃状态，使其对外可见
	 gatt_db_service_set_active(service, true);
 }
 
 
 /*--------------------------------------------------------------------------
  * 15. 构建 GATT 服务 (通用属性配置文件)
  *--------------------------------------------------------------------------*/
 /*
  * populate_gatt_service: 向 GATT 数据库中填充并激活标准的 GATT 服务
  *                        GATT 服务主要用于通知客户端服务器的拓扑结构发生了变化。
  * 
  * @server: 指向核心结构体 struct server 的指针
  */
 static void populate_gatt_service(struct server *server)
 {
	 bt_uuid_t uuid;
	 struct gatt_db_attribute *service, *svc_chngd;
 
	 // 【步骤1】添加 GATT 主服务 (UUID: 0x1801)
	 bt_uuid16_create(&uuid, UUID_GATT);
	 // 参数说明：数据库句柄、UUID、是否为主服务(true)、预留的属性槽位数(4个)
	 service = gatt_db_add_service(server->db, &uuid, true, 4);
 
	 /*
	  * 【步骤2】添加“服务变更”特征 (Service Changed, UUID: 0x2A05)
	  * 当服务器动态增删其他服务时，会通过此特征的 Indication 告知客户端。
	  */
	 bt_uuid16_create(&uuid, GATT_CHARAC_SERVICE_CHANGED);
	 svc_chngd = gatt_db_service_add_characteristic(service, &uuid,
			 BT_ATT_PERM_READ,                    // 权限：只读（实际值不能由客户端直接读取）
			 BT_GATT_CHRC_PROP_READ |             // 属性：支持读（用于拒绝非法读取）
			 BT_GATT_CHRC_PROP_INDICATE,          // 属性：支持指示（Indicate）
			 gatt_service_changed_cb,             // 读取回调（返回空以合规拒绝）
			 NULL,                                // 无写入回调
			 server);
	 
	 // 保存该特征的句柄到全局结构中，后续发送 Indication 时需要用到它
	 server->gatt_svc_chngd_handle = gatt_db_attribute_get_handle(svc_chngd);
 
	 // 【步骤3】添加“客户端配置描述符” (CCCD, UUID: 0x2902)
	 bt_uuid16_create(&uuid, GATT_CLIENT_CHARAC_CFG_UUID);
	 gatt_db_service_add_descriptor(service, &uuid,
				 BT_ATT_PERM_READ | BT_ATT_PERM_WRITE, // 权限：允许客户端读写以控制订阅
				 gatt_svc_chngd_ccc_read_cb,           // 读取当前订阅状态
				 gatt_svc_chngd_ccc_write_cb,          // 处理客户端的订阅/取消订阅请求
				 server);
 
	 // 【最后一步】将 GATT 服务标记为活跃状态
	 gatt_db_service_set_active(service, true);
 }
 
 static bool hr_msrmt_cb(void *user_data)
 {
	 struct server *server = user_data;
	 bool expended_present = !(server->hr_ee_count % 10);
	 uint16_t len = 2;
	 uint8_t pdu[4];
	 uint32_t cur_ee;
	 uint32_t val;
 
	 if (util_getrandom(&val, sizeof(val), 0) < 0)
		 return false;
 
	 pdu[0] = 0x06;
	 pdu[1] = 90 + (val % 40);
 
	 if (expended_present) {
		 pdu[0] |= 0x08;
		 put_le16(server->hr_energy_expended, pdu + 2);
		 len += 2;
	 }
 
	 bt_gatt_server_send_notification(server->gatt,
					  server->hr_msrmt_handle,
					  pdu, len, false);
 
	 cur_ee = server->hr_energy_expended;
	 server->hr_energy_expended = MIN(UINT16_MAX, cur_ee + 10);
	 server->hr_ee_count++;
 
	 return true;
 }
 
 static void update_hr_msrmt_simulation(struct server *server)
 {
	 if (!server->hr_msrmt_enabled || !server->hr_visible) {
		 if (server->hr_timeout_id)
			 timeout_remove(server->hr_timeout_id);
		 server->hr_timeout_id = 0;
		 return;
	 }
 
	 server->hr_timeout_id = timeout_add(1000, hr_msrmt_cb, server, NULL);
 }
 
 static void hr_msrmt_ccc_read_cb(struct gatt_db_attribute *attrib,
				  unsigned int id, uint16_t offset,
				  uint8_t opcode, struct bt_att *att,
				  void *user_data)
 {
	 struct server *server = user_data;
	 uint8_t error = 0;
	 uint8_t value[2] = { server->hr_msrmt_enabled ? 0x01 : 0x00, 0x00 };
 
	 (void)opcode;
	 (void)att;
 
	 if (offset > sizeof(value)) {
		 error = BT_ATT_ERROR_INVALID_OFFSET;
		 gatt_db_attribute_read_result(attrib, id, error, NULL, 0);
		 return;
	 }
 
	 gatt_db_attribute_read_result(attrib, id, error, value + offset,
					   sizeof(value) - offset);
 }
 
 static void hr_msrmt_ccc_write_cb(struct gatt_db_attribute *attrib,
				   unsigned int id, uint16_t offset,
				   const uint8_t *value, size_t len,
				   uint8_t opcode, struct bt_att *att,
				   void *user_data)
 {
	 struct server *server = user_data;
	 uint8_t ecode = 0;
 
	 (void)attrib;
	 (void)opcode;
	 (void)att;
 
	 if (!value || len != 2) {
		 ecode = BT_ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LEN;
		 goto done;
	 }
 
	 if (offset) {
		 ecode = BT_ATT_ERROR_INVALID_OFFSET;
		 goto done;
	 }
 
	 if (value[0] == 0x00)
		 server->hr_msrmt_enabled = false;
	 else if (value[0] == 0x01) {
		 if (server->hr_msrmt_enabled) {
			 printf("HR Measurement Already Enabled\n");
			 goto done;
		 }
		 server->hr_msrmt_enabled = true;
	 } else {
		 ecode = BT_ATT_ERROR_VALUE_NOT_ALLOWED;
		 goto done;
	 }
 
	 printf("Heart Rate notify %s\n",
			server->hr_msrmt_enabled ? "enabled" : "disabled");
	 update_hr_msrmt_simulation(server);
 
 done:
	 gatt_db_attribute_write_result(attrib, id, ecode);
 }
 
 static void hr_body_read_cb(struct gatt_db_attribute *attrib,
				 unsigned int id, uint16_t offset,
				 uint8_t opcode, struct bt_att *att,
				 void *user_data)
 {
	 const uint8_t body_sensor_location = 0x01; /* Chest */
 
	 (void)opcode;
	 (void)att;
	 (void)user_data;
 
	 if (offset > 1) {
		 gatt_db_attribute_read_result(attrib, id,
						   BT_ATT_ERROR_INVALID_OFFSET,
						   NULL, 0);
		 return;
	 }
 
	 gatt_db_attribute_read_result(attrib, id, 0,
					   &body_sensor_location + offset,
					   1 - offset);
 }
 
 static void hr_control_write_cb(struct gatt_db_attribute *attrib,
				 unsigned int id, uint16_t offset,
				 const uint8_t *value, size_t len,
				 uint8_t opcode, struct bt_att *att,
				 void *user_data)
 {
	 struct server *server = user_data;
	 uint8_t ecode = 0;
 
	 (void)attrib;
	 (void)opcode;
	 (void)att;
 
	 if (!value || len != 1) {
		 ecode = BT_ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LEN;
		 goto done;
	 }
 
	 if (offset) {
		 ecode = BT_ATT_ERROR_INVALID_OFFSET;
		 goto done;
	 }
 
	 if (value[0] == 0x01) {
		 server->hr_energy_expended = 0;
		 PRLOG("HR: Energy Expended value reset\n");
	 }
 
 done:
	 gatt_db_attribute_write_result(attrib, id, ecode);
 }
 
 static void populate_hr_service(struct server *server)
 {
	 bt_uuid_t uuid;
	 struct gatt_db_attribute *service, *hr_msrmt;
 
	 bt_uuid16_create(&uuid, UUID_HEART_RATE);
	 /*
	  * Heart Rate service needs 8 handles:
	  * service(1) + HR Measurement char/value(2) + CCCD(1) +
	  * Body Sensor char/value(2) + Control Point char/value(2)
	  */
	 service = gatt_db_add_service(server->db, &uuid, true, 8);
	 if (!service) {
		 fprintf(stderr, "Failed to create Heart Rate service\n");
		 return;
	 }
 
	 server->hr_handle = gatt_db_attribute_get_handle(service);
 
	 /* Heart Rate Measurement (Notify). */
	 bt_uuid16_create(&uuid, UUID_HEART_RATE_MSRMT);
	 hr_msrmt = gatt_db_service_add_characteristic(service, &uuid, 0,
						 BT_GATT_CHRC_PROP_NOTIFY,
						 NULL, NULL, server);
	 if (!hr_msrmt)
		 fprintf(stderr, "Failed to add Heart Rate Measurement\n");
	 else
		 server->hr_msrmt_handle = gatt_db_attribute_get_handle(hr_msrmt);
 
	 bt_uuid16_create(&uuid, GATT_CLIENT_CHARAC_CFG_UUID);
	 if (!gatt_db_service_add_descriptor(service, &uuid,
						 BT_ATT_PERM_READ | BT_ATT_PERM_WRITE,
						 hr_msrmt_ccc_read_cb,
						 hr_msrmt_ccc_write_cb, server))
		 fprintf(stderr, "Failed to add Heart Rate CCCD\n");
 
	 /* Body Sensor Location (Read). */
	 bt_uuid16_create(&uuid, UUID_HEART_RATE_BODY);
	 if (!gatt_db_service_add_characteristic(service, &uuid,
						 BT_ATT_PERM_READ,
						 BT_GATT_CHRC_PROP_READ,
						 hr_body_read_cb, NULL, server))
		 fprintf(stderr, "Failed to add Body Sensor Location\n");
 
	 /* Heart Rate Control Point (Write). */
	 bt_uuid16_create(&uuid, UUID_HEART_RATE_CTRL);
	 if (!gatt_db_service_add_characteristic(service, &uuid,
						 BT_ATT_PERM_WRITE,
						 BT_GATT_CHRC_PROP_WRITE,
						 NULL, hr_control_write_cb, server))
		 fprintf(stderr, "Failed to add Heart Rate Control Point\n");
 
	 if (server->hr_visible) {
		 gatt_db_service_set_active(service, true);
		 printf("Heart Rate Service Created and Activated!\n");
	 }
 }
 
 
 
 
 
 static void populate_custom_service(struct server *server) {
	 bt_uuid_t uuid;
	 struct gatt_db_attribute *service, *data_char;
	 
	 // --- 【1】定义 128-bit UUID 的字节数据 (保持不变) ---
	 // 注意：这里的字节序是小端序 (Little-Endian)
	 static uint8_t custom_svc_uuid_bytes[16] = {
		 0x78, 0x56, 0x34, 0x12, 0x34, 0x12, 0x78, 0x56, 
		 0x12, 0x34, 0x56, 0x78, 0x12, 0x34, 0x56, 0x78
	 };
	 static uint8_t custom_data_uuid_bytes[16] = {
		 0x79, 0x56, 0x34, 0x12, 0x34, 0x12, 0x78, 0x56, 
		 0x12, 0x34, 0x56, 0x78, 0x12, 0x34, 0x56, 0x78
	 };
 
	 // --- 【2】定义 uint128_t 变量 ---
	 uint128_t svc_uuid_val;
	 uint128_t data_uuid_val;
 
	 // --- 【3】拷贝字节数组 ---
	 memcpy(&svc_uuid_val, custom_svc_uuid_bytes, 16);
	 memcpy(&data_uuid_val, custom_data_uuid_bytes, 16);
 
	 printf("Trying to create custom service.....\n");
	 // --- 【4】创建服务 ---
	 bt_uuid128_create(&uuid, svc_uuid_val); 
	 // service decl + char decl + char value + CCCD = 4 handles
	 service = gatt_db_add_service(server->db, &uuid, true, 4);
	 if (!service) {
		 printf("Failed to create custom service\n");
		 return;
	 }
 
	 // --- 【5】创建特征值 ---
	 bt_uuid128_create(&uuid, data_uuid_val); 
	 
	 // NOTIFY + WRITE_WITHOUT_RESP only — Android/FBP reject writes when discovery
	 // reports empty props (common with INDICATE|READ|WRITE combo on raw L2CAP GATT).
	 data_char = gatt_db_service_add_characteristic(service, &uuid,
		 BT_ATT_PERM_WRITE,
		 BT_GATT_CHRC_PROP_WRITE_WITHOUT_RESP |
		 BT_GATT_CHRC_PROP_NOTIFY,
		 NULL,
		 custom_data_write_cb,
		 server);
 
	 if (!data_char) {
		 printf("Failed to add custom data characteristic (check num_handles)\n");
		 return;
	 }
 
	 // Save characteristic Handle
	 server->custom_data_handle = gatt_db_attribute_get_handle(data_char);
	 printf("Custom xfer char handle=0x%04x (notify+write-without-response)\n",
		server->custom_data_handle);
 
	 bt_uuid16_create(&uuid, GATT_CLIENT_CHARAC_CFG_UUID);
	 if (!gatt_db_service_add_descriptor(service, &uuid,
					 BT_ATT_PERM_READ | BT_ATT_PERM_WRITE,
					 custom_data_ccc_read_cb,
					 custom_data_ccc_write_cb, server))
	 printf("Failed to add custom data CCCD\n");
 
	 gatt_db_service_set_active(service, true);
	 
	 printf("Custom Service Created and Activated!\n"); 
 }
 
 
 /*--------------------------------------------------------------------------
  * 17. 数据库总装函数 (注册所有服务)
  *--------------------------------------------------------------------------*/
 /*
  * populate_db: 统一调用各个服务的构建函数，将 GAP, GATT 和 HRS 填充进 GATT 数据库
  * 
  * @server: 指向核心结构体 struct server 的指针
  */
 static void populate_db(struct server *server)
 {
	 // 依次构建并激活标准的蓝牙基础服务和心率业务服务
	 populate_gap_service(server);   // 通用访问配置文件 (设备名、外观等)
	 populate_gatt_service(server);  // 通用属性配置文件 (服务变更通知)
	 populate_hr_service(server);    // Heart Rate Service (0x180D)
	 populate_custom_service(server); // <--- 添加这一行，huang
 }
 
 
 
 
 
 /*--------------------------------------------------------------------------
  * 18. 创建并初始化 GATT 服务器实例
  *--------------------------------------------------------------------------*/
 /*
  * server_create: 创建一个全新的 GATT 服务器对象，并完成底层传输层与业务数据的初始化
  *                这是整个程序的核心启动函数，采用了典型的 C 语言“失败即跳转清理”模式。
  * 
  * @fd:         已连接的蓝牙套接字文件描述符 (用于 ATT 通信)
  * @mtu:        最大传输单元 (Maximum Transmission Unit)，决定单次数据包大小
  * @hr_visible: 控制心率服务是否在广播中对外可见
  * @return:     成功返回初始化好的 struct server 指针，失败返回 NULL
  */
 static struct server *server_create(int fd, uint16_t mtu, bool hr_visible)
 {
	 struct server *server;
	 size_t name_len = strlen(test_device_name); // 计算默认设备名称的长度
 
	 // 【步骤1】分配核心结构体内存
	 server = new0(struct server, 1); // 分配并清零内存
	 if (!server) {
		 fprintf(stderr, "Failed to allocate memory for server\n");
		 return NULL;
	 }
 
	 // 【步骤2】初始化底层 ATT 传输层
	 // 基于传入的文件描述符(fd)创建 ATT 通道，false 表示该通道不处于服务端模式(视具体API而定)
	 server->att = bt_att_new(fd, false);
	 if (!server->att) {
		 fprintf(stderr, "Failed to initialze ATT transport layer\n");
		 goto fail; // 发生错误，跳转到统一的资源释放逻辑
	 }
 
	 // 设置当 ATT 对象的引用计数降为 0 时，自动关闭底层的 socket 连接
	 if (!bt_att_set_close_on_unref(server->att, true)) {
		 fprintf(stderr, "Failed to set up ATT transport layer\n");
		 goto fail;
	 }
 
	 // 注册断开连接回调函数。当客户端断开蓝牙连接时，会触发 att_disconnect_cb
	 if (!bt_att_register_disconnect(server->att, att_disconnect_cb, server, NULL)) {
		 fprintf(stderr, "Failed to set ATT disconnect handler\n");
		 goto fail;
	 }
 
	 // 【步骤3】分配并初始化动态设备名称缓冲区
	 // ATT Device Name must not include the trailing NUL byte
	 server->name_len = name_len;
	 server->device_name = malloc(name_len + 1);
	 if (!server->device_name) {
		 fprintf(stderr, "Failed to allocate memory for device name\n");
		 goto fail;
	 }
	 // 拷贝默认的设备名称字符串，并确保以 '\0' 结尾
	 memcpy(server->device_name, test_device_name, name_len);
	 server->device_name[name_len] = '\0';
 
	 // 【步骤4】保存文件描述符并创建 GATT 数据库容器
	 server->fd = fd;
	 server->db = gatt_db_new(); // 在内存中创建一个空的 GATT 数据库
	 if (!server->db) {
		 fprintf(stderr, "Failed to create GATT database\n");
		 goto fail;
	 }
 
	 // 【步骤5】创建上层的 GATT 服务器对象
	 // 将前面创建的数据库(db)、传输层(att)以及 MTU 绑定在一起
	 server->gatt = bt_gatt_server_new(server->db, server->att, mtu, 0);
	 if (!server->gatt) {
		 fprintf(stderr, "Failed to create GATT server\n");
		 goto fail;
	 }
 
	 server->hr_visible = hr_visible;
 
	 // 如果开启了全局 verbose 模式，则为 ATT 层和 GATT 层挂载详细的调试打印回调
	 if (verbose) {
		 bt_att_set_debug(server->att, BT_ATT_DEBUG_VERBOSE,
						 att_debug_cb, "att: ", NULL);
		 bt_gatt_server_set_debug(server->gatt, gatt_debug_cb,
							 "server: ", NULL);
	 }
 
 
	 // 【步骤8】向数据库中填充所有标准服务和特征
	 // 注意：此时 bt_gatt_server 已经持有了 server->db 的引用，可以安全地修改数据库内容
	 populate_db(server);
 
	 // 【修复开始】
	 // 1. 为 external_data_ptr 分配内存并原地生成 1~CUSTOM_DATA_LEN 递增测试数据
	 // 避免额外的源数组占用和 memcpy 开销，确保数据生命周期与 server 一致
	 server->external_data_len = CUSTOM_DATA_LEN;
	 server->external_data_ptr = malloc(CUSTOM_DATA_LEN);
	 if (!server->external_data_ptr) {
		 fprintf(stderr, "Failed to allocate memory for external data\n");
		 server->external_data_len = 0;
	 } else {
		 uint8_t *buf = (uint8_t *)server->external_data_ptr;
		 for (int i = 0; i < CUSTOM_DATA_LEN; i++) {
			 buf[i] = (uint8_t)(i + 1); // uint8_t 自动取低8位: 1..255, 0, 1..255, 0...
		 }
		 PRLOG("Initialized external data: %d bytes (dynamic pattern)\n", CUSTOM_DATA_LEN);
	 }
 
	 // 2. Notify/Indicate timer starts after client writes CCCD
	 server->custom_notify_enabled = false;
	 server->custom_timeout_id = 0;
	 server->bridge_flush_id = 0;
 
	 g_active_server = server;
 
	 // 初始化全部成功，返回构建好的服务器实例
	 return server;
 
 fail:
	 // 【异常处理路径】如果中途任何一步失败，执行严格的资源回滚释放
	 gatt_db_unref(server->db);      // 减少数据库引用计数（若为0则销毁）
	 free(server->device_name);      // 释放设备名称占用的堆内存
	 bt_att_unref(server->att);      // 减少 ATT 传输层引用计数（若为0则关闭socket）
	 free(server);                   // 释放最外层的 server 结构体内存
 
 
 
	 return NULL; // 返回空指针，告知调用者初始化失败
 }
 
 
 
 /*--------------------------------------------------------------------------
  * 19. 销毁服务器实例 (资源清理)
  *--------------------------------------------------------------------------*/
 /*
  * server_destroy: 优雅地关闭并释放 GATT 服务器相关的所有资源
  *                 通常在蓝牙连接断开或程序退出时被调用。
  * 
  * @server: 指向核心结构体 struct server 的指针
  */
 static void server_destroy(struct server *server)
 {
	 if (g_active_server == server)
		 g_active_server = NULL;
 
	 if (server->hr_timeout_id)
		 timeout_remove(server->hr_timeout_id);
	 if (server->custom_timeout_id)
		 timeout_remove(server->custom_timeout_id);
	 if (server->bridge_flush_id)
		 timeout_remove(server->bridge_flush_id);

	 file_rx_abort(server);
	 file_tx_abort(server);
	 file_live_abort(server);
	 stream_abort(server);
	 free(server->external_data_ptr);
	 free(server->device_name);
 
 
	 // 【步骤2】释放 GATT 服务器对象
	 // 减少 bt_gatt_server 对象的引用计数。当计数归零时，BlueZ 会自动清理底层的 GATT 上下文
	 bt_gatt_server_unref(server->gatt);

	 /*
	  * server_create() owns the reference returned by bt_att_new().  The
	  * bt_gatt_server has a separate reference, so unref'ing only the GATT
	  * server leaves this ATT object (and its close-on-unref L2CAP socket)
	  * alive after the first phone disconnect.  The stale ATT session then
	  * collides with the next accepted connection and Android observes a
	  * momentary CONNECTED followed immediately by fbp-code 6.
	 */
	 bt_att_unref(server->att);

	 // 【步骤3】释放 GATT 数据库容器
	 // 减少 gatt_db 对象的引用计数，最终释放内存中存储的所有服务、特征及描述符数据
	 gatt_db_unref(server->db);
	 free(server);
 }
 
 
 
 
 
 /*--------------------------------------------------------------------------
  * 20. 监听并接受 BLE ATT 通道连接 (L2CAP Socket)
  *--------------------------------------------------------------------------*/
 /*
  * l2cap_le_att_listen_and_accept: 创建一个 L2CAP socket，并在指定的 ATT 信道上监听、接受中心设备的连接请求
  *                                 这是基于 Linux BlueZ 的底层网络编程方式，直接操作 socket 而非使用 GATT Server 高层 API。
  * 
  * @src:      本地蓝牙适配器的物理地址 (BD_ADDR)
  * @sec:      期望的安全等级 (例如 BT_SECURITY_LOW, BT_SECURITY_MEDIUM 等)
  * @src_type: 本地蓝牙地址类型 (BLE_PUBLIC_ADDR / BLE_RANDOM_ADDR)
  * @return:   成功返回新建立的连接文件描述符 (nsk)，失败返回 -1
  */
 static int l2cap_le_att_listen_and_accept(bdaddr_t *src, int sec, uint8_t src_type)
 {
	 int sk, nsk;              // sk: 监听用的服务端socket; nsk: accept后返回的新建连接socket
	 struct sockaddr_l2 srcaddr, addr; // L2CAP 协议的 socket 地址结构体
	 socklen_t optlen;         // 用于获取对端地址长度的变量
	 struct bt_security btsec; // 用于设置蓝牙安全级别的配置结构体
	 char ba[18];              // 用于存放转换后的蓝牙地址字符串 (格式如 "AA:BB:CC:DD:EE:FF\0")
 
	 // 【步骤1】创建 L2CAP 套接字
	 // PF_BLUETOOTH: 指定协议族为蓝牙
	 // SOCK_SEQPACKET: 指定套接字类型为有序分组包（保留消息边界，非常适合传输 ATT PDU）
	 // BTPROTO_L2CAP: 指定具体使用的蓝牙协议为 L2CAP
	 sk = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
	 if (sk < 0) {
		 perror("Failed to create L2CAP socket");
		 return -1;
	 }
 
	 // 【步骤2】配置并绑定本地源地址
	 memset(&srcaddr, 0, sizeof(srcaddr));
	 srcaddr.l2_family = AF_BLUETOOTH;          // 设置地址族为蓝牙
	 srcaddr.l2_cid = htobs(ATT_CID);           // 关键：绑定到 ATT 专属的信道标识符 (CID = 0x0004)
	 srcaddr.l2_bdaddr_type = src_type;         // 设置地址类型 (公共地址或随机地址)
	 bacpy(&srcaddr.l2_bdaddr, src);            // 拷贝本地蓝牙物理地址
 
	 // 将 socket 与上述配置的地址进行绑定
	 if (bind(sk, (struct sockaddr *) &srcaddr, sizeof(srcaddr)) < 0) {
		 perror("Failed to bind L2CAP socket");
		 goto fail; // 发生错误，跳转至清理逻辑
	 }
 
	 // 【步骤3】设置链路层安全级别
	 memset(&btsec, 0, sizeof(btsec));
	 btsec.level = sec; // 设定要求的安全等级（如是否需要配对/加密）
	 if (setsockopt(sk, SOL_BLUETOOTH, BT_SECURITY, &btsec, sizeof(btsec)) != 0) {
		 fprintf(stderr, "Failed to set L2CAP security level\n");
		 goto fail;
	 }
 
	 // 【步骤4】开始被动监听连接请求
	 // backlog 设置为 10，表示内核允许排队的最大未完成连接请求数
	 if (listen(sk, 10) < 0) {
		 perror("Listening on socket failed");
		 goto fail;
	 }
 
	 printf("Started listening on ATT channel. Waiting for connections\n");

	 /* 【步骤5】等待并接受传入的连接 */
	 memset(&addr, 0, sizeof(addr));
	 optlen = sizeof(addr);

#ifdef MY_SERVER_EMBEDDED
	 /*
	  * Embedded in my-fnirs-ev: libevent owns SIGINT/SIGTERM — do not
	  * clobber handlers. Poll accept() so g_exit_requested can stop us.
	  */
	 g_listen_sk = sk;
	 if (fcntl(sk, F_SETFL, fcntl(sk, F_GETFL, 0) | O_NONBLOCK) < 0) {
		 perror("fcntl O_NONBLOCK");
		 g_listen_sk = -1;
		 goto fail;
	 }
	 nsk = -1;
	 for (;;) {
		 if (g_exit_requested)
			 break;
		 nsk = accept(sk, (struct sockaddr *) &addr, &optlen);
		 if (nsk >= 0)
			 break;
		 if (errno == EINTR)
			 continue;
		 if (errno == EBADF)
			 break;
		 if (errno != EAGAIN && errno != EWOULDBLOCK) {
			 perror("Accept failed");
			 break;
		 }
		 usleep(100000);
	 }
	 g_listen_sk = -1;
#else
	 {
		 struct sigaction sa, old_int, old_term;

		 memset(&sa, 0, sizeof(sa));
		 sa.sa_handler = accept_interrupt_sig;
		 sigemptyset(&sa.sa_mask);
		 sigaction(SIGINT, &sa, &old_int);
		 sigaction(SIGTERM, &sa, &old_term);
		 g_listen_sk = sk;
		 nsk = accept(sk, (struct sockaddr *) &addr, &optlen);
		 g_listen_sk = -1;
		 sigaction(SIGINT, &old_int, NULL);
		 sigaction(SIGTERM, &old_term, NULL);
	 }
#endif

	 if (nsk < 0) {
		 if (errno == EINTR || errno == EBADF || g_exit_requested)
			 fprintf(stderr, "Accept interrupted\n");
		 else
			 perror("Accept failed");
		 goto fail;
	 }
 
	 // 【步骤6】打印连接成功的日志并清理监听套接字
	 ba2str(&addr.l2_bdaddr, ba); // 将二进制的蓝牙地址转换为可读的字符串格式
	 printf("Connect from %s\n", ba);
	 close(sk); // 关闭不再需要的原始监听套接字，只保留新建的连接套接字(nsk)
 
	 return nsk; // 返回新连接的 fd，后续可通过此 fd 收发 ATT 数据
 
 fail:
	 // 【异常处理路径】统一释放资源并返回错误码
	 close(sk);
	 return -1;
 }
 
 
 
 /*--------------------------------------------------------------------------
  * 22. 定义 getopt_long 长选项配置表
  *--------------------------------------------------------------------------*/
 /*
  * notify_options: 供标准库函数 getopt_long() 使用的选项数组
  *                 结构体成员含义依次为：{ 长选项名称, 是否需要参数, 标志位, 对应的短选项字符 }
  */
 static struct option notify_options[] = {
	 { "indicate", 0, 0, 'i' }, // 定义 "--indicate" 长选项，不需要传参(0)，映射到短选项 'i'
	 { }                         // 必须以全零的结构体作为数组的结束标记
 };
 
 
 
 /*--------------------------------------------------------------------------
  * 24. 确认回调函数 (用于 Indication)
  *--------------------------------------------------------------------------*/
 /*
  * conf_cb: 当客户端对发出的 Indication 做出确认(Confirmation)时触发
  *          注意：Notification 不需要确认，因此不会触发此回调。
  */
 static void conf_cb(void *user_data)
 {
	 PRLOG("Received confirmation\n"); // 打印日志，表明收到了来自中心设备的确认响应
 }
 
 
 
 
 
 /*--------------------------------------------------------------------------
  * 28. UUID 格式化工具 (底层打印辅助)
  *--------------------------------------------------------------------------*/
 /*
  * print_uuid: 将底层的 bt_uuid_t 结构体转换为标准的字符串格式并打印
  *             为了统一显示，无论原始是 16-bit 还是 32-bit UUID，均先转为 128-bit 格式再输出。
  * 
  * @uuid: 指向待转换的蓝牙 UUID 结构体的指针
  */
 static void print_uuid(const bt_uuid_t *uuid)
 {
	 char uuid_str[MAX_LEN_UUID_STR]; // 存放最终生成的 UUID 字符串缓冲区
	 bt_uuid_t uuid128;               // 用于暂存转换后的 128-bit UUID
 
	 // 将任意位宽的 UUID 统一扩展为标准 128-bit UUID
	 bt_uuid_to_uuid128(uuid, &uuid128);
	 // 将二进制 UUID 转换为可读的十六进制字符串 (如 "0000180d-0000-1000-8000-00805f9b34fb")
	 bt_uuid_to_string(&uuid128, uuid_str, sizeof(uuid_str));
 
	 printf("%s\n", uuid_str); // 打印格式化后的 UUID 字符串
 }
 
 
 /*--------------------------------------------------------------------------
  * 29. 打印 Include 声明 (包含其他服务的引用)
  *--------------------------------------------------------------------------*/
 /*
  * print_incl: 作为 gatt_db_service_foreach_incl 的回调函数，打印当前服务中包含的其他服务信息
  *             在 GATT 数据库中，Include 允许一个服务引用另一个已定义的服务。
  * 
  * @attr:      当前的 Include 属性句柄
  * @user_data: 用户数据指针，此处传入的是 struct server 核心实例
  */
 static void print_incl(struct gatt_db_attribute *attr, void *user_data)
 {
	 struct server *server = user_data;
	 uint16_t handle, start, end;
	 struct gatt_db_attribute *service;
	 bt_uuid_t uuid;
 
	 // 【步骤1】提取 Include 属性的元数据（自身句柄、被包含服务的起始与结束句柄）
	 if (!gatt_db_attribute_get_incl_data(attr, &handle, &start, &end))
		 return; // 如果获取失败，直接返回
 
	 // 【步骤2】根据起始句柄，从数据库中查找被包含的目标服务
	 service = gatt_db_get_attribute(server->db, start);
	 if (!service)
		 return;
 
	 // 获取目标服务的 UUID
	 gatt_db_attribute_get_service_uuid(service, &uuid);
 
	 // 【步骤3】使用绿色高亮打印 Include 信息
	 printf("\t  " COLOR_GREEN "include" COLOR_OFF " - handle: "
					 "0x%04x, - start: 0x%04x, end: 0x%04x,"
					 "uuid: ", handle, start, end);
	 print_uuid(&uuid); // 调用工具函数打印目标服务的 UUID
 }
 
 
 /*--------------------------------------------------------------------------
  * 30. 打印描述符 (Descriptor)
  *--------------------------------------------------------------------------*/
 /*
  * print_desc: 作为 gatt_db_service_foreach_desc 的回调函数，打印特征的附属描述符信息
  *             常见的描述符包括 CCCD (客户端配置描述符) 或扩展属性描述符等。
  * 
  * @attr:      当前的描述符属性句柄
  * @user_data: 未使用的用户数据指针
  */
 static void print_desc(struct gatt_db_attribute *attr, void *user_data)
 {
	 // 使用洋红色(Magenta)高亮打印描述符的句柄及其 UUID 类型
	 printf("\t\t  " COLOR_MAGENTA "descr" COLOR_OFF
					 " - handle: 0x%04x, uuid: ",
					 gatt_db_attribute_get_handle(attr));
	 print_uuid(gatt_db_attribute_get_type(attr)); // 获取并打印描述符自身的 UUID
 }
 
 
 /*--------------------------------------------------------------------------
  * 31. 打印特征 (Characteristic)
  *--------------------------------------------------------------------------*/
 /*
  * print_chrc: 作为 gatt_db_service_foreach_char 的回调函数，打印特征的核心属性
  *             并在打印完特征本身后，进一步递归遍历其下属的所有描述符。
  * 
  * @attr:      当前的特征属性句柄
  * @user_data: 未使用的用户数据指针
  */
 static void print_chrc(struct gatt_db_attribute *attr, void *user_data)
 {
	 uint16_t handle, value_handle; // handle: 特征声明句柄; value_handle: 特征值句柄
	 uint8_t properties;            // 特征属性标志位 (如 READ, WRITE, NOTIFY 等)
	 uint16_t ext_prop;             // 扩展属性标志位
	 bt_uuid_t uuid;                // 特征的 UUID
 
	 // 【步骤1】提取特征的各项元数据
	 if (!gatt_db_attribute_get_char_data(attr, &handle,
								 &value_handle,
								 &properties,
								 &ext_prop,
								 &uuid))
		 return;
 
	 // 【步骤2】使用黄色(Yellow)高亮打印特征的基本信息
	 printf("\t  " COLOR_YELLOW "charac" COLOR_OFF
				 " - start: 0x%04x, value: 0x%04x, "
				 "props: 0x%02x, ext_prop: 0x%04x, uuid: ",
				 handle, value_handle, properties, ext_prop);
	 print_uuid(&uuid); // 打印特征的 UUID
 
	 // 【步骤3】递归遍历该特征下的所有描述符，并调用 print_desc 进行打印
	 gatt_db_service_foreach_desc(attr, print_desc, NULL);
 }
 
 
 /*--------------------------------------------------------------------------
  * 32. 打印主服务 (Service) - 顶层入口
  *--------------------------------------------------------------------------*/
 /*
  * print_service: 作为 gatt_db_foreach_service 的回调函数，打印服务的整体信息
  *                这是整个打印逻辑的根节点，它会触发对内部 Include 和 Characteristic 的遍历。
  * 
  * @attr:      当前的服务属性句柄
  * @user_data: 用户数据指针，传入 struct server 以支持 Include 查询
  */
 static void print_service(struct gatt_db_attribute *attr, void *user_data)
 {
	 struct server *server = user_data;
	 uint16_t start, end; // 服务在数据库中的起始和结束句柄范围
	 bool primary;        // 标记该服务是主服务(Primary)还是次要服务(Secondary)
	 bt_uuid_t uuid;      // 服务的 UUID
 
	 // 【步骤1】提取服务的元数据
	 if (!gatt_db_attribute_get_service_data(attr, &start, &end, &primary,
									 &uuid))
		 return;
 
	 // 【步骤2】使用红色(Red)高亮打印服务头信息
	 printf(COLOR_RED "service" COLOR_OFF " - start: 0x%04x, "
				 "end: 0x%04x, type: %s, uuid: ",
				 start, end, primary ? "primary" : "secondary");
	 print_uuid(&uuid); // 打印服务的 UUID
 
	 // 【步骤3】深度遍历该服务内部的子元素
	 // 首先遍历并打印该服务引用的 Include 项
	 gatt_db_service_foreach_incl(attr, print_incl, server);
	 // 接着遍历并打印该服务包含的所有特征 (print_chrc 内部又会继续遍历描述符)
	 gatt_db_service_foreach_char(attr, print_chrc, NULL);
 
	 printf("\n"); // 每个服务打印完毕后换行，保持输出整洁
 }
 
 
 
 
 /*--------------------------------------------------------------------------
  * 33. 执行服务列表打印指令
  *--------------------------------------------------------------------------*/
 /*
  * cmd_services: 触发遍历整个 GATT 数据库，并以彩色层级结构在终端打印所有已注册的服务、特征及描述符
  * 
  * @server:   指向核心结构体 struct server 的指针
  * @cmd_str:  未使用的命令字符串参数
  */
 static void cmd_services(struct server *server, char *cmd_str)
 {
	 // 调用 BlueZ 库的全局遍历接口，对每个找到的 Service 回调 print_service 进行格式化输出
	 gatt_db_foreach_service(server->db, NULL, print_service, server);
 }
 
 
 /*--------------------------------------------------------------------------
  * 34. 十六进制字符串转 CSRK 密钥数组
  *--------------------------------------------------------------------------*/
 /*
  * convert_sign_key: 将用户输入的 32 位十六进制字符串转换为 16 字节的远程连接签名解析密钥 (Remote CSRK)
  *                   CSRK 用于在 LE Secure Connections 中对 ATT 数据包进行签名认证。
  * 
  * @optarg: 包含 32 个字符的十六进制字符串 (如 "D8515948451FEA320DC05A2E88308188")
  * @key:    目标 16 字节缓冲区
  * @return: true 表示转换成功；false 表示格式错误或长度不合法
  */
 static bool convert_sign_key(char *optarg, uint8_t key[16])
 {
	 int i;
 
	 // 【步骤1】严格校验输入长度。16 字节的密钥需要 32 个十六进制字符来表示
	 if (strlen(optarg) != 32) {
		 printf("sign-key length is invalid\n");
		 return false;
	 }
 
	 // 【步骤2】每两个字符为一组，循环解析为单字节并填入 key 数组
	 for (i = 0; i < 16; i++) {
		 // "%2hhx" 指示 sscanf 读取两位十六进制数并存入 unsigned char
		 if (sscanf(optarg + (i * 2), "%2hhx", &key[i]) != 1)
			 return false; // 如果某个字节解析失败，立即返回错误
	 }
 
	 return true;
 }
 
 
 /*--------------------------------------------------------------------------
  * 35. 打印设置签名密钥的帮助信息
  *--------------------------------------------------------------------------*/
 /*
  * set_sign_key_usage: 向用户展示如何设置远程设备的签名密钥 (CSRK)
  */
 static void set_sign_key_usage(void)
 {
	 printf("Usage: set-sign-key [options]\nOptions:\n"
		 "\t -c, --sign-key <remote csrk>\tRemote CSRK\n"
		 "e.g.:\n"
		 "\tset-sign-key -c D8515948451FEA320DC05A2E88308188\n");
 }
 
 
 /*--------------------------------------------------------------------------
  * 36. 远程签名计数器 (Sign Counter) 验证回调
  *--------------------------------------------------------------------------*/
 /*
  * remote_counter: 作为 bt_att_set_remote_key 的回调函数，由协议栈在收到带签名的 ATT PDU 时调用
  *                 用于防止重放攻击 (Replay Attack)。确保接收到的签名包序号是递增的。
  * 
  * @sign_cnt: 当前收到的数据包中携带的签名计数器值
  * @user_data: 用户数据指针
  * @return:   true 允许该数据包通过验证；false 丢弃该数据包
  */
 static bool remote_counter(uint32_t *sign_cnt, void *user_data)
 {
	 static uint32_t cnt = 0; // 静态变量，记录本地已知的最大有效计数器值
 
	 // 如果收到的计数器小于或等于本地记录的最大值，说明是过期或重放的包，拒绝处理
	 if (*sign_cnt <= cnt) // 注：原代码逻辑为 *sign_cnt < cnt，通常应为 <= 以拒绝重复序号
		 return false;
 
	 cnt = *sign_cnt; // 更新本地记录的最大计数器值
 
	 return true; // 计数器合法，允许处理该数据包
 }
 static bool parse_args(char *cmd_str, int max_args, char *argv[], int *argc)
 {
	 char *tok;
	 int n = 0;
 
	 if (!cmd_str || !argv || !argc || max_args <= 0)
		 return false;
 
	 *argc = 0;
	 tok = strtok(cmd_str, " \t\r\n");
	 while (tok && n < max_args) {
		 argv[n++] = tok;
		 tok = strtok(NULL, " \t\r\n");
	 }
	 *argc = n;
 
	 /* Return false if there are more tokens than max_args. */
	 return tok == NULL;
 }
 
 /*--------------------------------------------------------------------------
  * 37. 执行设置远程签名密钥指令
  *--------------------------------------------------------------------------*/
 /*
  * cmd_set_sign_key: 解析用户输入的命令，提取远程设备的 CSRK 并将其注册到 ATT 传输层
  * 
  * @server:   指向核心结构体 struct server 的指针
  * @cmd_str:  用户输入的原始命令字符串 (例如："set-sign-key -c D85159...")
  */
 static void cmd_set_sign_key(struct server *server, char *cmd_str)
 {
	 char *argv[3]; // 预分配足够的参数槽位
	 int argc = 0;
	 uint8_t key[16]; // 存放解析后的 16 字节密钥
 
	 memset(key, 0, 16); // 初始化密钥缓冲区
 
	 // 【步骤1】预处理与分词
	 if (!parse_args(cmd_str, 2, argv, &argc)) {
		 set_sign_key_usage();
		 return;
	 }
 
	 // 预期必须有两个参数：选项标识(-c) 和 密钥字符串
	 if (argc != 2) {
		 set_sign_key_usage();
		 return;
	 }
 
	 // 【步骤2】匹配选项并提取密钥
	 if (!strcmp(argv[0], "-c") || !strcmp(argv[0], "--sign-key")) {
		 // 尝试将十六进制字符串转换为二进制密钥数组
		 if (convert_sign_key(argv[1], key)) {
			 // 将密钥及防重放回调函数注册到底层 ATT 对象中
			 bt_att_set_remote_key(server->att, key, remote_counter, server);
		 }
	 } else {
		 // 未知的选项，打印帮助信息
		 set_sign_key_usage();
	 }
 }
 
 
 /*--------------------------------------------------------------------------
  * 41. 系统信号捕获回调
  *--------------------------------------------------------------------------*/
 /*
  * signal_cb: 捕获操作系统发送的信号（如 Ctrl+C），实现程序的优雅退出
  * 
  * @signum:    捕获到的信号编号
  * @user_data: 用户数据指针
  */
 static void signal_cb(int signum, void *user_data)
 {
	 switch (signum) {
	 case SIGINT:
	 case SIGTERM:
		 g_exit_requested = 1;
		 if (g_listen_sk >= 0)
			 accept_interrupt_sig(signum);
		 mainloop_quit();
		 break;
	 default:
		 break;
	 }
 }
 
 
 /*--------------------------------------------------------------------------
  * 42. 程序主入口 (Main Loop)
  *--------------------------------------------------------------------------*/
 /*
  * main: 整个 GATT Server 工具的生命周期管理器
  */
#ifdef MY_SERVER_EMBEDDED
static int embedded_listen_create(void)
{
	bdaddr_t src_addr;
	struct sockaddr_l2 srcaddr;
	struct bt_security btsec;
	int flags;
	int sk;

	bacpy(&src_addr, BDADDR_ANY);
	sk = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
	if (sk < 0) {
		perror("Failed to create L2CAP socket");
		return -1;
	}

	memset(&srcaddr, 0, sizeof(srcaddr));
	srcaddr.l2_family = AF_BLUETOOTH;
	srcaddr.l2_cid = htobs(ATT_CID);
	srcaddr.l2_bdaddr_type = BDADDR_LE_PUBLIC;
	bacpy(&srcaddr.l2_bdaddr, &src_addr);
	if (bind(sk, (struct sockaddr *)&srcaddr, sizeof(srcaddr)) < 0) {
		perror("Failed to bind L2CAP socket");
		goto fail;
	}

	memset(&btsec, 0, sizeof(btsec));
	btsec.level = BT_SECURITY_LOW;
	if (setsockopt(sk, SOL_BLUETOOTH, BT_SECURITY, &btsec,
							sizeof(btsec)) != 0) {
		fprintf(stderr, "Failed to set L2CAP security level\n");
		goto fail;
	}

	if (listen(sk, 10) < 0) {
		perror("Listening on socket failed");
		goto fail;
	}

	flags = fcntl(sk, F_GETFL, 0);
	if (flags < 0 || fcntl(sk, F_SETFL, flags | O_NONBLOCK) < 0) {
		perror("fcntl O_NONBLOCK");
		goto fail;
	}

	printf("Started nonblocking ATT listener\n");
	return sk;

fail:
	close(sk);
	return -1;
}

int my_server_embedded_start(void)
{
	if (g_listen_sk >= 0)
		return 0;

	g_exit_requested = 0;
	g_session_ended = 0;
	mainloop_init();
	g_listen_sk = embedded_listen_create();
	if (g_listen_sk < 0) {
		mainloop_cleanup();
		return -1;
	}

	printf("my-server build: %s (libevent embedded)\n", MY_SERVER_BUILD_ID);
	return 0;
}

int my_server_embedded_get_listen_fd(void)
{
	return g_listen_sk;
}

int my_server_embedded_get_mainloop_fd(void)
{
	return mainloop_get_fd();
}

int my_server_embedded_accept(void)
{
	struct sockaddr_l2 addr;
	socklen_t addrlen = sizeof(addr);
	char ba[18];
	int fd;

	if (g_listen_sk < 0 || g_active_server)
		return 0;

	memset(&addr, 0, sizeof(addr));
	fd = accept(g_listen_sk, (struct sockaddr *)&addr, &addrlen);
	if (fd < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
			return 0;
		perror("Accept failed");
		return -1;
	}

	ba2str(&addr.l2_bdaddr, ba);
	printf("Connect from %s\n", ba);
	g_session_ended = 0;
	if (!server_create(fd, 517, true)) {
		close(fd);
		return -1;
	}

	printf("Running GATT server on application event loop\n");
	return 1;
}

int my_server_embedded_dispatch(void)
{
	int ret = mainloop_iterate();
	int session_ended = 0;

	if (g_session_ended && g_active_server) {
		struct server *server = g_active_server;

		g_session_ended = 0;
		printf("Session ended, waiting for next connection...\n");
		server_destroy(server);
		session_ended = 1;
	}

	return ret < 0 ? ret : session_ended;
}

void my_server_embedded_stop(void)
{
	g_exit_requested = 1;
	if (g_listen_sk >= 0) {
		close(g_listen_sk);
		g_listen_sk = -1;
	}
	if (g_active_server)
		server_destroy(g_active_server);
	bridge_uart_close();
	mainloop_cleanup();
}

int my_server_embedded_main(int argc, char *argv[])
#else
 int main(int argc, char *argv[])
#endif
 {
	 int opt;
	 bdaddr_t src_addr;
	 int dev_id = -1;
	 int fd;
	 int sec = BT_SECURITY_LOW;
	 uint8_t src_type = BDADDR_LE_PUBLIC;
	 uint16_t mtu = 517;
	 bool hr_visible = true;
	 bool bridge_enable = true;
	 bool run_once = false;
	 const char *bridge_uart = NULL;
	 struct server *server;

	 setenv("TZ", "CST-8", 1);
	 tzset();

	 while ((opt = getopt(argc, argv, "u:Uvo")) != -1) {
		 switch (opt) {
		 case 'u':
			 bridge_uart = optarg;
			 break;
		 case 'U':
			 bridge_enable = false;
			 break;
		 case 'v':
			 verbose = true;
			 break;
		 case 'o':
			 run_once = true;
			 break;
		 default:
			 fprintf(stderr, "Usage: %s [-u /dev/ttyS1] [-U disable bridge] [-v] [-o once]\n",
				 argv[0]);
			 return EXIT_FAILURE;
		 }
	 }
 
	 if (!bridge_uart || !bridge_uart[0]) {
		 bridge_uart = getenv("BLE_BRIDGE_UART");
		 if (!bridge_uart || !bridge_uart[0])
			 bridge_uart = BRIDGE_UART_DEFAULT;
	 }
 
	 if (getenv("BLE_BRIDGE_DISABLE"))
		 bridge_enable = false;
 
	 // 【阶段2】确定并获取本地蓝牙物理地址
	 if (dev_id == -1)
		 bacpy(&src_addr, BDADDR_ANY);
	 else if (hci_devba(dev_id, &src_addr) < 0) {
		 perror("Adapter not available");
		 return EXIT_FAILURE;
	 }
 
	 printf("my-server build: %s\n", MY_SERVER_BUILD_ID);
 
	 mainloop_init();
 
	 if (bridge_enable && g_bridge_uart_fd < 0)
		 g_bridge_uart_fd = bridge_uart_open(bridge_uart, B2000000);
 
	 while (!g_exit_requested) {
		 fd = l2cap_le_att_listen_and_accept(&src_addr, sec, src_type);
		 if (fd < 0) {
			 if (g_exit_requested)
				 break;
			 fprintf(stderr, "Failed to accept L2CAP ATT connection\n");
			 return EXIT_FAILURE;
		 }
 
		 server = server_create(fd, mtu, hr_visible);
		 if (!server) {
			 close(fd);
			 continue;
		 }
 
		 printf("Running GATT server\n");
#ifdef MY_SERVER_EMBEDDED
		 mainloop_run();
#else
		 mainloop_run_with_signal(signal_cb, NULL);
#endif
 
		 printf("\nSession ended, waiting for next connection...\n");
		 server_destroy(server);
		 if (run_once)
			 break;

		 /*
		  * mainloop_run() is single-use: after mainloop_quit() it removes all
		  * registered fds and closes epoll_fd.  A new ATT session therefore
		  * needs a fresh mainloop before bt_att_new() tries io_new(); without
		  * this, every connection after the first fails with
		  * "Failed to initialze ATT transport layer".
		  */
		 mainloop_init();
	 }
 
	 printf("\nShutting down...\n");
	 bridge_uart_close();
	 return EXIT_SUCCESS;
 }
