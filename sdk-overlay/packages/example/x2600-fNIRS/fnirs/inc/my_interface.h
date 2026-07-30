
#ifndef _MY_INTERFACE_H_
#define _MY_INTERFACE_H_

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */

#include "stdint.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>   // 目录操作函数（opendir, readdir, closedir）
#include <errno.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/ioctl.h>

#include <sys/stat.h> // 文件属性判断（可选，用于更精确的文件类型检查）

#include "linux/can.h"
#include "linux/can/raw.h"

#define SW_VERSION	"FNIRS_HUB_SV1.5"
//V1.3
//1.亮灯时间仍为1.3毫秒；
//2.修改采样过程逻辑;
//V1.4
//1.亮灯时间为3毫秒；
//2.放开LED工作电流控制；

/*
1> format:
    \033[method;前景色;背景色m

      1）显示方式：0（默认值）、1（高亮）、22（非粗体）、4（下划线）、24（非下划线）、5（闪烁）、25（非闪烁）、7（反显）、27（非反显）
      2）前景色：30（黑色）、31（红色）、32（绿色）、 33（黄色）、34（蓝色）、35（洋红）、36（青色）、37（白色）
      3）背景色：40（黑色）、41（红色）、42（绿色）、 43（黄色）、44（蓝色）、45（洋红）、46（青色）、47（白色）
      
2) eg:
    printf( "\033[1;31;40m Red: hello world\r\n \033[0m" );
*/
#if 1
#define NORMAL_COLOR  "\033[0m"
#else
#define NORMAL_COLOR
#endif
#define RED_COLOR  "\033[1;31m"
#define GREEN_COLOR  "\033[1;32m"
#define YELLO_COLOR  "\033[1;33m"
#define BLUE_COLOR  "\033[1;34m"



#if 1
#define wprint(fmt, ...)  printf(fmt, ##__VA_ARGS__)
#else
#define wprint(fmt, ...)
#endif



