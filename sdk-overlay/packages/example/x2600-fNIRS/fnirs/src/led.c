
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "unistd.h"
#include "pthread.h"
#include "errno.h"

//#include "wos_log.h"
//#include "wos_gpio.h"

#include "led.h"

#define LED1_R  WOS_GPIOB(31)
#define LED2_R  WOS_GPIOB(30)

#define LED1_G  WOS_GPIOB(29)
#define LED2_G  WOS_GPIOB(28)

#define LED2_B  WOS_GPIOB(27)
#define LED1_B  WOS_GPIOB(26)

typedef struct
{
    pthread_t tid;
    uint32_t run;
#ifdef FNIRS_EV_IO
    uint8_t ev_mode;
#endif
}led_obj_t;

led_obj_t *led_obj(void)
{
    static led_obj_t obj = {};

    return &obj;
}

#ifndef FNIRS_EV_IO
static void *led_task(void *args)
{
    led_obj_t *obj = (led_obj_t *)args;
	
    while(obj->run)
	{
        usleep(50000);
    }
	
    pthread_exit(NULL);
}
#endif




int32_t led_init(void)
{
    led_obj_t *obj = led_obj();
	
    wos_phy_gpio_init(LED1_R, WOS_GPIO_OUTPUT);
    wos_phy_gpio_init(LED1_G, WOS_GPIO_OUTPUT);
    wos_phy_gpio_init(LED1_B, WOS_GPIO_OUTPUT);
    wos_phy_gpio_init(LED2_R, WOS_GPIO_OUTPUT);
    wos_phy_gpio_init(LED2_G, WOS_GPIO_OUTPUT);
    wos_phy_gpio_init(LED2_B, WOS_GPIO_OUTPUT);

    wos_phy_gpio_set_output(LED1_R, WOS_GPIO_LOW);
    wos_phy_gpio_set_output(LED1_G, WOS_GPIO_LOW);
    wos_phy_gpio_set_output(LED1_B, WOS_GPIO_LOW);
    wos_phy_gpio_set_output(LED2_R, WOS_GPIO_LOW);
    wos_phy_gpio_set_output(LED2_G, WOS_GPIO_LOW);
    wos_phy_gpio_set_output(LED2_B, WOS_GPIO_LOW);

    obj->run = 1;

#ifdef FNIRS_EV_IO
    obj->ev_mode = 1;
    obj->tid = 0;
    WLOGI("led_init: event mode (no led_task thread)\r\n");
    return 0;
#else
    pthread_create(&obj->tid, NULL, led_task, obj);
    return 0;
#endif
}

int32_t led_exit(void)
{
    led_obj_t *obj = led_obj();
	
    obj->run = 0;

#ifdef FNIRS_EV_IO
    if (obj->ev_mode)
        goto gpio_off;
    pthread_join(obj->tid, NULL);
gpio_off:
#else
    pthread_join(obj->tid, NULL);
#endif
	
    wos_phy_gpio_set_output(LED1_R, WOS_GPIO_LOW);
    wos_phy_gpio_set_output(LED1_G, WOS_GPIO_LOW);
    wos_phy_gpio_set_output(LED1_B, WOS_GPIO_LOW);
    wos_phy_gpio_set_output(LED2_R, WOS_GPIO_LOW);
    wos_phy_gpio_set_output(LED2_G, WOS_GPIO_LOW);
    wos_phy_gpio_set_output(LED2_B, WOS_GPIO_LOW);
	
    return 0;
}


void led_set_evt(uint32_t led, uint32_t evt)
{

}

//LED1为扫描、采样指示灯

//扫描指示
void led_scan_nodes(void)
{
	static int flag  = 0;

	if(flag)
		wos_phy_gpio_set_output(LED2_G, WOS_GPIO_HIGH);
	else
	
	wos_phy_gpio_set_output(LED2_G, WOS_GPIO_LOW);

	flag = !flag;
}

//采样指示
void led_sample_nodes(int flag)
{
	wos_phy_gpio_set_output(LED2_G, WOS_GPIO_LOW);
	//wos_phy_gpio_set_output(LED2_R, WOS_GPIO_LOW);

	if(flag)
	{
		wos_phy_gpio_set_output(LED2_B, WOS_GPIO_HIGH);
	}
	else
	{
		wos_phy_gpio_set_output(LED2_B, WOS_GPIO_LOW);
	}

	flag = !flag;
}


//test指示灯
void led_test_nodes(int flag)
{
	//wos_phy_gpio_set_output(LED2_G, WOS_GPIO_LOW);
	//wos_phy_gpio_set_output(LED2_R, WOS_GPIO_LOW);

	if(flag)
	{
		wos_phy_gpio_set_output(LED2_R, WOS_GPIO_HIGH);
	}
	else
	{
		wos_phy_gpio_set_output(LED2_R, WOS_GPIO_LOW);
	}

	//flag = !flag;
}



//LED1为系统状态指示
void led_system(int status)
{
	switch(status)
	{
	case 0:
		wos_phy_gpio_set_output(LED1_R, WOS_GPIO_LOW);
	    wos_phy_gpio_set_output(LED1_G, WOS_GPIO_LOW);
	    wos_phy_gpio_set_output(LED1_B, WOS_GPIO_LOW);
		break;
	
	case 1: //运行灯
		wos_phy_gpio_set_output(LED1_G, WOS_GPIO_HIGH);
		break;

	case 2: //故障灯
		wos_phy_gpio_set_output(LED1_R, WOS_GPIO_HIGH);
		break;
	}
}

//关所有灯
void led_all_off(void)
{
	wos_phy_gpio_set_output(LED1_R, WOS_GPIO_LOW);
	wos_phy_gpio_set_output(LED1_G, WOS_GPIO_LOW);
	wos_phy_gpio_set_output(LED1_B, WOS_GPIO_LOW);
	wos_phy_gpio_set_output(LED2_R, WOS_GPIO_LOW);
	wos_phy_gpio_set_output(LED2_G, WOS_GPIO_LOW);
	wos_phy_gpio_set_output(LED2_B, WOS_GPIO_LOW);

}



