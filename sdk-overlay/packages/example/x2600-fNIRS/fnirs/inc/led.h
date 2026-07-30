
#ifndef _LED_H_
#define _LED_H_

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */

#include "stdint.h"
#include "my_interface.h"

typedef enum {
    LED_1,
    LED_2,
    LED_MAX,
}led_t;

typedef enum {
    SAMPLE_IDLE,
    SAMPLE_AUTOGAIN,
    SAMPLE_RUNNING,
}led1_evt_t;

typedef enum {
    WIFI_AP,
    WIFI_STA_CONNECTING,
    WIFI_STA_CONNECTED,
}led2_evt_t;

int32_t led_init(void);
int32_t led_exit(void);
void led_scan_nodes(void);
void led_sample_nodes(int flag);
void led_can_nodes(int flag);
void led_system(int status);
void led_all_off(void);

void led_set_evt(uint32_t led, uint32_t evt);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif
