
/**
 * @brief functional near-infrared spectroscopy node.
 * 
 */

#ifndef _FNIRS_NODE_H_
#define _FNIRS_NODE_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "stdint.h"
#include "my_interface.h"
#include "fnirs_ble_ipc.h"

#define FNIRS_HUB_ID        0x00
#define FNODE_NID_MIN       1
#define FNODE_NID_MAX       12

#define FNODE_GAIN_DEF      0 //15
#define FNODE_GAIN_MAX      50 //35
#define FNODE_GAIN_STEP     1

#define FNODE_TIME_STAMP_SIZE 8 //采样时间戳，8字节单调毫秒值

//采样值最大值
#define FNODE_SPDATA_MAX        0x800000
//自动增益控制目标采样值
#define FNODE_SPDATA_TARGET     0x700000

#define FNODE_ACK(addr) (addr + 0x80)

#define FNODE_CHN(nid, sid)         (((nid) << 4) | (sid))
#define FNODE_CHN2NID(chn)          (((chn) >> 4) & 0x0f)
#define FNODE_CNN2SID(chn)          ((chn) & 0x0f)

#define FNODE_SRCID_MAX         6
typedef enum {
    FNODE_SRCA_735nm,
    FNODE_SRCA_850nm,
    FNODE_SRCB_735nm,
    FNODE_SRCB_850nm,
    FNODE_SRCC_735nm,
    FNODE_SRCC_850nm,
    FNODE_SRC_MAX,
}fnode_sid_t;  //source id

#define FNODE_DETID_MAX         4
typedef enum {
    FNODE_DET_CHA,
    FNODE_DET_CHB,
    FNODE_DET_CHC,
    FNODE_DET_CHD,
    FNODE_DET_MAX,
}fnode_did_t;  //detector id

#define FNODE_DATA_SIZE         (FNODE_DETID_MAX * FNODE_SRCID_MAX * 4)

typedef enum {
    //canfd frame
    FNODE_CANID_HTON = 0x600, //HUB TO NODE
    FNODE_CANID_NTOH = 0x200, //NODE TO HUB
}fnode_canid_t; //can id

typedef enum {
    FNODE_ADDR_SCAN = 0x00, // HTON, 扫描当前活跃的节点，ack
    FNODE_ADDR_RESET = 0x01, // HTON, 重置
    FNODE_ADDR_SETUP = 0x02, // HTON , 设置光源，no ack
    FNODE_ADDR_SAMPLE = 0x03, //HTON, 采样， no ack
    FNODE_ADDR_SETUP_AND_UPLOAD = 0x04, //亮LED并发送上次采样数据
    FNODE_ADDR_SAMPLE_AND_UPLOAD = 0x05, //与SETUP_AND_UPLOAD配对使用
    FNODE_ADDR_DATA = 0x10,  // HTON,NTOH, 获取采样数据, 数据上报
    FNODE_ADDR_SENSOR = 0x11, //NTOH, 上报传感器数据
	FNODE_ADDR_DATA_POLL = 0x12,  // HTON,NTOH, 获取采样数据, 数据点对点上报（HUB请求指定node上报数据）
	FNODE_ADDR_SENSOR_POLL = 0x13, //NTOH, 上报传感器数据,数据点对点上报
	FNODE_ADDR_DATA_LAST_AND_UPLOAD = 0x14, //请求上报最后一次采样数据
	FNODE_ADDR_SENSOR_LAST_AND_UPLOAD = 0x15, //请求上报最后一次传感器数据

	//0xA0~0xBF	OTA_xxx	OTA升级相关命令
	FNODE_CMD_OTA_OTA_START	= 0x20,	//OTA启动升级
	FNODE_CMD_OTA_OTA_CANCEL = 0x21,	//取消OTA升级
	FNODE_CMD_OTA_OTA_PACKAGE = 0x22,	//OTA固件包
	FNODE_CMD_OTA_OTA_FINISH = 0x23,	//完成OTA升级
	FNODE_CMD_OTA_OTA_INFO = 0x24,	//查询节点的固件信息

	//工厂模式
	FNODE_ADDR_SAMPLE_and_REPORT = 0x33, //HTON, 采样并上报命令， no ack
	FNODE_ADDR_SAMPLE_and_REPORT_DATA = 0x34, //HTON, 采样上报数据， no ack

	FNODE_ADDR_CALI_PD_and_REPORT = 0x35, //HTON, 校准PD偏置并上报命令， no ack
	FNODE_ADDR_CALI_PD_and_REPORT_DATA = 0x36, //HTON, 校准PD偏置采样上报数据， no ack

/* TODO: 用于产测模式，*/
    FNODE_ADDR_NID = 0xfe, // HTON,NTOH, 设置nid. 用于修改或者写入nid
    FNODE_ADDR_ERROR = 0xff,  // HTON,NTOH, 异常. hub查询node异常代码
}fnode_addr_t;

