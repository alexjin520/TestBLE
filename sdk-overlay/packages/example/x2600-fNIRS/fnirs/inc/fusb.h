
#ifndef _FUSB_H_
#define _FUSB_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "stdint.h"
#include <stdatomic.h>
#include "fnode.h"
#include "my_interface.h"

#define FUSB_EOT         0x04 /*结束符*/
#define FUSB_HOST2HUB    0x01 /*SID*/
#define FUSB_HUB2HOST    0x10 /*SID*/

typedef enum {

// HOST to HUB
    FUSB_RESET = 0x0000,
    FUSB_SCAN = 0x0001,
    FUSB_S_GAIN = 0x0002, //设置采样LED电流
    FUSB_SAMPLE_ON = 0x0003, //开始采样
    FUSB_SAMPLE_OFF = 0x0004, //结束采样
    FUSB_NTPDATE = 0x0005, //校时
    FUSB_SET_SAMPLE_ARRAY = 0x0006, //设置发光LED序列
    


//PC上位机与控制器工厂测试相关命令
	FUSB_FACTORY_MODE = 0x1001, //工厂测试模式进入/离开
	FUSB_MEASURE = 0x1002, //单通道3厘米发射与接收
	FUSB_CALI_PD_OFFSET = 0x1003, //校准PD OFFSET
    FUSB_MEASURE_ALTERNATE = 0x1004, //单通道3厘米 735和850nm交替发射与接收，huang

	FUSB_CHK_PD_UNIFORMITY = 0x1006, //单光源周边1厘米距离PD接收一致性???
	FUSB_LED_ON = 0x1007, //以互斥方式单独点亮某个LED

	FUSB_LED_ON_MEASURE = 0x1008, //LED常亮，单通道3厘米发射与接收

	FUSB_MEASURE_DATA = 0x1102, //单通道3厘米发射与接收 -- 数据
	FUSB_CALI_PD_DATA = 0x1103, //校准PD偏置 -- 数据
	FUSB_LED_ON_DATA = 0x1107, //以互斥方式单独点亮某个LED -- 数据 == 暂时未用
	
	FUSB_LED_ON_MEASURE_DATA = 0x1108, //LED常亮，单通道3厘米发射与接收

//PC上位机与控制器信息查询相关命令
	FUSB_HUB_SN = 0x1010, //查询控制器SN号
	FUSB_NODE_SN = 0x1011, //查询节点SN号

//PC上位机与控制器OTA相关命令
	FUSB_OTA_FILE_START = 0x2001, //OTA BIN文件信息
	FUSB_OTA_FILE_CANCEL = 0x2002, //取消OTA升级
	FUSB_OTA_FILE_PACKAGE = 0x2003, //OTA BIN文件包传输
	FUSB_OTA_FILE_FINISH = 0x2004, //OTA BIN文件完成传输
	FUSB_OTA_NODES_START = 0x2005, //通知控制器升级节点
	FUSB_OTA_PROGRESS = 0x2006, //升级节点进度通知
	FUSB_OTA_RESULT = 0x2007, //整体升级情况汇总


// HUB to HOST
    FUSB_SPDATA = 0x8000, //采样数据
    FUSB_HBEAT = 0x8001, // HUB 为了防止硬件故障，空闲时，3s一次发送的心跳包。

    FUSB_CHARGING = 0x8002,//hub上报 处于充电状态
    FUSB_DISCHARGING = 0x8003, //hub上报，未连接充电器，上报电压
    FUSB_LOWVOLTAGE = 0x8004, //低电压警告
    FUSB_POWEROFF = 0x8005, //hub上报，准备关机
    FUSB_SPDATA_WITH_TIME = 0x8006,//采样数据,带时间戳
    FUSB_BAT_FULL = 0x8007, //hub上报，电池充满

}fusb_addr_t;

typedef enum {
    FUSB_SUCCESS = 0,
    FUSB_FAILED = -1,
}fusb_errcode_t;

#define FUSB_PKT_BUFSIZE        100


#define FUSB_RWBUFF_SIZE        (0x10000 + 16)
#define FUSB_FRAME_TOUT         20

typedef struct {
    uint8_t nodes;
    uint8_t spdata[FNODE_TIME_STAMP_SIZE
        + FNODE_NID_MAX * FNODE_NID_MAX * FNODE_DATA_SIZE
		+ FNODE_NID_MAX * sizeof(fnode_sensor_t)]; //每个采样点4bytes
    uint32_t size; // data size
}fusb_pkt_t;

#define FUSB_FACTORY_PKT_BUF_COUNT        (100 * 100)

typedef struct {
	uint8_t buf[64];
	uint8_t size;
}fusb_factory_mode_pkg_t; //工厂模式测试通知消息缓存


typedef struct {
    pthread_t tid;
    pthread_mutex_t write_mtx;
    pthread_mutex_t tx_wait_mtx;
    pthread_cond_t tx_wait_cond;
	
    uint32_t run;
	
    wos_timer_t op_tm;
    wos_timer_t hb_tm;
    wos_timer_t vd_tm; //huang新增，检查电压定时器
    int32_t fd;
    uint8_t *rwbuff;
    uint8_t *txbuff;
    uint32_t rxlen;

    fusb_pkt_t *pkt;
    _Atomic uint32_t front;
    _Atomic uint32_t tail;

	//工厂模式测试通知消息环形缓存队列
	fusb_factory_mode_pkg_t *factory_mode_notify_queue;
    uint32_t q_read_index;
    uint32_t q_write_index;

    uint8_t ev_mode;
    
}fusb_obj_t;






/**
 *   帧结构
 *   SID(1byte)	ADDR(2byte)	LEN(2byte)	DATA(len byte)	EOT(2byte)
 *   源地址     协议地址     数据长度      数据             EOT，EOT
 */


int32_t fusb_init(void);
int32_t fusb_exit(void);
int32_t fusb_ringbuf_enqueue(uint8_t *spdata, uint32_t size, uint8_t nodes);

#ifdef FNIRS_EV_IO
void fusb_ev_attach_fd(int32_t fd);
void fusb_ev_detach_fd(void);
void fusb_ev_poll(void);
void fusb_ev_process_read(void);
void fusb_ev_drain_read(void);
#endif

//工厂模式下上报数据环形缓存接口——取数据
fusb_factory_mode_pkg_t* fusb_factory_ringbuf_dequeue(fusb_obj_t *obj);
//工厂模式下上报数据环形缓存接口——添加数据
int32_t fusb_factory_ringbuf_enqueue(uint8_t *data, uint32_t size);


#ifdef __cplusplus
}
#endif


#endif
