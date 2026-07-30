#ifndef _BLE_SERVICE_H_
#define _BLE_SERVICE_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "stdint.h"

int32_t ble_service_init(void);
int32_t ble_service_start(void);
void ble_service_stop(void);
void ble_service_tick(void);

#ifdef __cplusplus
}
#endif

#endif