//OTA固件包最大容量
#define OTA_PACKAGE_MAX_LEN		(56)

//OTA错误
typedef enum {
	FNODE_OTA_ERROR_NO = 0, //正常
	FNODE_OTA_ERROR_PACKAGE_DUPLICATE = 1, //重复包【也是正常】

	FNODE_OTA_ERROR_FLASH_ERASE = 2, //擦除FLASH错误
	FNODE_OTA_ERROR_FLASH_WRITE = 3, //写FLASH错误

	FNODE_OTA_ERROR_PACKAGE_CRC = 4, //固件包校验错误
	FNODE_OTA_ERROR_FIRMWARE_CRC = 5, //固件检验错误

	FNODE_OTA_ERROR_OFFSET = 6, //固件包偏移错误
	FNODE_OTA_ERROR_PACKAGE_TOOLARGE = 7, //固件包太大

	FNODE_OTA_ERROR_FIRMWARE_TOOLARGE = 8, //固件太大

	FNODE_OTA_ERROR_FIRMWARE_RX_SIZE = 9, //接收到的固件大小不对


	FNODE_OTA_ERROR_FILE_NAME_SIZE = 20,
	FNODE_OTA_ERROR_FILE_CMD_CRC = 21,
	FNODE_OTA_ERROR_FILE_CREATE = 22,
	FNODE_OTA_ERROR_FILE_SAVEDATA = 23,
	
	FNODE_OTA_ERROR_FILE_PACKAGE_TOOLARGE = 24, //文件包大小太大
	FNODE_OTA_ERROR_FILE_PACKAGE_CRC = 25, //文件包校验错误

	FNODE_OTA_ERROR_FILE_RX_SIZE = 26,
	FNODE_OTA_ERROR_FILE_CRC_CALC = 27,
	FNODE_OTA_ERROR_FILE_CRC = 28,
	FNODE_OTA_ERROR_FILE_OPEN = 29,

	FNODE_OTA_ERROR_MAX,
}FNODE_OTA_ERROR_enmu_t;

//OTA状态机--状态
typedef enum {
	FNODE_OTA_STATUS_IDLE,


	FNODE_OTA_STATUS_FILE_START, //OTA BIN文件信息
	FNODE_OTA_STATUS_FILE_PACKAGE, //OTA BIN文件包传输
	FNODE_OTA_STATUS_FILE_FINISH, //OTA BIN文件完成传输


	
	FNODE_OTA_STATUS_START,

	FNODE_OTA_STATUS_PACKAGE,

	FNODE_OTA_STATUS_FINISH,

	FNODE_OTA_STATUS_GONE,

	FNODE_OTA_STATUS_ERROR,
}FNODE_OTA_STATUS_enmu_t;



//目标系统中存储文件名长度N -- N<=50，不包含最后的’\0’；相对于/opt/golgi/目录；
#define OTA_FILE_DOWNLAOD_MAX_LEN	(50)

#define OTA_FILE_DOWNLOAD_PACKAGE_MAX_LEN	(1024)
/* The MCU OTA staging partition is 50 KiB, including the 128-byte OTA header. */
#define OTA_IMAGE_MAX_SIZE			(50U * 1024U)

//OTA状态机--数据结构
typedef struct {
	FNODE_OTA_STATUS_enmu_t status; //OTA状态

	// === BIN文件下载 ===
	
	char bin_path[256];
	
	int downlaod_bin_fd; //目标系统中存储文件描述符
	
	char download_bin_filename[OTA_FILE_DOWNLAOD_MAX_LEN + 1]; //目标系统中存储文件名 ,/opt/golgi/目录下
	unsigned int download_bin_size; //待下载的BIN文件大小
	unsigned int download_bin_crc; //整个待下载的BIN文件CRC16校验值
	
	//当前下载文件的状态信息
	unsigned int download_bin_offset; //待下载的BIN文件的接收偏移

	// === 节点OTA ===
	int firmware_bin_fd; //OTA升级时使用的文件描述符
	
	unsigned int firmware_bin_size; //固件大小
	unsigned int firmware_bin_crc; //整个固件的CRC16校验值

	//当前信息
	unsigned int firmware_bin_offset; //当前发送的固件偏移
	unsigned int firmware_bin_package_len; //当前发送的固件包长度
	unsigned int firmware_bin_tx_len; //当前发送的固件长度

	unsigned int node_ota_package_handling; //下发固件中标志

	//OTA启动时参与节点的信息
	unsigned int node_ota_bitmap; //[NID16升级标志]..[NID9升级标志] , [NID8升级标志]..[NID1升级标志]
	unsigned int node_ota_current_bitmap; //当前参与OTA的节点统计bitmap
	
	unsigned int node_ack_ota_start_bitmap; //OTA START响应节点统计bitmap

	unsigned int node_ack_ota_package_bitmap; //OTA PACKAGE响应节点统计bitmap
	
	unsigned int node_ack_ota_finish_bitmap; //OTA FINISH响应节点统计bitmap
}ota_info_t;