#define WLOG(tag, fmt, ...) \
    do {\
        wprint(tag ": [%s] %s@%s#%d: " fmt, SW_VERSION, __FUNCTION__, __FILE__, __LINE__, ##__VA_ARGS__); \
    }while(0)

#define WLOGD(fmt, ...)  \
    do {\
        WLOG(GREEN_COLOR "[DEBUG]" NORMAL_COLOR, fmt, ##__VA_ARGS__); \
    }while(0)
    
#define WLOGI(fmt, ...)  \
    do {\
        WLOG(BLUE_COLOR "[INFO]" NORMAL_COLOR, fmt, ##__VA_ARGS__);  \
    }while(0)

#define WLOGW(fmt, ...)  \
    do {\
        WLOG(YELLO_COLOR "[WARN]" NORMAL_COLOR, fmt, ##__VA_ARGS__);  \
    }while(0)

#define WLOGE(fmt, ...) \
    do {\
        WLOG(RED_COLOR "[ERROR]" NORMAL_COLOR, fmt, ##__VA_ARGS__); \
    }while(0)
    
#define WLOGF(fmt, ...) \
    do {\
        WLOG(RED_COLOR "[FATAL]" NORMAL_COLOR, fmt, ##__VA_ARGS__);; \
    }while(0)


#define MYLOG(tag, fmt, ...) \
		do {\
			wprint("@@@ " tag " >> " fmt, ##__VA_ARGS__); \
		}while(0)
#define MYLOGI(fmt, ...)  \
		do {\
			WLOG(BLUE_COLOR "[INFO]" NORMAL_COLOR, fmt, ##__VA_ARGS__);  \
		}while(0)




/* unit: ms*/
typedef struct {
    uint64_t time;
}wos_timer_t;

/*
* @brief: 初始化timer, 设置时间为0
* @param [in] timer : timer指针
* @return : none
*/
void wos_timer_init(wos_timer_t *timer);

/*
* @brief: 开始timer, 设置时间当前时间
* @param [in] timer : timer指针
* @return : none
*/
void wos_timer_start(wos_timer_t *timer);

/*
* @brief: 判断当前时间是否超时, 如果timer->time 为0，也判定为超时
* @param [in] timer : timer指针
* @return : 0-未超时，other-超时
*/
uint32_t wos_timer_is_expired(wos_timer_t *timer);

/*
* @brief: 设置倒计时时间
* @param [in] timer : timer指针
* @return : none
*/
void wos_timer_countdown_ms(wos_timer_t *timer, uint64_t millisecond);

/*
* @brief: 强制设置时间超时
* @param [in] timer : timer指针
* @return : none
*/
void wos_timer_expire(wos_timer_t *timer);

/*
* @brief: 倒计时剩余时间
* @param [in] timer : timer指针
* @return : 剩余时间。如果已经超时，返回0
*/
uint64_t wos_timer_left(wos_timer_t *timer);

typedef void *wcan_t;


int32_t wcan_init(const char *canx, wcan_t *wcan);

int32_t wcan_exit(wcan_t wcan);

int32_t wcan_read(wcan_t wcan, struct can_frame *frame, uint32_t timeout);

int32_t wcan_write(wcan_t wcan, struct can_frame *frame);

int32_t wcanfd_read(wcan_t wcan, struct canfd_frame *frame, uint32_t timeout);

int32_t wcanfd_write(wcan_t wcan, struct canfd_frame *frame);

int32_t wcan_get_fd(wcan_t wcan);


/*
* @brief: 打开串口设备, wos_uart_fopen("/dev/ttyS0", WOS_UART_B115200, 8, 'N', 1);
        不同的平台系统串口设备符号可能不同，建议使用 wos_uart_open 接口. 
        x2600 串口设备列表: {"/dev/ttyS0", "/dev/ttyS1", "/dev/ttyS2", "/dev/ttyS3",
                           "/dev/ttyS4", "/dev/ttyS5", "/dev/ttyS6", "/dev/ttyS7"};
* @param [in] dev : 串口设备文件
* @param [in] baudrate : 波特率
* @param [in] nbits : 数据位
* @param [in] parity : 奇偶校验
* @param [in] stop : 停止位
* @return : 小于0 失败; 大于等于0返回设备句柄.
*/
int32_t wos_uart_fopen(const char *dev, uint32_t baudrate, uint8_t nbits, uint8_t parity, uint8_t stop);

/*
* @brief: 打开串口设备，wos_uart_open(WOS_UART0, WOS_UART_B115200, 8, 'N', 1);
        功能等同于 wos_uart_fopen . 
* @param [in] dev : 串口设备文件
* @param [in] baudrate : 波特率
* @param [in] nbits : 数据位
* @param [in] parity : 奇偶校验
* @param [in] stop : 停止位
* @return : 小于0 失败; 大于等于0返回设备句柄.
*/
int32_t wos_uart_open(uint8_t uartx, uint32_t baudrate, uint8_t nbits, uint8_t parity, uint8_t stop);

/*
* @brief : 关闭串口设备
* @param [in] fd : 已经打开的设备句柄
* @return : 0 if OK; otherwise fail.
*/
int32_t wos_uart_close(int32_t fd);

/*
* @brief : 串口读
* @param [in] fd : 已经打开的设备句柄
* @param [in] buf : 串口数据读缓冲内存空间
* @param [in] len : 串口数据读缓冲内存空间最大长度
* @param [in] tout_ms : 串口数据读帧超时时间
* @return : 失败返回-1. 成功返回读取的数据长度
*/
int32_t wos_uart_read(int32_t fd, uint8_t *buf, uint32_t len, uint32_t tout_ms);

/*
* @brief : 串口写
* @param [in] fd : 已经打开的设备句柄
* @param [in] buf : 串口写数据内存空间
* @param [in] len : 串口写数据长度
* @return : 失败返回-1. 成功返回写数据长度
*/
int32_t wos_uart_write(int32_t fd, const uint8_t *buf, uint32_t len);

typedef enum {
    WOS_GROUPA,
    WOS_GROUPB,
    WOS_GROUPC,
    WOS_GROUPD,
    WOS_GROUPE,
    WOS_GROUP_MAX,
}wos_gpio_groups_t;

#define WOS_GPIO_NONE               0xffff
#define WOS_GPIO_NUM(group, num)    (group * 32 + num)
#define WOS_GPIOA(num)              WOS_GPIO_NUM(WOS_GROUPA, num)
#define WOS_GPIOB(num)              WOS_GPIO_NUM(WOS_GROUPB, num)
#define WOS_GPIOC(num)              WOS_GPIO_NUM(WOS_GROUPC, num)
#define WOS_GPIOD(num)              WOS_GPIO_NUM(WOS_GROUPD, num)
#define WOS_GPIOE(num)              WOS_GPIO_NUM(WOS_GROUPE, num)


typedef enum {
    WOS_GPIO_INPUT,
    WOS_GPIO_OUTPUT,
}wos_gpio_dir_t;

typedef enum{
    WOS_GPIO_LOW = 0,
    WOS_GPIO_HIGH = 1,
    WOS_GPIO_UNKNOWN = 255,
}wos_gpio_state_t;

// 是否具备GPIO控制，由硬件主板决定，主板一般不完全支持所有控制GPIO控制
typedef enum {
    //gpio input
    WOS_GPIO_FIRE,  // 火警控制, input
    WOS_GPIO_PUSH,  // 室内按键, input
    WOS_GPIO_DOOR,  // 门磁感应, input
    WOS_GPIO_DISA,  // 防拆开关, input
    WOS_GPIO_RESET, // 复位按键, input
    WOS_GPIO_ALARM0, // 告警输入0
    WOS_GPIO_ALARM1, // 告警输入1

    //gpio output
    WOS_GPIO_RELAY = 0x80, // 继电器开关,output
    WOS_GPIO_SWITCH,// 板载开关, output
    WOS_GPIO_WLED,  // 补光灯控制:白光
    WOS_GPIO_IRLED, // 补光灯控制:红外
    WOS_GPIO_LCD,   // LCD控制:熄屏/亮屏
    WOS_GPIO_WIFI,  // WIFI控制:硬件上下电
    WOS_GPIO_LTE,   // 4G模块控制:硬件上下电
    WOS_GPIO_BLE,   // BLE模块控制:硬件上下电
    WOS_GPIO_MCU,   // MCU模块控制:硬件上下电

    WOS_GPIO_MAX,
}wos_logic_gpio_t;

/*
* @brief: 初始化gpio，配置gpio输入或输出。
* @param [in] gpio : 逻辑gpio，参考 wos_logic_gpio_t 定义
* @param [in] dir : 逻辑gpio的输入或输出, 参考 wos_gpio_dir_t 定义
* @return : 0 if OK; otherwise fail.
*/
int32_t wos_logic_gpio_init(uint32_t gpio, uint32_t dir);

/*
* @brief: 反初始化gpio, 重新对 gpio 的初始化配置之前需要反初始化. 请注意, 反初始化并不会改变 gpio 的历史状态.
* @param [in] gpio : 逻辑gpio，参考 wos_logic_gpio_t 定义
* @return : 0 if OK; otherwise fail.
*/
int32_t wos_logic_gpio_deinit(uint32_t gpio);

/*
* @brief: 获取逻辑gpio的输入状态，请注意，获取的gpio状态并非物理gpio的电平状态，而是输入gpio的逻辑电平，即根据逻辑意义赋予不同的值。
          比如WOS_GPIO_FIRE火警输入，如果返回WOS_GPIO_HIGH,表示火警生效，否则为火警没有生效。这个时候WOS_GPIO_HIGH并非表示
          这个gpio的物理电平，因为该gpio的实际电平会根据不同的原理图赋予不同的含义。
* @param [in] gpio :  逻辑gpio，参考 wos_logic_gpio_t 定义
* @return : -1失败，否则返回WOS_GPIO_LOW 或 WOS_GPIO_HIGH。
*/
int32_t wos_logic_gpio_get_input(uint32_t gpio);

/*
* @brief: 设置逻辑gpio的输出状态。请注意，设置的gpio状态并非物理gpio的电平状态，而是输出gpio的逻辑电平，即根据逻辑意义赋予不同的值。
          比如WOS_GPIO_RELAY 继电器输出，如果输出WOS_GPIO_HIGH,表示继电器从常闭=>断开，常开=>闭合；同理如果输出WOS_GPIO_LOW,
          则继电器恢复到初始状态，常闭=>闭合，常开=>断开。而这个gpio的物理电平，会根据不同的原理图赋予不同的含义。  
* @param [in] gpio :  逻辑gpio，参考 wos_logic_gpio_t 定义
* @param [in] state : 逻辑gpio的逻辑状态，参考 wos_logic_gpio_t 定义                 
* @return : 0 if OK; otherwise fail.
*/
int32_t wos_logic_gpio_set_output(uint32_t gpio, uint32_t state);

/*
* @brief: init the gpio, and config the gpio output or intput. Do not use this api until you know exactly what it do!
* @param [in] gpio :  wos physical gpio. It is different from the logic gpio. 
* @param [in] dir : input or output, referenct to  wos_gpio_dir_t
* @return : 0 if OK; otherwise fail.
*/
int32_t wos_phy_gpio_init(uint32_t gpio, uint32_t dir);

/*
* @brief: deinit the gpio. It wont to change the gpio state. Do not use this api until you know exactly what it do!
* @param [in] gpio :  wos physical gpio. It is different from the logic gpio. 
* @param [in] dir : input or output, referenct to  wos_gpio_dir_t
* @return : 0 if OK; otherwise fail.
*/
int32_t  wos_phy_gpio_deinit(uint32_t gpio);

/*
* @brief: Get the input state of the gpio. Do not use this api until you know exactly what it do!
* @param [in] gpio :  wos physical gpio. It is different from the logic gpio. 
* @return : 0 if OK; otherwise fail.
*/
int32_t wos_phy_gpio_get_input(uint32_t gpio);

/*
* @brief: Set the output state of the gpio. Do not use this api until you know exactly what it do!
* @param [in] gpio :  wos physical gpio. It is different from the logic gpio. 
* @param [in] state :  wos physical state. referenct to  wos_gpio_state_t
* @return : 0 if OK; otherwise fail.
*/
int32_t wos_phy_gpio_set_output(uint32_t gpio, uint32_t state);

/*
* @brief: read from system clock and write to hwclock.
* @return : 0 if OK; otherwise fail.
*/
int32_t wos_hwclock_write(void);

/*
* @brief: read from hwclock and write to system clock.
* @return : 0 if OK; otherwise fail.
*/
int32_t wos_hwclock_read(void);

/*
* @brief: set system clock with utc
* @param [in] utc : Universal Time Coordinated
* @return : 0 if OK; otherwise fail.
*/
int32_t wos_rtc_set(uint64_t utc);

/*
* @brief: 系统时间戳, 等同于gettimeofday
* @return : 返回系统毫秒时间.
*/
uint64_t wos_system_timestamp_ms(void);

/*
* @brief: 系统时间戳, 等同于clock_gettime(CLOCK_MONOTONIC, &ts)
* @return : 返回系统毫秒运行时间.
*/
uint64_t wos_system_clock_ms(void);

/*
* @brief: 系统时间戳, 等同于clock_gettime(CLOCK_MONOTONIC, &ts)
* @return : 返回系统微秒运行时间.
*/
uint64_t wos_system_clock_us(void);

/*
* @brief: 系统时间戳, 等同于clock_gettime(CLOCK_MONOTONIC, &ts)
* @return : 返回系统秒运行时间.
*/
uint64_t wos_system_clock(void);


// === 2025-11-14 maomao add ===


/*
* @brief: CRC16计算函数
* @return : 
*/
unsigned short my_crc16(char* const buf, int count);
unsigned short my_crc16_update(unsigned short crc_reg, char* const buf, int count);

/**
 * @brief 遍历目录下所有.bin文件
 * @param dir_path 目标目录路径（如"/tmp"）
 * @return 成功返回找到的.bin文件数量；失败返回-1
 */
int traverse_bin_files(const char *dir_path);

/**
 * @brief 遍历目录下是否存在.bin文件，且返回第一个.bin文件路径
 * @param dir_path 目标目录路径（如"/tmp"）
 * @param ota_bin_path 目标文件路径缓存
 * @param path_len 目标文件路径缓存大小
 * @return 成功返回找到的.bin文件数量；失败返回-1
 */
int traverse_ota_bin_files(const char *dir_path, char *ota_bin_path, int path_len);

/**
 * @brief 按56字节分包发送文件内容
 * @param bin_path .bin文件路径
 * @param serial_fd 串口文件描述符
 * @return 成功返回发送的总字节数；失败返回-1
 */
ssize_t send_bin_test(const char *bin_path);

/**
 * @brief 读取.bin文件并计算CRC16校验值
 * @param bin_path .bin文件路径
 * @param file_size 输出参数：文件大小（字节）
 * @return 成功返回CRC16校验值；失败返回0xFFFF（需结合file_size判断）
 */
int calculate_bin_crc16(const char *bin_path, off_t *file_size, unsigned short *crc_buf);




#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif
