#include "ev_module.h"
#include "ev_app.h"

#include "ble_service.h"
#include "fNIRS.h"
#include "fdatalog.h"
#include "fnirs_ble_ipc.h"
#include "fnirs_stream.h"
#include "fusb.h"
#include "led.h"
#include "my_interface.h"

static int ev_fnirs_init(ev_app_t *app, ev_module_t *mod)
{
    (void)app;
    (void)mod;

    led_init();
    led_system(1);

#if !defined(FNIRS_EMBEDDED)
    ble_service_init();
    if (ble_service_start() != 0)
        WLOGW("BLE file service start failed (fNIRS continues)\r\n");
#endif

    fusb_init();
    fdatalog_init(NULL);

#if defined(FNIRS_EMBEDDED) && defined(FNIRS_EV_IO)
    WLOGI("fNIRS CAN/node deferred (lazy init on first BLE/UART command)\r\n");
#else
    if (fNIRS_init() != 0)
        WLOGW("fNIRS_init failed (CAN/node); GATT continues, retry after bus ready\r\n");
#endif

    fnirs_stream_init();

    if (fnirs_ble_ipc_init() != 0)
        WLOGW("fnirs_ble_ipc_init failed\r\n");

    WLOGI("fNIRS business modules ready\r\n");
    return 0;
}

static void ev_fnirs_shutdown(ev_app_t *app, ev_module_t *mod)
{
    (void)app;
    (void)mod;

#if !defined(FNIRS_EMBEDDED)
    ble_service_stop();
#endif
    fnirs_stream_exit();
    fnirs_ble_ipc_exit();
    fNIRS_exit();
    fusb_exit();
    fdatalog_exit();
    led_exit();

    WLOGI("fNIRS business modules stopped\r\n");
}

ev_module_t ev_fnirs_module = EV_MODULE_REGISTER("fnirs", ev_fnirs_init, ev_fnirs_shutdown);