//工厂测试模式状态机--状态
typedef enum {
	FNODE_FACTORY_STATUS_IDLE,

	FNODE_FACTORY_STATUS_ENTER, //工厂模式中，等待后续指令

	FNODE_FACTORY_STATUS_FUSB_MEASURE, //单通道3厘米发射与接收
	FNODE_FACTORY_STATUS_FUSB_CALI_PD_OFFSET, //校准PD OFFSET
	FNODE_FACTORY_STATUS_FUSB_LED_ON, //以互斥方式点亮某个灯

	FNODE_FACTORY_STATUS_FUSB_LED_ON_MEASURE_LED_OFF, //LED常亮，单通道3厘米发射与接收

	FNODE_FACTORY_STATUS_ERROR,
	
	FNODE_FACTORY_STATUS_FUSB_MEASURE_ALTERNATE, //单通道3厘米 735nm和850nm 交替发射与接收

}FNODE_FACTORY_STATUS_enmu_t;

//工厂测试模式状态机--数据结构
typedef struct {
	FNODE_FACTORY_STATUS_enmu_t status; //工厂测试模式状态

	uint8_t info_valid; //以下配置信息是否有效
	
	//发射探头信息 -- 单个灯
	uint32_t nid_tx; //[1, 12]
	uint32_t led_id; //[1, 3]
	uint32_t led_735_850; //[1 2]
	uint32_t tx_ma;

	uint32_t onoff_value;

	//接收探头信息 -- 单个灯
	uint32_t nid_rx; //[1, 12]
	uint32_t rx_pd; //[1, 4]

	//采样间隔和次数
	uint32_t sample_interval; //ms
	uint32_t sample_count;

	

	//PD校准偏置节点ID
	uint32_t cali_node_id;
	
}factory_test_t;

//OTA状态机
extern ota_info_t ota_info;

//工厂测试模式状态机
extern factory_test_t factory_test;


typedef enum {
    FNODE_GROUP0,
    FNODE_GROUP1,
    FNODE_GROUP_MAX,
}fnode_group_t;

typedef struct {
    uint8_t node_mcu_hw[4];
    uint8_t node_mcu_sw[4];
    uint8_t node_sensor_hw[4];
    uint32_t node_sn_code;
    uint8_t dock_uid[8];
    uint32_t border; //描述信息分界符
    uint64_t timestamp;
    uint32_t alive;//must at last position.
}fnode_desc_t __attribute__((aligned(4)));

// #define FNODE_SENSOR_DATA_SIZE  (sizeof(int16_t) * 8)
typedef struct  {
    int32_t temp;
    int16_t accel_x;
    int16_t accel_y;
    int16_t accel_z;
    int16_t gyro_x;
    int16_t gyro_y;
    int16_t gyro_z;
}fnode_sensor_t;

//16
#define FNODE_SENSOR_DATA_SIZE  sizeof(fnode_sensor_t)

typedef void *fnode_handler_t;

//fNIRS node
int32_t fnode_init(fnode_handler_t *fh, uint32_t group);

int32_t fnode_exit(fnode_handler_t fh);

// 获取当前node 的 description
int32_t fnode_desc(fnode_handler_t fh, fnode_desc_t desc[FNODE_NID_MAX]);

// 获取当前node 的 数量
uint32_t fnode_num(fnode_handler_t fh);

// 状态重置
int32_t fnode_reset(fnode_handler_t fh);

//设置发光LED序列
int32_t fnode_set_array(uint8_t *data, uint16_t size);

// 设置增益
/**
 * 高4bit，整数部分，低4bit，分数部分。取值范围（0x01，0x28）即最小增益 0.0625， 最大增益2.5. 
 * 1. 假设gain = 0x10， 即 增益 = 1.0 (默认值)
 * 2. 假设gain = 0x18, 即增益 = (1 + 8 / 16) = 1.5
 * 3. 假设gain = 0x01, 即增益 = (1 / 16) = 0.0625
 * 4. 假设gain = 0x00, 则恢复成默认值，增益 = 1.0
*/
int32_t fnode_s_gain(fnode_handler_t fh, uint8_t gain);

// 开始采样
int32_t fnode_sample_on(fnode_handler_t fh);
int32_t fnode_sample_off(fnode_handler_t fh);
uint32_t fnode_sample_ison(fnode_handler_t fh);

//开始OTA升级
int32_t fnode_ota_start(fnode_handler_t fh);
int32_t fnode_ota_cancel(fnode_handler_t fh);

