
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "stdint.h"
#include "unistd.h"
#include "signal.h"

//#include "wos_log.h"

//#include "dtu.h"
#include "fNIRS.h"
#include "fusb.h"
#include "fdatalog.h"
//#include "httpd.h"
//#include "udp.h"
#include "led.h"
#include "ble_service.h"
#include "fnirs_stream.h"
#include "fnirs_ble_ipc.h"

static void signal_handler(int signo)
{
    WLOGI("signal: %d\r\n", signo);

    ble_service_stop();

    fnirs_stream_exit();

    fnirs_ble_ipc_exit();
	
    // udp_exit();
    
    fNIRS_exit();
	
    // dtu_exit();
    
    fusb_exit();

    fdatalog_exit();
	
    exit(0);
}

static void custom_tzset(void)
{
    setenv("TZ", "CST-8", 1);
	
    tzset();
}

int32_t main(int32_t argc, const char *argv[])
{
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    custom_tzset();

    //WLOGW("main() -- 1\r\n");

    led_init();

    ble_service_init();
    if (ble_service_start() != 0)
        WLOGW("BLE file service start failed (fNIRS continues)\r\n");
	
    fusb_init();

    fdatalog_init(NULL);//数据记录进emmc线程初始化
	
    //dtu_init();
    
    fNIRS_init();

    fnirs_stream_init();

    fnirs_ble_ipc_init();
	
    //udp_init();
    
    //httpd_init();
    
    while(1)
	{
        sleep(2);

        ble_service_tick();

		led_system(1);

        //WLOGW("main() -- sleep\r\n");
    }
	
    //httpd_exit();
    
    //udp_exit();
    
    fNIRS_exit();
	
    //dtu_exit();
    
    fusb_exit();

    fdatalog_exit();
	
    return 0;
}

