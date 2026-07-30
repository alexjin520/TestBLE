#ifndef _FNIRS_BLE_IPC_H_
#define _FNIRS_BLE_IPC_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "stdint.h"

#define FNIRS_BLE_SOCK_PATH       "/tmp/fnirs_ble.sock"
#define FNIRS_BLE_REQ_MAGIC0      0xFB
#define FNIRS_BLE_REQ_MAGIC1      0x10
#define FNIRS_BLE_RSP_MAGIC0      0xFB
#define FNIRS_BLE_RSP_MAGIC1      0x11

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

#define FNIRS_BLE_MAX_STREAM_CH     8
#define FNIRS_BLE_STATUS_OK         0
#define FNIRS_BLE_STATUS_FAIL       1

typedef struct {
    uint8_t src_node;
    uint8_t led_id;
    uint8_t det_node;
    uint8_t det_id;   /* photodiode 1..4 on det_node */
} fnirs_ble_stream_ch_t;

typedef struct {
    uint8_t count;
    fnirs_ble_stream_ch_t ch[FNIRS_BLE_MAX_STREAM_CH];
} fnirs_ble_stream_cfg_t;

int32_t fnirs_ble_ipc_init(void);
void fnirs_ble_ipc_exit(void);

/* One request/response per connected fd (used by my-fnirs-ev ev_ble_ipc). */
void fnirs_ble_handle_client(int fd);

/* In-process dispatch when GATT runs inside my-fnirs-ev (no Unix socket). */
int32_t fnirs_ble_dispatch_request(uint8_t cmd, const uint8_t *req, uint16_t req_len,
                                   uint8_t *status_out, uint8_t *rsp, uint16_t rsp_max,
                                   uint16_t *rsp_len_out);

void fnirs_ble_ipc_get_stream_cfg(fnirs_ble_stream_cfg_t *cfg);
int32_t fnirs_ble_ipc_stream_active(void);

#ifdef __cplusplus
}
#endif

#endif