//进入工厂测试模式
int32_t fnode_factory_test_mode_enter(fnode_handler_t fh, uint8_t flag);

//清除工厂模式暂存配置数据
void fnode_factory_test_mode_clear_var(void);

int32_t fnode_factory_test_mode_FUSB_MEASURE(fnode_handler_t fh, uint8_t* data, uint8_t data_len);
int32_t fnode_factory_test_mode_FUSB_MEASURE_ALTERNATE(fnode_handler_t fh, uint8_t* data, uint8_t data_len);

int32_t fnode_factory_test_mode_LED_ON_FUSB_MEASURE(fnode_handler_t fh, uint8_t* data, uint8_t data_len);

// 采样数据集合
//72个灯
#define FNODE_SAMPLE_SRC_MAX        (FNODE_SRCID_MAX * FNODE_NID_MAX)
//48个PD
#define FNODE_SAMPLE_DET_MAX        (FNODE_DETID_MAX * FNODE_NID_MAX)

/**
 * Iterate all alive-node channels (same order as fhub_append / Hangzhou.bin
 * sample matrix) and invoke cb(scaled int16 DC) per PD scalar.
 */
int32_t fnode_stream_push_channels(
    fnode_desc_t desc[FNODE_NID_MAX],
    uint32_t spdata[FNODE_SAMPLE_SRC_MAX][FNODE_SAMPLE_DET_MAX],
    const fnirs_ble_stream_ch_t *channels, uint8_t count,
    void (*cb)(int16_t v, void *ctx), void *ctx);

int32_t fnode_stream_foreach_channel(
    fnode_desc_t desc[FNODE_NID_MAX],
    uint32_t spdata[FNODE_SAMPLE_SRC_MAX][FNODE_SAMPLE_DET_MAX],
    void (*cb)(int16_t v, void *ctx), void *ctx);

/**
 * Pick one int16 scalar for BLE live stream from active LED sequence.
 * Returns DC level (raw/1000) in *out_dc; frame-to-frame delta in *out_ac.
 */
int32_t fnode_stream_pick_int16(fnode_handler_t fh,
    uint32_t spdata[FNODE_SAMPLE_SRC_MAX][FNODE_SAMPLE_DET_MAX],
    int16_t *out_dc, int16_t *out_ac);

#define FNODE_DATA_BLOCK_MAX        0x03
//(FNODE_DETID_MAX * 4)
#define FNODE_DATA_BLOCK_SIZE       16
#define FNODE_SAMPLE_DATA_OFFSET    8
/**
 * data 数据结构
|addr(1byte) | fnum(1byte) | snum(1byte)| padding(5byte) | data(4*4)    | data(4*4)    | data(4*4)    |
| :--------- |  :--------- |  :--------- |  :---------    |  :------     |  :--------   |  :------     |
| 0x10       |  0x00       |  3          |  0x00          | seq +sp data | seq +sp data | seq +sp data |

* 定义：canfd 帧作为整个采样帧的一个sector，最大包含三个block数据，每个block包含 FNODE_DET_MAX 个采样数据
1. 假设6个node，那么每个node 共传输: 6 * 6(SRC_MAX) / 3 = 12 sector.
2. 那么同理，6个node的情况下，共传输12 * 6 = 96 canfd frame
3. 需要计算传输开销，以及定义传输的时间点？
 */
//

#ifdef FNIRS_EV_IO
struct canfd_frame;

void fnode_ev_hub_service(fnode_handler_t fh);
int32_t fnode_ev_hub_process_frame(fnode_handler_t fh, struct canfd_frame *frame);
int32_t fnode_ev_can_fd(fnode_handler_t fh);
void fnode_ev_idle_service(fnode_handler_t fh);
void fnode_ev_kick_idle(fnode_handler_t fh);
void fnode_ev_led_scan_tick(fnode_handler_t fh);
void fnode_ev_ble_scan_pulse(fnode_handler_t fh);
int fnode_ev_sample_active(fnode_handler_t fh);
uint32_t fnode_ev_sample_next_delay_us(fnode_handler_t fh);
void fnode_ev_sample_tick(fnode_handler_t fh);
void fnode_ev_sample_begin(fnode_handler_t fh);
int fnode_ev_ota_active(fnode_handler_t fh);
void fnode_ev_ota_tick(fnode_handler_t fh);
void fnode_ev_ota_begin(fnode_handler_t fh);
int fnode_ev_factory_active(fnode_handler_t fh);
void fnode_ev_factory_tick(fnode_handler_t fh);
void fnode_ev_factory_kick(fnode_handler_t fh);
void fnode_ev_factory_stop(fnode_handler_t fh);
#endif

#ifdef __cplusplus
}
#endif

#endif
