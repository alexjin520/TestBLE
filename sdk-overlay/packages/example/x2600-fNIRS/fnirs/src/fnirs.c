
#include "stdio.h"
#include "string.h"
#include "errno.h"
#include "stdlib.h"
#include "unistd.h"
#include "signal.h"
#include "pthread.h"
#include "linux/list.h"

#include "fnode.h"
#include "fhub.h"

#ifdef FNIRS_EV_IO
#include "ev_can.h"
#endif

static fnode_handler_t fNIRS_handler;
static int g_fnirs_inited;

int32_t fNIRS_init(void)
{
    int32_t rc = -1;
	
    if (g_fnirs_inited)
        return 0;

    rc = fnode_init(&fNIRS_handler, FNODE_GROUP0);
    if (rc == 0) {
        g_fnirs_inited = 1;
#ifdef FNIRS_EV_IO
        ev_can_try_attach();
        WLOGI("fNIRS lazy init ok, CAN/node ready\r\n");
#endif
    }

    return rc;
}

int32_t fNIRS_lazy_init(void)
{
    if (g_fnirs_inited)
        return 0;

    return fNIRS_init();
}

int32_t fNIRS_exit(void)
{
    int32_t rc = -1;

    if (!g_fnirs_inited)
        return 0;
	
    rc = fnode_exit(fNIRS_handler);
	
    fNIRS_handler = NULL;
    g_fnirs_inited = 0;
	
    return rc;
}

int32_t fNIRS_reset(void)
{
    int32_t rc = -1;
	
    rc = fnode_reset(fNIRS_handler);
	
    return rc;
}

int32_t fNIRS_s_gain(uint8_t gain)
{
    int32_t rc = -1;
	
    rc = fnode_s_gain(fNIRS_handler, gain);
	
    return rc;
}

fnode_handler_t fNIRS_handler_ref(void)
{
    return fNIRS_handler;
}

int32_t fNIRS_on(void)
{
    int32_t rc = -1;

    rc = fNIRS_lazy_init();
    if (rc != 0)
        return rc;

    rc = fnode_sample_on(fNIRS_handler);

    return rc;
}

int32_t fNIRS_off(void)
{
    int32_t rc = -1;
    
    rc = fnode_sample_off(fNIRS_handler);
	
    usleep(20000);
	
    fnode_reset(fNIRS_handler);
	
    return rc;
}

uint32_t fNIRS_ison(void)
{
    return fnode_sample_ison(fNIRS_handler);
}

int32_t fNIRS_desc(fnode_desc_t desc[FNODE_NID_MAX])
{
    if (fNIRS_lazy_init() != 0)
        return -1;

    return fnode_desc(fNIRS_handler, desc);
}

//2025-11-18 maomao add
int32_t fNIRS_ota_start(void)
{
    int32_t rc = -1;

    rc = fNIRS_lazy_init();
    if (rc != 0)
        return rc;

    rc = fnode_ota_start(fNIRS_handler);

    return rc;
}

int32_t fNIRS_ota_cancel(void)
{
    if (!g_fnirs_inited)
        return 0;

    return fnode_ota_cancel(fNIRS_handler);
}

//2025-11-26 maomao add
int32_t fNIRS_factory_test_mode_enter(uint8_t flag)
{
    int32_t rc = -1;
	
    rc = fnode_factory_test_mode_enter(fNIRS_handler, flag);
	
    return rc;
}



//2025-11-26 maomao add
int32_t fNIRS_factory_test_mode_FUSB_MEASURE(uint8_t* data, uint8_t data_len)
{
    int32_t rc = -1;
	
    rc = fnode_factory_test_mode_FUSB_MEASURE(fNIRS_handler, data, data_len);
	if(rc == -1)
	{
		printf("FUSB_MEASURE -1\r\n");
	}
	
    return rc;
}



//2026-04-22 huang add
int32_t fNIRS_factory_test_mode_FUSB_MEASURE_ALTERNATE(uint8_t* data, uint8_t data_len)
{
    int32_t rc = -1;
	
    rc = fnode_factory_test_mode_FUSB_MEASURE_ALTERNATE(fNIRS_handler, data, data_len);
	if(rc == -1)
	{
		printf("FUSB_MEASURE_ALTERNATE -1\r\n");
	}
	
    return rc;
}



int32_t fNIRS_factory_test_mode_FUSB_CALI_PD(uint8_t* data, uint8_t data_len)
{
    int32_t rc = -1;
	
    rc = fnode_factory_test_mode_FUSB_CALI_PD(fNIRS_handler, data, data_len);
	if(rc == -1)
	{
		printf("FUSB_MEASURE -1\r\n");
	}
	
    return rc;
}

int32_t fNIRS_factory_test_mode_LED_ON(uint8_t* data, uint8_t data_len)
{
    int32_t rc = -1;
	
    rc = fnode_factory_test_mode_FUSB_LED_ON(fNIRS_handler, data, data_len);
	if(rc == -1)
	{
		printf("FUSB_LED -1\r\n");
	}
	
    return rc;
}

//2025-12-17 maomao add fnode_factory_test_mode_LED_ON_FUSB_MEASURE
int32_t fNIRS_factory_test_mode_LED_ON_FUSB_MEASURE(uint8_t* data, uint8_t data_len)
{
    int32_t rc = -1;
	
    rc = fnode_factory_test_mode_LED_ON_FUSB_MEASURE(fNIRS_handler, data, data_len);
	if(rc == -1)
	{
		printf("LED_ON FUSB_MEASURE -1\r\n");
	}
	
    return rc;
}

