
#ifndef _FNIRS_H_
#define _FNIRS_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "stdint.h"
#include "fnode.h"
#include "fhub.h"
#include "my_interface.h"

int32_t fNIRS_init(void);
int32_t fNIRS_lazy_init(void);

int32_t fNIRS_exit(void);

int32_t fNIRS_reset(void);

int32_t fNIRS_s_gain(uint8_t gain);

fnode_handler_t fNIRS_handler_ref(void);

int32_t fNIRS_on(void);

int32_t fNIRS_off(void);

uint32_t fNIRS_ison(void);

int32_t fNIRS_desc(fnode_desc_t desc[FNODE_NID_MAX]);

int32_t fNIRS_ota_start(void);
int32_t fNIRS_ota_cancel(void);

int32_t fNIRS_factory_test_mode_enter(uint8_t flag);
int32_t fNIRS_factory_test_mode_FUSB_MEASURE(uint8_t* data, uint8_t data_len);
int32_t fNIRS_factory_test_mode_FUSB_MEASURE_ALTERNATE(uint8_t* data, uint8_t data_len);
int32_t fNIRS_factory_test_mode_FUSB_CALI_PD(uint8_t* data, uint8_t data_len);
int32_t fNIRS_factory_test_mode_LED_ON(uint8_t* data, uint8_t data_len);
int32_t fNIRS_factory_test_mode_LED_ON_FUSB_MEASURE(uint8_t* data, uint8_t data_len);


#ifdef __cplusplus
}
#endif

#endif
