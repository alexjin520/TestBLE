#ifndef EV_BLE_ASYNC_H_
#define EV_BLE_ASYNC_H_

#include <stdint.h>

struct event_base;

#ifdef __cplusplus
extern "C" {
#endif

int32_t ev_ble_async_dispatch(uint8_t cmd, const uint8_t *req, uint16_t req_len,
                              uint8_t *status_out, uint8_t *rsp, uint16_t rsp_max,
                              uint16_t *rsp_len_out);

void fnirs_ble_ev_process_job(uint8_t cmd, const uint8_t *payload, uint16_t len,
                              int rsp_fd);

void fnirs_ble_ev_init(struct event_base *base);
void fnirs_ble_ev_shutdown(void);
void fnirs_ble_ev_cancel_async(void);

#ifdef __cplusplus
}
#endif

#endif
