
#include "stdio.h"
#include "string.h"
#include "errno.h"
#include "stdlib.h"
#include "unistd.h"
#include "signal.h"
#include "pthread.h"
#include "linux/list.h"
#include "linux/can.h"

#include "fnode.h"
#include "fhub.h"
#include "fusb.h"
#include "fdatalog.h"
#include "msgq.h"
#include <stdbool.h>

#include <sched.h>
#include <stdatomic.h>
#include <time.h>

#ifndef MAX
#define MAX(a, b) ((a) < (b) ? (b) : (a))
#endif

#ifndef MIN
#define MIN(a, b) ((a) > (b) ? (b) : (a))
#endif

// 下发OTA START命令后等待时长
#define FNODE_OTA_START_WAIT_TIME 1000

// 下发OTA PACKAGE命令后等待时长
#define FNODE_OTA_PACKAGE_WAIT_TIME 1000
#define FNODE_OTA_PACKAGE_MAX_RETRY 1

// 下发OTA FINISH命令后等待时长
#define FNODE_OTA_FINISH_WAIT_TIME (1000 * 5)

// 最大采样LED数量
#define MAX_SAMPLE_NODES_LED 36 // 12(节点数)*3(LED数量)

// 帧间隔最小时间。用于接收帧数据。
// FIXME: 需要测试时间是否充裕
#define FNODE_FRAME_TOUT 50

#define FNODE_SCAN_PEROID 1000
#ifdef FNIRS_EV_IO
#define FNODE_SCAN_RESCAN_MS 100
#else
#define FNODE_SCAN_RESCAN_MS FNODE_SCAN_PEROID
#endif

#define FNODE_FRAME_HZ 4

#ifdef FNIRS_EV_IO
#define FNODE_RX_SNIFF_FIRST 20
#define FNODE_RX_SNIFF_EVERY 1000

static void fnode_ev_rx_sniff(const struct canfd_frame *frame, int accepted)
{
	static unsigned int rx_total;
	static unsigned int rx_ntoh;
	static unsigned int rx_other;
	unsigned int seq;
	uint8_t data0 = 0xff;

	if (!frame)
		return;

	seq = ++rx_total;
	if (accepted)
		rx_ntoh++;
	else
		rx_other++;

	if (frame->len > 0)
		data0 = frame->data[0];

	if (seq <= FNODE_RX_SNIFF_FIRST ||
	    (FNODE_RX_SNIFF_EVERY && seq % FNODE_RX_SNIFF_EVERY == 0)) {
		WLOGI("CAN-FD rx sniff #%u id=0x%03x base=0x%03x nid=%u len=%u data0=0x%02x accepted=%d ntoh=%u other=%u\r\n",
		      seq, frame->can_id, frame->can_id & 0x700,
		      frame->can_id & 0xff, frame->len, data0, accepted,
		      rx_ntoh, rx_other);
	}
}
#endif

static int fnode_can_payload_ok(const struct canfd_frame *frame, uint32_t need)
{
	if (!frame || need == 0 || need > sizeof(frame->data))
		return 0;

	return frame->len >= need;
}

static int fnode_can_slice_ok(const struct canfd_frame *frame, uint32_t off, uint32_t len)
{
	if (!frame || len == 0)
		return 0;
	if (off > sizeof(frame->data) || len > sizeof(frame->data) - off)
		return 0;

	return frame->len >= off + len;
}

// HUB工作状态
typedef enum
{
	FHUB_STATE_RESET,  // 空闲状态--扫描NODE
	FHUB_STATE_SAMPLE, // 采样状态--控制NODE进行发光和采样

	FHUB_STATE_OTA, // OTA升级状态

	FHUB_STATE_FACTORY_TEST, // 设备测试状态
} fhub_state_t;

// 线程间通信事件定义
typedef enum
{
	FNODE_EVT_SP_RESET, // 系统重置，重置

	// FNODE_EVT_SP_AUTOGAIN, // 自动增益

	FNODE_EVT_SP_START,	   // 采样开始
	FNODE_EVT_SP_SAVEDATA, // 表示已接收完一帧数据。
	FNODE_EVT_SP_STOP,	   // 采样结束

	FNODE_EVT_SP_SAVEDATA_POLL, // 表示已接收完一帧数据,在点对点（轮询）模式下

	FNODE_EVT_MAX,
} fnode_event_t;

typedef struct sample_node_info
{
	uint8_t node_id;
	uint8_t node_led_id;
	uint8_t power_735;
	uint8_t power_850;
} sample_node_info_t;

typedef struct
{
	pthread_t tid[2];
	uint32_t run;

	void *wcan;		// can 句柄
	uint32_t group; // node group id

	fnode_desc_t desc[FNODE_NID_MAX]; // node 信息
	uint32_t node_num;				  // node 数量 //空闲状态下扫描到的节点数量

	// 潜在问题：依据扫描到的节点数据进行计算的，如果采样过程中有节点掉线，导致无法达到数量
	uint32_t node_sp; // node 采样数量 //一个采样周期后应该收到的CAN FD采样数据包数

	uint32_t state;	   // hub 当前工作状态
	uint32_t reset;	   // reset flag;
	uint32_t gain_735; // gain 为0，自动增益，否则，手动增益。
	uint32_t gain_850; // gain 为0，自动增益，否则，手动增益。
	// uint32_t sample_gain; // 采样时的增益。如果手动增益，则设置为手动增益值，否则，自动增益到目标增益值

	// 下列3种状态是互斥的
	uint32_t sample;			// 采样标志
	uint32_t ota_start_flag;	// OTA节点标志
	uint32_t factory_mode_flag; // 工厂测试模式标志

	int32_t qid;	   // msgq id;
	wos_timer_t timer; // 操作定时器

	uint32_t ota_in_flag;  // 1 -- OTA过程中
	wos_timer_t ota_timer; // OTA定时器

	fhub_handler_t hub;
	uint8_t hub_fnum;

	//[以相同灯索引组织各个节点][以节点组织各个PD]，每个PD采样值为[发光节点_发光灯,采样24位值]
	uint32_t sample_data[FNODE_SAMPLE_SRC_MAX][FNODE_SAMPLE_DET_MAX];

	uint32_t spsector; // 采样数据数量统计

	// 12个节点的传感器数据
	fnode_sensor_t sens_data[FNODE_NID_MAX];

	uint8_t sens_num; // 传感器数据数量统计

	uint8_t poll_frame_cnt[FNODE_NID_MAX]; // 收到几帧，计数器

	uint32_t poll_frame_number_need; // 每个nid需要上传几帧数据

	// 新增：用于线程间同步
	pthread_mutex_t event_mtx;
	pthread_cond_t event_cond;
	volatile bool node_ack_ready[FNODE_NID_MAX]; // [nid-1] = true 表示该节点就绪

	// 新增，用于从主机设置发光序列
	sample_node_info_t sample_node_array_from_host[MAX_SAMPLE_NODES_LED]; // 记录从上位机传下来的发光序列
	uint8_t led_num_from_host;											  // 总共发光led的数量
	volatile bool array_from_host_is_used;								  // 标志位，是否启用设置发光序列功能，0不启用，1启用
	uint32_t sample_peroid_ms;											  // 采样周期ms

#ifdef FNIRS_EV_IO
	uint32_t ev_sp_phase;
	uint8_t ev_sp_fnum;
	uint32_t ev_sp_i;
	uint32_t ev_sp_j;
	uint8_t ev_sp_poll_nid;
	uint8_t ev_sp_poll_acks;
	uint8_t ev_sp_alive;
	uint32_t ev_sp_array_size;
	uint8_t ev_sp_gain_735;
	uint8_t ev_sp_gain_850;
	uint64_t ev_sp_deadline_us;
	uint64_t ev_sp_cycle_deadline_us;
	uint32_t ev_ota_phase;
	int ev_ota_fd;
	uint64_t ev_ota_deadline_ms;
	uint8_t ev_ota_package_retry;
	uint32_t ev_ft_phase;
	uint32_t ev_ft_substep;
	uint32_t ev_ft_index;
	uint8_t ev_ft_seq;
	uint64_t ev_ft_deadline_ms;
#endif

} fnode_t;

// 性能统计
typedef struct
{
	uint64_t sample_start_time;
	uint64_t sample_stop_time;

	uint64_t sample_getdata_req_time;
	uint64_t sample_getdata_ack_time;
} smaple_profile_t;

static smaple_profile_t my_smaple_profile;

sample_node_info_t sample_node_array2[] = {
	//{[1, 12], [1, 3]}
	{1, 3},
	{1, 2},
	{8, 2},
	{1, 1},

	{2, 1},
	{2, 3},

	{2, 2},
	{9, 2},
	{10, 1},

	{9, 3},
	{9, 1},
};

sample_node_info_t sample_node_array3[] = {
	//{[1, 12], [1, 3]}
	{7, 3},
	{7, 2},
	{8, 2},
	{7, 1},

	{12, 1},
	{12, 3},

	{12, 2},
	{9, 2},
	{10, 1},

	{9, 3},
	{9, 1},
};

/*
sample_node_info_t sample_node_array[] = {
	//{[1, 12], [1, 3]} 原始亮灯数组
	{4, 1, 0x1E, 0x1E},
	{2, 3, 0x1E, 0x1E},
	{8, 3, 0x1E, 0x1E},
	{11, 2, 0x1E, 0x1E},

	{2, 2, 0x1E, 0x1E},
	{8, 1, 0x1E, 0x1E},

	{3, 3, 0x1E, 0x1E},
	{9, 3, 0x1E, 0x1E},
	{10, 2, 0x1E, 0x1E},

	{9, 1, 0x1E, 0x1E},
	{9, 2, 0x1E, 0x1E},
};*/


sample_node_info_t sample_node_array[] = {
	//{[1, 12], [1, 3]} 原始亮灯数组

	{10, 2, 0x1E, 0x1E},

	{11, 1, 0x1E, 0x1E},
	{11, 2, 0x1E, 0x1E},

	{12, 2, 0x1E, 0x1E},
};



/*
sample_node_info_t sample_node_array[] = { //{[1, 12], [1, 3]} 20260527，20个灯测试，huang
	{11, 1},{10, 1},{11, 3},{10, 3},
	{11, 2},{10, 2},

	{8, 2},{9, 2},{8, 3},{9, 3},
	{8, 1},{9, 1},

	{2, 2},{4, 1},{3, 2},
	{2, 3},{4, 2},{3, 3},
	{2, 1},{4, 3},
};*/

sample_node_info_t sample_node_array_huang[] = {
	//{[1, 12], [1, 3]}
	{7, 1},
	{7, 2},
	{7, 3},

	{8, 1},
	{8, 2},
	{8, 3},

	{11, 2},
	{11, 1},

	{12, 3},
	{12, 2},
	{12, 1},

};

sample_node_info_t sample_node_array_20251201[] = {
	//{[1, 12], [1, 3]}
	{4, 3},
	{2, 2},
	{8, 2},
	{11, 1},

	{2, 1},
	{8, 3},

	{3, 2},
	{9, 2},
	{10, 1},

	{9, 3},
	{9, 1},
};

sample_node_info_t sample_node_array_20251220[] = {
	//{[1, 12], [1, 3]}
	{4, 1},
	{2, 3},
	{8, 3},
	{11, 2},

	{2, 2},
	{8, 1},

	{3, 3},
	{9, 3},
	{10, 2},

	{9, 1},
	{9, 2},
};

static const char *fnode_canx[FNODE_GROUP_MAX] = {"can0", "can1"};

// 单例获取函数：返回全局唯一的 fnode_t 实例指针
static fnode_t *fnode_obj(void)
{
	static fnode_t node = {0}; // 静态初始化为零（C标准保证线程安全初始化）
	return &node;
}

static int32_t fnode_can_init(fnode_t *node)
{
	int32_t rc = -1;
	rc = wcan_init(fnode_canx[node->group], &node->wcan);
	if (0 != rc)
	{
		WLOGW("wcan_init %s failed, %s\r\n", fnode_canx[node->group], strerror(errno));
		return -1;
	}

	return 0;
}

/**
 * @brief 通过 CANFD 总线向所有 NODE 广播一条命令帧。
 *
 * 此函数用于 HUB（主机）向连接在 CAN 总线上的所有从节点（NODE）发送控制命令，
 * 如采样、亮灯、上报数据等。采用广播方式（固定 CAN ID），所有 NODE 都会收到。
 *
 * @param[in] node  指向 fnode_t 结构体的指针，包含 CAN 设备句柄等上下文信息。
 * @param[in] data  要发送的原始数据缓冲区（命令内容）。
 * @param[in] len   要发送的数据长度（字节数），不能超过 CANFD 最大有效载荷。
 * @return          成功返回 0；失败返回 -1。
 */
static int32_t fnode_can_write(fnode_t *node, uint8_t *data, uint32_t len)
{
	// 定义一个 CANFD 帧结构体，用于封装要发送的数据。
	// struct canfd_frame 是 Linux 或 RTOS 中常见的 CANFD 帧格式（通常来自 <linux/can.h> 或 BSP 封装）。
	struct canfd_frame frame;

	// 返回值变量，初始化为 -1（表示默认失败）。
	int32_t rc = -1;

	// 静态计数器，用于调试时统计该函数被调用的次数（每调用一次 +1）。
	// 注意：多线程环境下此计数器非原子操作，仅用于单线程调试。
	static int count = 0;
	static int failure_count = 0;
	static int first_scan_logged = 0;

	// === 参数合法性校验 ===
	// 检查输入参数是否有效：
	// - node 不能为 NULL（必须提供有效的 HUB 上下文）
	// - data 不能为 NULL（必须提供有效数据）
	// - len 不能超过 CANFD 单帧最大数据长度（通常是 64 字节）
	if (NULL == node || NULL == data || len > CANFD_MAX_DLEN)
	{
		return -1; // 任一条件不满足，立即返回错误
	}

	// === 初始化 CANFD 帧 ===
	// 将整个 frame 结构体清零，避免残留脏数据影响发送。
	memset(&frame, 0, sizeof(struct canfd_frame));

	// === 设置 CAN 帧 ID ===
	// FNODE_CANID_HTON：可能是预定义的 CAN 标识符基础值（例如 0x180）
	// FNIRS_HUB_ID：HUB 的设备 ID（例如 0x01）
	// 两者按位或（|）组合成最终的 CAN ID。
	// 注：此处使用的是标准/扩展帧 ID，具体格式取决于硬件和协议定义。
	// 所有 NODE 被设计为监听这个固定的广播 ID。
	frame.can_id = FNODE_CANID_HTON | FNIRS_HUB_ID;

	// === 设置数据长度 ===
	// ⚠️ 关键点：虽然传入了实际数据长度 `len`，
	// 但这里强制将帧长度设为 CANFD 最大长度（64 字节）。
	// 原因注释已说明：“NODE 固件中限制一定要 64 字节”。
	// 这是一种“填充”策略：有效数据在前 `len` 字节，其余字节为 0（由 memset 保证）。
	frame.len = CANFD_MAX_DLEN; // 通常 #define CANFD_MAX_DLEN 64

	// === 设置 CANFD 特性标志 ===
	// CANFD_BRS (Bit Rate Switch)：启用 CANFD 的高速数据段。
	// 表示在数据段使用比仲裁段更高的波特率，提升传输效率。
	// 必须确保 HUB 和所有 NODE 的 CAN 控制器都支持并配置了 BRS。
	frame.flags = CANFD_BRS;

	// === 填充有效数据 ===
	// 将用户传入的 `data` 缓冲区的前 `len` 字节拷贝到 CAN 帧的数据区开头。
	// 剩余的 (64 - len) 字节保持为 0（由前面的 memset 保证）。
	memcpy(frame.data, data, len);

	// === 执行底层 CANFD 发送 ===
	// 调用 BSP 或驱动层提供的 API `wcanfd_write`，
	// 将构造好的 CANFD 帧通过 `node->wcan`（CAN 设备句柄）发送出去。
	rc = wcanfd_write(node->wcan, &frame);

	// === 调试计数（可选）===
	// 每次调用，静态计数器加 1。
	count++;

	// 可选的日志打印（当前被注释掉）：
	// WLOGI("fnode_can_write() -- rc:%d [%d]\r\n", rc, count);

	// === 检查发送结果 ===
	// 如果底层驱动返回非 0，说明发送失败（如总线错误、缓冲区满等）。
	if (0 != rc)
	{
		failure_count++;
		/* Avoid flooding the log when the interface remains unavailable. */
		if (failure_count == 1 || (failure_count % 50) == 0)
			WLOGE("CAN-FD tx failed: cmd=0x%02x rc=%d errno=%d (%s), failures=%d\r\n",
			      data[0], rc, errno, strerror(errno), failure_count);
		return -1; // 向上层报告失败
	}

	if (data[0] == FNODE_ADDR_SCAN && !first_scan_logged) {
		first_scan_logged = 1;
		WLOGI("CAN-FD first node scan transmitted (can_id=0x%x, len=%u)\r\n",
		      frame.can_id, frame.len);
	}

	// 发送成功，返回 0。
	return 0;
}

// 向指定 fnode 实例的消息队列发送一个事件（event + 附加值）
static int32_t fnode_snd_event(fnode_t *fnode, uint16_t event, uint16_t value)
{
	// 定义一个消息队列消息结构体
	msgq_t m;

	// 将 event（高16位）和 value（低16位）打包成一个 32 位整数
	// 例如：event=5, value=3 → data = 0x00050003
	uint32_t data = ((event << 16) | value);

	// 参数校验：fnode 指针不能为 NULL，且 event 必须在合法范围内（小于 FNODE_EVT_MAX）
	if (NULL == fnode || event >= FNODE_EVT_MAX)
	{
		return -1; // 返回错误
	}

	// 设置消息类型为普通消息（MSGQ_MTYPE_NORMAL），用于 msgq_rcv 匹配
	m.mtype = MSGQ_MTYPE_NORMAL;

	// 记录数据大小（虽然实际未通过指针传数据，但用于接收端校验）
	m.datasz = sizeof(data); // 即 4 字节

	// ⚠️【关键点】将整数 data 强制转换为 void* 指针，作为消息的“数据”字段
	// 注意：这是 hack 用法，在 64 位系统上若 data 高位非零可能出问题，
	// 但因 data 是 uint32_t（≤0xFFFFFFFF），在大多数 Linux 64 位系统中仍可工作（地址空间低 4GB 可寻址）
	m.data = (void *)data;

	// 调用底层消息队列发送接口，将消息发到 fnode->qid 队列
	return msgq_snd(fnode->qid, &m);
}

// 从指定 fnode 实例的消息队列接收一个事件，并解析出 event 和 value
static int32_t fnode_rcv_event(fnode_t *fnode, uint16_t *event, uint16_t *value)
{
	// 定义接收用的消息结构体
	msgq_t m;

	// 用于存储接收到的 32 位打包数据
	uint32_t data;

	// 返回码，默认为失败
	int32_t rc = -1;

	// 参数校验：fnode 和 event 指针不能为空（value 可为 NULL，表示不关心）
	if (NULL == fnode || NULL == event)
	{
		return -1;
	}

	// 从消息队列 fnode->qid 中接收一条 MSGQ_MTYPE_NORMAL 类型的消息
	// 阻塞等待直到有消息到达
	rc = msgq_rcv(fnode->qid, MSGQ_MTYPE_NORMAL, &m);
	if (0 != rc)
	{
		return -1; // 接收失败（如中断、队列删除等）
	}

	// 校验消息数据大小是否为 4 字节（即一个 uint32_t）
	// 用于防止不同版本或错误消息混入
	if (m.datasz != sizeof(uint32_t))
	{
		WLOGW("Invalid msg\r\n"); // 打印警告日志
		return -1;
	}

	// ⚠️【关键点】将 m.data（void*）强制转回 uint32_t
	// 假设发送端是用 (void*)data 发的，这里还原
	data = (uint32_t)m.data;

	// 从高16位提取事件类型
	*event = (data >> 16);

	// 如果调用者提供了 value 指针，则从低16位提取附加值
	if (NULL != value)
	{
		*value = (data & 0xffff); // 掩码确保只取低16位
	}

	// 成功返回
	return 0;
}

// OTA状态机
ota_info_t ota_info =
	{
		.downlaod_bin_fd = -1,
		.firmware_bin_fd = -1,
};

// 工厂测试模式状态机
factory_test_t factory_test;

/********************************************************************************************
 *  OTA初始化处理
 *
 ********************************************************************************************/
int fnode_ota_init(fnode_t *node)
{
	if (node->state == FHUB_STATE_RESET)
	{
		//===初始化相关状态和数据 ===
		// 1.获取文件信息，遍历/opt/golgi/***.bin

		// 2.

		return 0;
	}

	return -1;
}

#define OTA_HANDLER_START

/********************************************************************************************
 *  OTA响应命令处理
 *	根据当前OTA状态分类处理
 ********************************************************************************************/
static void fnode_CANFD_frame_ota_handler(fnode_t *node, struct canfd_frame *frame, uint8_t fnum)
{
	uint8_t nid = 0;
	uint8_t cmd = 0;

	// 节点ID
	nid = frame->can_id & 0xff;
	if (nid < FNODE_NID_MIN || nid > FNODE_NID_MAX)
	{
		WLOGW("OTA -- ignore NID = %d\r\n", nid);

		return;
	}

	// 命令字
	if (!fnode_can_payload_ok(frame, 1))
		return;

	cmd = frame->data[0];

	switch (cmd)
	{
	case FNODE_ACK(FNODE_CMD_OTA_OTA_START): // 0x20, OTA启动升级
		if (ota_info.status == FNODE_OTA_STATUS_START)
		{
			unsigned int firmware_bin_size;
			unsigned int firmware_bin_crc;

			if (!fnode_can_payload_ok(frame, 11))
				break;

			// 固件大小
			firmware_bin_size = ((unsigned int)frame->data[1] << 24) +
				((unsigned int)frame->data[2] << 16) +
				((unsigned int)frame->data[3] << 8) +
				(unsigned int)frame->data[4];

			// 固件的CRC16校验值
			firmware_bin_crc = ((unsigned int)frame->data[5] << 24) +
				((unsigned int)frame->data[6] << 16) +
				((unsigned int)frame->data[7] << 8) +
				(unsigned int)frame->data[8];

			if (ota_info.node_ota_bitmap & (1 << (nid - 1)))
			{
				if (firmware_bin_size == ota_info.firmware_bin_size &&
				    (uint16_t)firmware_bin_crc ==
					(uint16_t)ota_info.firmware_bin_crc &&
				    frame->data[9] == 1 &&
				    (frame->data[10] == 1 ||
				     frame->data[10] == FNODE_OTA_ERROR_NO)) {
					printf("[OTA START] ACK NID = %d ready\r\n", nid);
					if (frame->data[10] == FNODE_OTA_ERROR_NO)
						WLOGI("[OTA START] NID=%u accepted legacy erase result=0\r\n",
						      nid);
					ota_info.node_ack_ota_start_bitmap |=
						(1 << (nid - 1));
				} else {
					WLOGW("[OTA START] NID=%u rejected size=%u crc=0x%04x participate=%u erase=%u\r\n",
					      nid, firmware_bin_size,
					      (uint16_t)firmware_bin_crc,
					      frame->data[9], frame->data[10]);
				}
			}
		}
		break;

	case FNODE_ACK(FNODE_CMD_OTA_OTA_CANCEL): // 0x21,	取消OTA升级
		break;

	case FNODE_ACK(FNODE_CMD_OTA_OTA_PACKAGE): // 0x22, OTA固件包
		if (ota_info.status == FNODE_OTA_STATUS_PACKAGE)
		{
			uint32_t ack_offset;
			uint32_t expected_offset;
			uint8_t ack_len;
			uint8_t ack_result;

			if (!fnode_can_payload_ok(frame, 7))
				break;

			ack_offset = ((uint32_t)frame->data[1] << 24) |
				((uint32_t)frame->data[2] << 16) |
				((uint32_t)frame->data[3] << 8) |
				(uint32_t)frame->data[4];
			ack_len = frame->data[5];
			ack_result = frame->data[6];
			expected_offset = ota_info.firmware_bin_offset -
				ota_info.firmware_bin_package_len;

			if ((ota_info.node_ota_current_bitmap &
			     (1 << (nid - 1))) &&
			    ack_offset == expected_offset &&
			    ack_len == ota_info.firmware_bin_package_len &&
			    (ack_result == FNODE_OTA_ERROR_NO ||
			     ack_result == FNODE_OTA_ERROR_PACKAGE_DUPLICATE)) {
				ota_info.node_ack_ota_package_bitmap |=
					(1 << (nid - 1));
			} else if (ota_info.node_ota_current_bitmap &
				   (1 << (nid - 1))) {
				WLOGW("[OTA PACKAGE] NID=%u bad ACK offset=%u/%u len=%u/%u result=%u\r\n",
				      nid, ack_offset, expected_offset, ack_len,
				      ota_info.firmware_bin_package_len, ack_result);
			}
		}
		break;

	case FNODE_ACK(FNODE_CMD_OTA_OTA_FINISH): // 0x23,	完成OTA升级
		if (!fnode_can_payload_ok(frame, 2))
			break;

		printf("[OTA FINISH] ACK NID = %d frame: [%d]\r\n", nid, frame->data[1]);
		if (ota_info.status == FNODE_OTA_STATUS_FINISH)
		{
			if (frame->data[1] == 0 &&
			    (ota_info.node_ota_current_bitmap & (1 << (nid - 1))))
			{
				printf("[OTA FINISH] ACK NID = %d\r\n", nid);

				// 更新收到OTA FINISH响应的节点信息
				ota_info.node_ack_ota_finish_bitmap |= (1 << (nid - 1));

			}
		}
		break;

	case FNODE_ACK(FNODE_CMD_OTA_OTA_INFO): // 0x24,	查询节点的固件信息
		break;

	default:
		break;
	}
}

/********************************************************************************************
 *  工厂测试响应命令处理
 *	根据当前命令值分类处理
 ********************************************************************************************/
static void fnode_CANFD_frame_FACTORY_handler(fnode_t *node, struct canfd_frame *frame, uint8_t fnum)
{
	uint8_t nid = 0;
	uint8_t cmd = 0;

	// 节点ID
	nid = frame->can_id & 0xff;
	if (nid < FNODE_NID_MIN || nid > FNODE_NID_MAX)
	{
		WLOGW("FACTORY -- ignore NID = %d\r\n", nid);

		return;
	}

	// 命令字
	if (!fnode_can_payload_ok(frame, 1))
		return;

	cmd = frame->data[0];

	// WLOGW("FACTORY -- cmd = %X\r\n", cmd);

	switch (cmd)
	{
	case FNODE_ACK(FNODE_ADDR_SAMPLE_and_REPORT_DATA): // 采样并上报命令的数据
	{
		volatile uint32_t index = 0;
		volatile uint32_t sample_value;

		if (!fnode_can_payload_ok(frame, 14) ||
		    !fnode_can_slice_ok(frame, 1, 13))
			break;

		volatile uint8_t fnum = frame->data[1];
		volatile uint8_t nid_tx = frame->data[2];
		volatile uint8_t led_id = frame->data[3];
		volatile uint8_t nid_rx = frame->data[4];
		volatile uint8_t pd_id = frame->data[5];

		index = ((uint32_t)frame->data[6] << 24) + ((uint32_t)frame->data[6 + 1] << 16) + ((uint32_t)frame->data[6 + 2] << 8) + ((uint32_t)frame->data[6 + 3]);

		sample_value = ((uint32_t)frame->data[10] << 24) + ((uint32_t)frame->data[10 + 1] << 16) + ((uint32_t)frame->data[10 + 2] << 8) + ((uint32_t)frame->data[10 + 3]);
#if 1
		printf("%d <%d> [%d, %d, %d, %d] {%d}\r\n", fnum, index,
			   nid_tx, led_id, nid_rx, pd_id,
			   sample_value);
#endif
		uint8_t canfd_buf[64] = {0};

		if (FNODE_FACTORY_STATUS_FUSB_LED_ON_MEASURE_LED_OFF == factory_test.status) // 常亮测试时，不同命令字
		{
			canfd_buf[0] = (FUSB_LED_ON_MEASURE_DATA >> 8) & 0xFF;
			canfd_buf[1] = FUSB_LED_ON_MEASURE_DATA & 0xFF;
		}
		else
		{
			canfd_buf[0] = (FUSB_MEASURE_DATA >> 8) & 0xFF;
			canfd_buf[1] = FUSB_MEASURE_DATA & 0xFF;
		}

		memcpy(&canfd_buf[2], &frame->data[1], 13);
		// 工厂模式测试命令结果,采用透传数据方式
		fusb_factory_ringbuf_enqueue(canfd_buf, 2 + 13);
	}
	break;

	case FNODE_ACK(FNODE_ADDR_CALI_PD_and_REPORT_DATA): // 校准PD偏置采样上报的数据
	{
		int32_t ch0_cal_value;
		int32_t ch1_cal_value;
		int32_t ch2_cal_value;
		int32_t ch3_cal_value;

		if (!fnode_can_payload_ok(frame, 20) ||
		    !fnode_can_slice_ok(frame, 1, 19))
			break;

		ch0_cal_value = ((uint32_t)frame->data[4] << 24) + ((uint32_t)frame->data[4 + 1] << 16) + ((uint32_t)frame->data[4 + 2] << 8) + ((uint32_t)frame->data[4 + 3]);

		ch1_cal_value = ((uint32_t)frame->data[8] << 24) + ((uint32_t)frame->data[8 + 1] << 16) + ((uint32_t)frame->data[8 + 2] << 8) + ((uint32_t)frame->data[8 + 3]);

		ch2_cal_value = ((uint32_t)frame->data[12] << 24) + ((uint32_t)frame->data[12 + 1] << 16) + ((uint32_t)frame->data[12 + 2] << 8) + ((uint32_t)frame->data[12 + 3]);

		ch3_cal_value = ((uint32_t)frame->data[16] << 24) + ((uint32_t)frame->data[16 + 1] << 16) + ((uint32_t)frame->data[16 + 2] << 8) + ((uint32_t)frame->data[16 + 3]);
#if 1
		printf("CALI PD -- [%d, %d] -- [%d, %d, %d, %d]\r\n",
			   frame->data[2], frame->data[3],
			   ch0_cal_value, ch1_cal_value, ch2_cal_value, ch3_cal_value);
#endif
		uint8_t canfd_buf[64] = {0};

		canfd_buf[0] = (FUSB_CALI_PD_DATA >> 8) & 0xFF;
		canfd_buf[1] = FUSB_CALI_PD_DATA & 0xFF;

		memcpy(&canfd_buf[2], &frame->data[1], 19);
		// 工厂模式测试命令结果,采用透传数据方式
		fusb_factory_ringbuf_enqueue(canfd_buf, 2 + 19);
	}
	break;
	}
}

/*
 * 0x84/0x94 ACK 的前4字节为应答信息，后16字节正好包含同一发光
 * 通道下一个节点的4路PD数据。每个4字节PD值的最后一字节都应是同一 chn。
 */
static int fnode_store_upload_ack(fnode_t *node,
				  const struct canfd_frame *frame,
				  uint8_t nid)
{
	uint8_t chn;
	uint8_t src_nid;
	uint8_t src_sid;
	uint8_t src_idx;
	uint8_t det_idx;
	uint8_t i;

	if (!node || nid < FNODE_NID_MIN || nid > FNODE_NID_MAX ||
	    !fnode_can_slice_ok(frame, 4, FNODE_DATA_BLOCK_SIZE))
		return -1;

	chn = frame->data[7];
	for (i = 1; i < FNODE_DETID_MAX; i++) {
		if (frame->data[4 + i * 4 + 3] != chn) {
			WLOGW("Upload ACK channel mismatch: nid=%u pd=%u chn=0x%02x/0x%02x\r\n",
			      nid, i, frame->data[4 + i * 4 + 3], chn);
			return -1;
		}
	}

	src_nid = FNODE_CHN2NID(chn);
	src_sid = FNODE_CNN2SID(chn);
	if (src_sid >= FNODE_SRC_MAX ||
	    src_nid < FNODE_NID_MIN || src_nid > FNODE_NID_MAX) {
		WLOGE("Invalid upload ACK chn = %#x\r\n", chn);
		return -1;
	}

	src_idx = (src_nid - 1) + (src_sid * FNODE_NID_MAX);
	det_idx = (nid - 1) * FNODE_DETID_MAX;
	memcpy(&node->sample_data[src_idx][det_idx],
	       &frame->data[4], FNODE_DATA_BLOCK_SIZE);
	node->spsector++;

	return 0;
}

/********************************************************************************************
 *  接收到的CAN帧处理
 *	根据当前状态分类处理：
 *	1.空闲状态--扫描NODE
 *		处理扫描ACK;
 *	2.采样状态--控制NODE进行发光和采样
 *		处理传感器数据；
 *		处理采样数据；
 ********************************************************************************************/
static void fnode_CANFD_frame_handler(fnode_t *node, struct canfd_frame *frame, uint8_t fnum)
{
	static uint32_t scan_ack_seen_mask;
	uint8_t nid = 0;
	uint8_t cmd = 0;

	nid = frame->can_id & 0xff;
	if (nid < FNODE_NID_MIN || nid > FNODE_NID_MAX)
	{
		// ignore;
		WLOGW("ignore NID = %d\r\n", nid);

		return;
	}

	if (!fnode_can_payload_ok(frame, 1))
		return;

	cmd = frame->data[0];

	/* ========== NID=0x0C 时打印完整 CAN-FD 数据段 (用户态版本) ========== */
	/*if (nid == 0x0C) {
		printf("[CANFD] NID=0x%02X CMD=0x%02X FNUM=0x%02X LEN=%u DATA:",
			   nid, cmd, fnum, frame->len);

		// 用户态不支持 %*ph，必须手动循环打印
		for (uint8_t i = 0; i < frame->len && i < 64; i++) {
			if (i > 0 && (i % 16) == 0)
				printf("\n                            ");
			printf(" %02X", frame->data[i]);
		}
		printf("\n");
		fflush(stdout); // 强制刷新，避免日志延迟输出
	}*/
	/* =================================================================== */

	// WLOGI("frame from nid: %d, cmd: %#x\r\n", nid, cmd);
	switch (node->state) // 根据当前状态分类处理
	{
	case FHUB_STATE_RESET: // 空闲状态--扫描NODE
		switch (cmd)
		{
		case FNODE_ACK(FNODE_ADDR_SCAN):
			// printf("SCAN -- %d\r\n", nid);
			memset(&node->desc[nid - 1], 0,
			       sizeof(node->desc[nid - 1]));
			if (fnode_can_slice_ok(frame, 1,
					       offsetof(fnode_desc_t, border))) {
				memcpy(&node->desc[nid - 1], &frame->data[1],
				       offsetof(fnode_desc_t, border));
			} else if (fnode_can_slice_ok(frame, 1, 16)) {
				/*
				 * 兼容旧节点描述：
				 * [mcu_hw:4][mcu_sw:4][dock_uid:8]。
				 */
				memcpy(node->desc[nid - 1].node_mcu_hw,
				       &frame->data[1], 4);
				memcpy(node->desc[nid - 1].node_mcu_sw,
				       &frame->data[5], 4);
				memcpy(node->desc[nid - 1].dock_uid,
				       &frame->data[9], 8);
				WLOGI("CAN-FD legacy scan ack accepted: nid=%u len=%u\r\n",
				      nid, frame->len);
			} else {
				WLOGW("CAN-FD scan ack too short: nid=%u len=%u need=17/25\r\n",
				      nid, frame->len);
				break;
			}

			node->desc[nid - 1].timestamp = wos_system_clock_ms();
			node->desc[nid - 1].alive = 1;
			if (!(scan_ack_seen_mask & (1U << (nid - 1)))) {
				scan_ack_seen_mask |= 1U << (nid - 1);
				WLOGI("CAN-FD scan ack accepted: nid=%u len=%u\r\n", nid, frame->len);
			}
			break;

		default:
			break;
		}
		break;

	case FHUB_STATE_SAMPLE: // 采样状态--控制NODE进行发光和采样
		switch (cmd)
		{
		case FNODE_ACK(FNODE_ADDR_SENSOR): // 传感器数据
			// FIXME: 传感器相关数据
			if (!fnode_can_payload_ok(frame, 2))
				break;

			if (fnum != frame->data[1])
			{
				// ignore. 无效帧序号。造成的原因是: 新的
				WLOGE("invalid sensor data fnum: %d/%d\r\n", frame->data[1], fnum);

				break;
			}

			if (!fnode_can_slice_ok(frame, 2, FNODE_SENSOR_DATA_SIZE))
				break;

			memcpy(&node->sens_data[nid - 1], &frame->data[2], FNODE_SENSOR_DATA_SIZE);
			node->sens_num += 1;
			break;

		case FNODE_ACK(FNODE_ADDR_SENSOR_POLL): // 传感器数据

			if (!fnode_can_slice_ok(frame, 2, FNODE_SENSOR_DATA_SIZE))
				break;

			memcpy(&node->sens_data[nid - 1], &frame->data[2], FNODE_SENSOR_DATA_SIZE);
			node->sens_num += 1;
			break;

		case FNODE_ACK(FNODE_ADDR_DATA): // 采样数据
			// printf("{%d}\r\n", nid);
			// FIXME: 存储数据
			if (!fnode_can_payload_ok(frame, 3))
				break;

			if (fnum != frame->data[1])
			{
				// ignore. 无效帧序号。造成的原因可能是新的采样流程
				WLOGE("invalid spdata fnum: %d/%d\r\n", frame->data[1], fnum);

				break;
			}

			if (frame->data[2] > FNODE_DATA_BLOCK_MAX) // 包内块数
			{
				// error
				WLOGW("invalid fnum [%d]\r\n", frame->data[2]);

				break;
			}

			if (!fnode_can_payload_ok(frame,
			    FNODE_SAMPLE_DATA_OFFSET +
			    (uint32_t)frame->data[2] * FNODE_DATA_BLOCK_SIZE))
				break;

			my_smaple_profile.sample_getdata_ack_time = wos_system_clock_ms();

			for (uint8_t i = 0; i < frame->data[2]; i++)
			{
				uint8_t chn = frame->data[FNODE_SAMPLE_DATA_OFFSET + i * FNODE_DATA_BLOCK_SIZE + 3];
				// fprintf(stderr, "chn: %#x\r\n", chn);

				uint8_t src_nid = FNODE_CHN2NID(chn); // 发光的节点号 [1, 12]
				uint8_t src_sid = FNODE_CNN2SID(chn); // 发光节点的灯号 [0, 5]
				// 多余的校验，确保正确无误
				if (src_sid >= FNODE_SRC_MAX || src_nid < FNODE_NID_MIN || src_nid > FNODE_NID_MAX)
				{
					WLOGE("Invalid chn = %#x\r\n", chn);

					continue;
				}

				// 存储到相应的位置
				uint8_t src_idx = (src_nid - 1) + (src_sid * FNODE_NID_MAX); // 灯位置
				uint8_t det_idx = (nid - 1) * FNODE_DETID_MAX;				 // 发送数据的节点对应的采样数据存储位置
				memcpy(&node->sample_data[src_idx][det_idx],
					   &frame->data[FNODE_SAMPLE_DATA_OFFSET + i * FNODE_DATA_BLOCK_SIZE],
					   FNODE_DATA_BLOCK_SIZE);

				/*
				 // ⭐ 定点观测：仅当 src_nid=8, src_sid=0, det_idx=12 时打印16字节
				if (src_nid == 8 && src_sid == 0 && nid == 12) {
					const uint8_t *bp = (const uint8_t *)&node->sample_data[src_idx][det_idx];
					WLOGI("RX_DUMP: [%02X %02X %02X %02X %02X %02X %02X %02X "
						  "%02X %02X %02X %02X %02X %02X %02X %02X]\r\n",
						  bp[0],  bp[1],  bp[2],  bp[3],
						  bp[4],  bp[5],  bp[6],  bp[7],
						  bp[8],  bp[9],  bp[10], bp[11],
						  bp[12], bp[13], bp[14], bp[15]);
				}*/
			}

#if 0
				printf("*Node(%d) ack %llums\r\n",
					nid,
					my_smaple_profile.sample_getdata_ack_time - my_smaple_profile.sample_getdata_req_time + 1);
#endif
			node->spsector += 1;
			break;

		case FNODE_ACK(FNODE_ADDR_DATA_POLL): // 点对点 采样数据
			// printf("{%d}\r\n", nid);
			// FIXME: 存储数据

			if (!fnode_can_payload_ok(frame, 3))
				break;

			if (frame->data[2] > FNODE_DATA_BLOCK_MAX) // 包内块数
			{
				// error
				WLOGW("A invalid fnum [%d]\r\n", frame->data[2]);
				break;
			}

			if (!fnode_can_payload_ok(frame,
			    FNODE_SAMPLE_DATA_OFFSET +
			    (uint32_t)frame->data[2] * FNODE_DATA_BLOCK_SIZE))
				break;

			my_smaple_profile.sample_getdata_ack_time = wos_system_clock_ms();

			for (uint8_t i = 0; i < frame->data[2]; i++)
			{
				uint8_t chn = frame->data[FNODE_SAMPLE_DATA_OFFSET + i * FNODE_DATA_BLOCK_SIZE + 3];
				// fprintf(stderr, "chn: %#x\r\n", chn);

				uint8_t src_nid = FNODE_CHN2NID(chn); // 发光的节点号 [1, 12]
				uint8_t src_sid = FNODE_CNN2SID(chn); // 发光节点的灯号 [0, 5]
				// 多余的校验，确保正确无误
				if (src_sid >= FNODE_SRC_MAX || src_nid < FNODE_NID_MIN || src_nid > FNODE_NID_MAX)
				{
					WLOGE("Invalid chn = %#x\r\n", chn);

					continue;
				}

				// 存储到相应的位置
				uint8_t src_idx = (src_nid - 1) + (src_sid * FNODE_NID_MAX); // 灯位置
				uint8_t det_idx = (nid - 1) * FNODE_DETID_MAX;				 // 发送数据的节点对应的采样数据存储位置
				memcpy(&node->sample_data[src_idx][det_idx],
					   &frame->data[FNODE_SAMPLE_DATA_OFFSET + i * FNODE_DATA_BLOCK_SIZE],
					   FNODE_DATA_BLOCK_SIZE);
			}

#if 0
				printf("*Node(%d) ack %llums\r\n",
					nid,
					my_smaple_profile.sample_getdata_ack_time - my_smaple_profile.sample_getdata_req_time + 1);
#endif

			node->spsector += 1;

			uint8_t idx = nid - 1; // nid ∈ [1,12] → idx ∈ [0,11]

			// 累加该节点的接收帧数
			node->poll_frame_cnt[idx]++;
			// WLOGD("Node %d: received DATA_POLL ACK frame #%d\n", nid, node->poll_frame_cnt[idx]);

			// 【关键】使用你设定的阈值判断
			if (node->poll_frame_cnt[idx] == node->poll_frame_number_need)
			{
#ifdef FNIRS_EV_IO
				pthread_mutex_lock(&node->event_mtx);
				node->node_ack_ready[nid - 1] = true;
				pthread_cond_signal(&node->event_cond);
				pthread_mutex_unlock(&node->event_mtx);
#else
				fnode_snd_event(node, FNODE_EVT_SP_SAVEDATA_POLL, nid);
#endif

				led_sample_nodes(0);

				node->poll_frame_cnt[idx] = 0;
			}

			break;

		case FNODE_ACK(FNODE_ADDR_SETUP_AND_UPLOAD):
		case FNODE_ACK(FNODE_ADDR_DATA_LAST_AND_UPLOAD):
			my_smaple_profile.sample_getdata_ack_time =
				wos_system_clock_ms();
			(void)fnode_store_upload_ack(node, frame, nid);
			break;

		case FNODE_ADDR_ERROR:
			// FIXME:
			break;

		default:
			break;
		}
		break;

	// 2025-11-14 maomao add
	case FHUB_STATE_OTA: // OTA升级状态
		// WLOGE("=== FHUB_STATE_OTA ===\r\n");
		fnode_CANFD_frame_ota_handler(node, frame, fnum);
		break;

	case FHUB_STATE_FACTORY_TEST: // 设备测试状态
		fnode_CANFD_frame_FACTORY_handler(node, frame, fnum);
		break;

	default:
		break;
	}
}

static void fnode_desc_monitor(fnode_t *node)
{
	static wos_timer_t timer = {0};
	uint32_t state = node->state;
	uint64_t now = 0;
	uint32_t i = 0;

	if (!wos_timer_is_expired(&timer))
	{
		return;
	}

	wos_timer_countdown_ms(&timer, FNODE_SCAN_PEROID * 3);

	if (FHUB_STATE_RESET != state)
	{
		return;
	}

	now = wos_system_clock_ms();

	for (i = 0; i < FNODE_NID_MAX; i++)
	{
		if (node->desc[i].alive && (now - node->desc[i].timestamp > FNODE_SCAN_PEROID * 3))
		{
			node->desc[i].alive = 0;
		}
	}
}

#ifndef FNIRS_EV_IO
static uint32_t fnode_gain_ok(fnode_t *node)
{
	uint32_t data[FNODE_DETID_MAX];

	// 合并并压缩
	for (uint32_t src_nid = 0; src_nid < FNODE_NID_MAX; src_nid++)
	{
		if (0 == node->desc[src_nid].alive)
		{
			continue;
		}

		for (uint32_t src_sid = 0; src_sid < FNODE_SRCID_MAX; src_sid++)
		{
			for (uint32_t det_nid = 0; det_nid < FNODE_NID_MAX; det_nid++)
			{
				if (0 == node->desc[det_nid].alive)
				{
					continue;
				}

				uint32_t src_idx = src_nid + (src_sid * FNODE_NID_MAX);
				uint32_t det_idx = det_nid * FNODE_DETID_MAX;

				memcpy(data, &node->sample_data[src_idx][det_idx], FNODE_DATA_BLOCK_SIZE);

				for (uint32_t i = 0; i < FNODE_DETID_MAX; i++)
				{
					// 达到目标增益值
					if ((data[i] & 0x7fffff) >= FNODE_SPDATA_TARGET)
					{
						return 1;
					}
				}
			}
		}
	}

	return 0;
}

#endif /* !FNIRS_EV_IO */

static int fnode_hub_process_events(fnode_t *node, uint8_t *fnum)
{
	uint16_t event = 0;
	uint16_t value = 0;
	int32_t rc;

	fnode_desc_monitor(node);

	rc = fnode_rcv_event(node, &event, &value);
	if (rc != 0)
		return 0;

	switch (event) {
	case FNODE_EVT_SP_RESET:
		if (fnum)
			*fnum = 0;
		memset(node->desc, 0, sizeof(node->desc));
		break;

	case FNODE_EVT_SP_START:
		fhub_start(node->hub, node->desc);
		break;

	case FNODE_EVT_SP_SAVEDATA:
		fhub_append(node->hub, node->sample_data, node->sens_data);
		node->sens_num = 0;
		node->spsector = 0;
		if (fnum)
			*fnum = (uint8_t)value;
		node->hub_fnum = (uint8_t)value;
		break;

	case FNODE_EVT_SP_SAVEDATA_POLL:
#ifdef FNIRS_EV_IO
		if (value >= 1 && value <= FNODE_NID_MAX) {
			pthread_mutex_lock(&node->event_mtx);
			node->node_ack_ready[value - 1] = true;
			pthread_cond_signal(&node->event_cond);
			pthread_mutex_unlock(&node->event_mtx);
		}
#else
		fnode_snd_event(node, event, value);
		return 0;
#endif
		break;

	case FNODE_EVT_SP_STOP:
		break;

	default:
		break;
	}

	return 1;
}

#ifdef FNIRS_EV_IO
void fnode_ev_hub_service(fnode_handler_t fh)
{
	fnode_t *node = (fnode_t *)fh;
	int i;

	if (!node)
		return;

	for (i = 0; i < 32; i++) {
		if (!fnode_hub_process_events(node, &node->hub_fnum))
			break;
	}
}

int32_t fnode_ev_hub_process_frame(fnode_handler_t fh, struct canfd_frame *frame)
{
	fnode_t *node = (fnode_t *)fh;

	if (!node || !frame)
		return -1;

	if (FNODE_CANID_NTOH != (frame->can_id & 0x700)) {
#ifdef FNIRS_EV_IO
		fnode_ev_rx_sniff(frame, 0);
#endif
		return 0;
	}

#ifdef FNIRS_EV_IO
	fnode_ev_rx_sniff(frame, 1);
#endif

	/* Drain CAN first with stable hub_fnum; hub events run after drain. */
	fnode_CANFD_frame_handler(node, frame, node->hub_fnum);
	return 1;
}

int32_t fnode_ev_can_fd(fnode_handler_t fh)
{
	fnode_t *node = (fnode_t *)fh;

	if (!node || !node->wcan)
		return -1;

	return wcan_get_fd(node->wcan);
}
#endif

#ifndef FNIRS_EV_IO
/********************************************************************************************
 *  HUB任务 -- 配合NODE任务进行工作
 *	工作过程：
 *		1.非阻塞式等待消息；若有，则对消息分类处理；
 *		2.主动接收CAN数据,并处理CAN帧;
 *
 ********************************************************************************************/
static void *fhub_task(void *args)
{
	fnode_t *node = (fnode_t *)args;
	struct canfd_frame frame;
	volatile uint8_t fnum = 0;
	int32_t rc = -1;

	fhub_open(&node->hub);

	while (node->run)
	{
		fnode_hub_process_events(node, &fnum);

		rc = wcanfd_read(node->wcan, &frame, 10);
		if (0 != rc)
		{
			usleep(50);
			continue;
		}

		if (FNODE_CANID_NTOH != (frame.can_id & 0x700))
		{
			usleep(50);
			continue;
		}

		fnode_CANFD_frame_handler(node, &frame, fnum);

		usleep(50);
	}

	fhub_close(node->hub);

	pthread_exit(NULL);
}

/********************************************************************************************
 * 每个LED灯点亮时间
 * 	2800us以下当前仍旧残留之前灯的光亮
 *	注意：最早版本为1300us，而且周期为250ms;
 *		测试3000us，且周期为350ms -> 400ms;
 ********************************************************************************************/
#define LED_ON_INTERVAL_US (3000) // 1300//(1000 * 3)

/********************************************************************************************
 * 留给节点进行采样的时间间隔
 *	当前节点实现方案：
 *		1.收到此命令后，直接使用最新的数据；
 *		2.也有可能，此命令到CAN RX FIFO，但MCU正在忙于ADC采样而会延迟执行，
 *		ADC采样大概30~40us
 ********************************************************************************************/
#define ADC_SAMPLE_INTERVAL_US (200)

/********************************************************************************************
 *  采样过程处理
 ********************************************************************************************/
static void fnode_sample_handler(fnode_t *node)
{
	fnode_desc_t desc[FNODE_NID_MAX];
	uint8_t buf[64] = {0};
	uint8_t gain_735 = 0; //
	uint8_t gain_850 = 0; //
	uint8_t fnum = 0;
	/* 1个NODE -- 15HZ ; 12个NODE -- 4HZ*/
	// uint32_t fps[FNODE_NID_MAX] = {15, 10, 8, 8, 8, 8, 6, 6, 5, 5, 4, 4};

	uint32_t sample_period_table2[FNODE_NID_MAX] = {66, 100, 125, 125, 125,
													125, 166, 166,
													200,
													200,
													250,
													250};

	uint32_t sample_period_table[FNODE_NID_MAX] = {
		66,	 /* 1 -- 66 */
		100, /* 2 -- 100 */
		240, /* 3 -- 125 */
		240, /* 4 -- 125 */
		240, /* 5 -- 125 */
		240, /* 6 -- 125 */
		240, /* 7 -- 166 */
		280, /* 8 -- 166 */
		350, /* 9 -- 200 */
		400, /* 10 -- 200 */
		450, /* 11 -- 250 */
		500	 /* 12 -- 250 */
	};
	uint32_t sample_peroid_ms = 0;

	// TODO: 根据模块数量，计算采样周期
	if (node->node_num < FNODE_NID_MIN || node->node_num > FNODE_NID_MAX)
	{
		WLOGW("Invalid node num: %d\r\n", node->node_num);

		return;
	}

	// 1.获取整体采样频率，计算采样周期 [12个节点时为250ms]
	sample_peroid_ms = sample_period_table[node->node_num - 1];
	WLOGI("[%d]nodes, sample_peroid_ms: %d ms\r\n", node->node_num, sample_peroid_ms);

	fnode_desc(node, desc);

#if 1 // 后续改进：PC上位机主动进行增益调整过程

	gain_735 = node->gain_735;
	gain_850 = node->gain_850;
#endif

	wos_timer_init(&node->timer); /* 强制过期 */

	// 3.根据采样标志进行工作
	while (node->sample)
	{
		my_smaple_profile.sample_start_time = wos_system_clock_ms();

		wos_timer_countdown_ms(&node->timer, sample_peroid_ms);

		// FIXME: 循环采样。 setup => sample => setup ... (loop)
		// printf("[%llu]sample <START...>\r\n", my_smaple_profile.sample_start_time);

		//===1.执行采样命令===
		// 采样中LED指示
		led_sample_nodes(1);

		// 逐个NODE
		for (uint32_t nid = 0; nid < FNODE_NID_MAX; nid++)
		{
			if (0 == desc[nid].alive)
			{
				continue;
			}

			// nid = i + 1;
			// 逐个LED灯
			for (uint32_t sid = 0; sid < FNODE_SRC_MAX; sid++)
			{
				uint8_t chn = FNODE_CHN(nid + 1, sid);

				// 亮灯:FNODE_ADDR_SETUP
				buf[0] = FNODE_ADDR_SETUP;
				buf[1] = fnum;
				buf[2] = chn;
				buf[3] = gain_735;
				buf[4] = gain_850;
				fnode_can_write(node, buf, 5);

				usleep(LED_ON_INTERVAL_US);

				// 采样: FNODE_ADDR_SAMPLE
				buf[0] = FNODE_ADDR_SAMPLE;
				buf[1] = fnum;
				buf[2] = chn;
				fnode_can_write(node, buf, 3);

				usleep(ADC_SAMPLE_INTERVAL_US);
			}
		}

		//===2.获取本周期采样结果 [此处仅仅下发上报数据命令，不会等待数据]===
		my_smaple_profile.sample_getdata_req_time = wos_system_clock_ms();
		buf[0] = FNODE_ADDR_DATA;
		buf[1] = fnum;
		fnode_can_write(node, buf, 2);

		my_smaple_profile.sample_stop_time = wos_system_clock_ms();

		printf("Sample [%d], take %llu ms\r\n",
			   fnum,
			   my_smaple_profile.sample_stop_time - my_smaple_profile.sample_start_time + 1);

		fnum++; // 下一轮采样周期

		// 采样停止LED指示
		led_sample_nodes(0);

		//*****************************************
		//*****************************************
		//*****************************************
		//===3.等待采样过程周期过期===
		// 过期意味着HUB已经收完节点上报的ACK数据
		while (!wos_timer_is_expired(&node->timer))
		{
			usleep(1000);
		}

		// printf("All Node ack data %llu ms\r\n",
		//	my_smaple_profile.sample_getdata_ack_time - my_smaple_profile.sample_getdata_req_time + 1);

		//===4.通知保存数据 == 第一次实际没有数据可以保存与传输的[已修改]===
		fnode_snd_event(node, FNODE_EVT_SP_SAVEDATA, fnum);

		if (node->sample == 0)
			printf("sample stop\r\n");
	}
}

/********************************************************************************************
 *  采样过程处理2 poll
 ********************************************************************************************/
static void fnode_10HZ_sample_handler_poll(fnode_t *node)
{
	fnode_desc_t desc[FNODE_NID_MAX];
	uint8_t buf[64] = {0};
	uint8_t gain_735 = 0; //
	uint8_t gain_850 = 0; //
	uint8_t fnum = 0;
	uint8_t rev_ack_nid_count = 0; // 计算当前有几个节点上报完数据了
	uint8_t alive_node_number = 0; // 当前存活的节点数

	uint32_t sample_peroid_ms = 0;

	//核实node节点数量
	if (node->node_num < FNODE_NID_MIN || node->node_num > FNODE_NID_MAX)
	{
		WLOGW("Invalid node num: %d\r\n", node->node_num);

		return;
	}

	// 根据标志选择LED发光序列，是使用默认的LED发光序列，还是选用上位机发送的
	const sample_node_info_t *current_array;
	uint32_t array_size;

	if (node && node->array_from_host_is_used)
	{
		current_array = node->sample_node_array_from_host;
		array_size = node->led_num_from_host; // 使用实际数量，避免遍历未初始化项
	}
	else
	{
		current_array = sample_node_array; // 默认数组（全局或局部）
		array_size = sizeof(sample_node_array) / sizeof(sample_node_array[0]);
		node->sample_peroid_ms = 120; // 默认数组用120ms周期
	}

	fdatalog_write_led_array((const uint8_t *)current_array, (uint8_t)array_size,
		node->array_from_host_is_used ? 1 : 0);

	// TODO: 根据模块数量，计算采样周期
	if (node->node_num < FNODE_NID_MIN || node->node_num > FNODE_NID_MAX)
	{
		WLOGW("Invalid node num: %d\r\n", node->node_num);

		return;
	}

	// 1.获取整体采样频率，计算采样周期 [12个节点时为250ms]
	// sample_peroid_ms = 120; //110; //99;
	WLOGI("[%d]nodes, sample_peroid_ms: %d ms\r\n", node->node_num, node->sample_peroid_ms);

	fnode_desc(node, desc);

#if 1 // 后续改进：PC上位机主动进行增益调整过程

	gain_735 = node->gain_735;
	gain_850 = node->gain_850;
#endif

	// wos_timer_init(&node->timer); /* 强制过期 */

	// 统计当前存活的节点数
	for (uint8_t i = 0; i < FNODE_NID_MAX; i++)
	{
		if (desc[i].alive)
		{ // 更简洁：直接判断非零
			alive_node_number++;
		}
	}

	// 3.根据采样标志进行工作
	while (node->sample)
	{
		// my_smaple_profile.sample_start_time = wos_system_clock_ms();

		/*
		算出需要几个64byte长度的CANFD贞，首先得到亮灯数组长度，然后*2（735nm和850nm），然后/3向上取整数
		*/
		node->poll_frame_number_need = (2 * array_size + 2) / 3;
		WLOGI("Total CANFD frame number need: %u\n", node->poll_frame_number_need);

		// wos_timer_countdown_ms(&node->timer, sample_peroid_ms);//开始计时，sample_peroid_ms=99ms
		// uint64_t now = get_monotonic_ms();
		// WLOGI("Timer set: now=%llu, deadline=%llu\n", (unsigned long long)now, (unsigned long long)node->timer.time);

		// FIXME: 循环采样。 setup => sample => setup ... (loop)
		// printf("[%llu]sample <START...>\r\n", my_smaple_profile.sample_start_time);

		//===1.执行采样命令===
		// 采样中LED指示
		led_sample_nodes(1);

		// 逐个NODE LED去点亮
		for (uint32_t i = 0; i < array_size; i++)
		{
			uint8_t nid = current_array[i].node_id - 1;		 // 节点位置[1, 12]
			uint8_t led_id = current_array[i].node_led_id - 1; // 灯位置[1, 3]

			if (0 == desc[nid].alive)
			{
				WLOGW("Node %d is dead, skipping...\n", nid + 1); // 打印死掉的节点
				continue;
			}

			// WLOGI("Sampling started for node %d \n", sample_node_array_huang[i].node_id);

			for (uint32_t j = 0; j < 2; j++) // 735nm 850nm
			{
				uint8_t chn = FNODE_CHN(nid + 1, 2 * led_id + j);

				// 亮灯
				buf[0] = FNODE_ADDR_SETUP;
				buf[1] = fnum;
				buf[2] = chn;
				if (1 == node->array_from_host_is_used) // 外部有传入序列
				{
					if (j == 0)
					{ // 735
						buf[3] = current_array[i].power_735;
					}
					else
					{
						// 850
						buf[3] = current_array[i].power_850;
					}
				}
				else // 没有外部序列传入，使用默认
				{
					if (j == 0)
					{ // 735
						buf[3] = gain_735;
					}
					else
					{
						// 850
						buf[3] = gain_850;
					}
				}

				fnode_can_write(node, buf, 4);

				usleep(LED_ON_INTERVAL_US);

				// 采样
				buf[0] = FNODE_ADDR_SAMPLE;
				buf[1] = fnum;
				buf[2] = chn;
				fnode_can_write(node, buf, 3);

				usleep(ADC_SAMPLE_INTERVAL_US);
			}
		}

//===2.获取本周期采样结果 [此处仅仅下发上报数据命令，不会等待数据]===
// my_smaple_profile.sample_getdata_req_time = wos_system_clock_ms();
#if 0 // 广播方式轮询上报
				buf[0] = FNODE_ADDR_DATA;
				buf[1] = fnum;
				fnode_can_write(node, buf, 2);
#else // 点对点上报

		while (alive_node_number != rev_ack_nid_count)
		{ // 这一层while等待收集齐所有节点

			for (uint8_t nid = 1; nid <= FNODE_NID_MAX; nid++)
			{
				// 检查全局轮询是否已超时（如总超时 100ms）
				/*
				if (wos_timer_is_expired(&node->timer)) {
					WLOGI("Data poll total timeout, stop polling at nid=%d\n", nid);
					WLOGI("Expired! now=%llu, timer.time=%llu\n", (unsigned long long)get_monotonic_ms(), (unsigned long long)node->timer.time);
					break;
				}*/

				// led_test_nodes(1);

				if (!desc[nid - 1].alive)
				{
					continue; // 跳过非活跃节点
				}

				// === 1. 发送 POLL 请求 ===
				buf[0] = FNODE_ADDR_DATA_POLL;
				buf[1] = nid; // 目标节点 ID
				fnode_can_write(node, buf, 2);

				// led_test_nodes(1);

				// === 2. 等待本节点 DATA_POLL 收齐 ===
#ifdef FNIRS_EV_IO
				{
					struct timespec ts;
					int got_ack = 0;

					clock_gettime(CLOCK_REALTIME, &ts);
					ts.tv_sec += 1;

					pthread_mutex_lock(&node->event_mtx);
					while (!node->node_ack_ready[nid - 1]) {
						if (pthread_cond_timedwait(&node->event_cond,
							    &node->event_mtx, &ts) == ETIMEDOUT)
							break;
					}
					if (node->node_ack_ready[nid - 1]) {
						node->node_ack_ready[nid - 1] = false;
						got_ack = 1;
					}
					pthread_mutex_unlock(&node->event_mtx);

					if (got_ack)
						rev_ack_nid_count++;
					else
						WLOGW("Node %u DATA_POLL timeout\r\n", nid);
				}
#else
				while (1)
				{
					uint16_t event, value;

					if (fnode_rcv_event(node, &event, &value) == 0) {
						if (event == FNODE_EVT_SP_SAVEDATA_POLL &&
						    value == nid) {
							rev_ack_nid_count++;
							break;
						}
					}
				}
#endif

				
				led_sample_nodes(1);
				// usleep(100);//小等一下
			}
		}
		rev_ack_nid_count = 0; // 计数器清零

#endif

		// 采样停止LED指示
		// led_sample_nodes(0);

		//===4.通知保存数据 == 第一次实际没有数据可以保存与传输的[已修改]===
		fnode_snd_event(node, FNODE_EVT_SP_SAVEDATA, fnum);

		usleep(1000); //
		led_sample_nodes(0);

		if (node->sample == 0)
			printf("sample stop\r\n");
	}
}

/********************************************************************************************
 *  10Hz边采边传：利用每次亮灯/采样间隔上传上一通道数据。
 ********************************************************************************************/
static void fnode_10HZ_sample_and_upload_handler(fnode_t *node)
{
	fnode_desc_t desc[FNODE_NID_MAX];
	uint8_t buf[64] = {0};
	uint8_t gain_735 = 0; //
	uint8_t gain_850 = 0; //
	volatile uint8_t fnum = 0;

	// uint32_t sample_peroid_ms = 0;

	// 根据标志选择LED发光序列，是使用默认的LED发光序列，还是选用上位机发送的
	const sample_node_info_t *current_array;
	uint32_t array_size;

	if (node && node->array_from_host_is_used)
	{
		current_array = node->sample_node_array_from_host;
		array_size = node->led_num_from_host; // 使用实际数量，避免遍历未初始化项
		node->sample_peroid_ms =
			(LED_ON_INTERVAL_US * array_size * 2 + 15000) / 1000;
	}
	else
	{
		current_array = sample_node_array; // 默认数组（全局或局部）
		array_size = sizeof(sample_node_array) / sizeof(sample_node_array[0]);
		node->sample_peroid_ms = 90;
	}

	fdatalog_write_led_array((const uint8_t *)current_array, (uint8_t)array_size,
		node->array_from_host_is_used ? 1 : 0);

	// TODO: 根据模块数量，计算采样周期
	if (node->node_num < FNODE_NID_MIN || node->node_num > FNODE_NID_MAX)
	{
		WLOGW("Invalid node num: %d\r\n", node->node_num);

		return;
	}

	// 1.获取整体采样频率，计算采样周期 [12个节点时为250ms]
	// sample_peroid_ms = 120; //110; //99;
	WLOGI("[%d]nodes, sample_peroid_ms: %d ms\r\n", node->node_num, node->sample_peroid_ms);

	fnode_desc(node, desc);

#if 1 // 后续改进：PC上位机主动进行增益调整过程

	gain_735 = node->gain_735;
	gain_850 = node->gain_850;

#endif

	wos_timer_init(&node->timer); /* 强制过期 */

	// 3.根据采样标志进行工作
	while (node->sample)
	{
		// my_smaple_profile.sample_start_time = wos_system_clock_ms();

		wos_timer_countdown_ms(&node->timer, node->sample_peroid_ms);

		// FIXME: 循环采样。 setup => sample => setup ... (loop)
		// printf("[%llu]sample <START...>\r\n", my_smaple_profile.sample_start_time);

		//===1.执行采样命令===
		// 采样中LED指示
		led_sample_nodes(1);

		/*
		//新增，用于从主机设置发光序列
		sample_node_info_t sample_node_array_from_host[MAX_SAMPLE_NODES_LED]; //记录从上位机传下来的发光序列
		uint8_t led_num_from_host;// 总共发光led的数量
		volatile bool array_from_host_is_used;//标志位，是否启用设置发光序列功能，0不启用，1启用
		*/

		// 逐个NODE LED
		for (uint32_t i = 0; i < array_size; i++)
		{
			uint8_t nid = current_array[i].node_id - 1;		   // 节点位置[1, 12]
			uint8_t led_id = current_array[i].node_led_id - 1; // 灯位置[1, 3]

			if (0 == desc[nid].alive)
			{
				continue;
			}

			for (uint32_t j = 0; j < 2; j++) // 735nm 850nm
			{
				uint8_t chn = FNODE_CHN(nid + 1, 2 * led_id + j);

				// 亮灯
				buf[0] = FNODE_ADDR_SETUP_AND_UPLOAD;
				buf[1] = fnum;
				buf[2] = chn;
				if (1 == node->array_from_host_is_used) // 外部有传入序列
				{
					if (j == 0)
					{ // 735
						buf[3] = current_array[i].power_735;
					}
					else
					{
						// 850
						buf[3] = current_array[i].power_850;
					}
				}
				else // 没有外部序列传入，使用默认
				{
					if (j == 0)
					{ // 735
						buf[3] = gain_735;
					}
					else
					{
						// 850
						buf[3] = gain_850;
					}
				}

				fnode_can_write(node, buf, 4);

				usleep(LED_ON_INTERVAL_US);

				// 采样
				buf[0] = FNODE_ADDR_SAMPLE_AND_UPLOAD;
				buf[1] = fnum;
				buf[2] = chn;
				fnode_can_write(node, buf, 3);

				usleep(ADC_SAMPLE_INTERVAL_US);
			}
		}

		/*
		 * 最后一个 SAMPLE_AND_UPLOAD 后没有下一次 SETUP 可触发上报，
		 * 因而单独请求节点冲刷最后一个通道的数据。
		 */
		buf[0] = FNODE_ADDR_DATA_LAST_AND_UPLOAD;
		fnode_can_write(node, buf, 1);

		// my_smaple_profile.sample_stop_time = wos_system_clock_ms();

		// printf("Sample [%d], take %llu ms\r\n",
		//	fnum,
		//	my_smaple_profile.sample_stop_time - my_smaple_profile.sample_start_time + 1);

		fnum++; // 下一轮采样周期
		// printf("fnum is %d \r\n", fnum);

		// 采样停止LED指示
		led_sample_nodes(0);

		//*****************************************
		//*****************************************
		//*****************************************
		//===3.等待采样过程周期过期===
		// 过期意味着HUB已经收完节点上报的ACK数据
		while (!wos_timer_is_expired(&node->timer))
		{
			usleep(1000);
		}

		// printf("All Node ack data %llu ms\r\n",
		//	my_smaple_profile.sample_getdata_ack_time - my_smaple_profile.sample_getdata_req_time + 1);

		// 给最后一批CAN-FD ACK留出接收时间，再提交完整帧。
		usleep(4000);
		fnode_snd_event(node, FNODE_EVT_SP_SAVEDATA, fnum);

		if (node->sample == 0)
			printf("sample stop\r\n");
	}
}

#endif /* !FNIRS_EV_IO */

static int fnode_task_OTA_START_handler(fnode_t *node)
{
	uint8_t addr = FNODE_ADDR_RESET;
	uint8_t buf[64] = {0};
	off_t firmware_size;
	unsigned short firmware_crc;

	// reset before OTA
	addr = FNODE_ADDR_RESET;
	fnode_can_write(node, &addr, 1);

	// 获取文件大小
	struct stat file_stat;
	if (stat(ota_info.bin_path, &file_stat) < 0)
	{
		perror("stat file failed");
		return -1;
	}
	if (file_stat.st_size <= 0 ||
	    (uint64_t)file_stat.st_size > OTA_IMAGE_MAX_SIZE) {
		WLOGW("OTA image size %ld exceeds staging partition\r\n",
		      (long)file_stat.st_size);
		return -1;
	}

	printf("Found .bin file: %s, size: %ld bytes\n",
	       ota_info.bin_path, (long)file_stat.st_size);

	if (calculate_bin_crc16(ota_info.bin_path, &firmware_size,
				&firmware_crc) != 0)
	{ // crc = 0x717F
		perror("file crc failed");
		return -1;
	}
	if (firmware_size != file_stat.st_size ||
	    firmware_size <= 0 ||
	    (uint64_t)firmware_size > OTA_IMAGE_MAX_SIZE) {
		WLOGW("OTA image changed while calculating CRC: stat=%ld crc_size=%ld\r\n",
		      (long)file_stat.st_size, (long)firmware_size);
		return -1;
	}
	ota_info.firmware_bin_size = (unsigned int)firmware_size;
	ota_info.firmware_bin_crc = firmware_crc;

	ota_info.node_ack_ota_start_bitmap = 0;
	printf("OTA node bitmap: 0x%X -- %d nodes\r\n", ota_info.node_ota_bitmap, fnode_num(node));

	int index = 0;
	// 下发OTA开始命令
	buf[index++] = FNODE_CMD_OTA_OTA_START;
	// 固件大小
	buf[index++] = (ota_info.firmware_bin_size >> 24) & 0xFF;
	buf[index++] = (ota_info.firmware_bin_size >> 16) & 0xFF;
	buf[index++] = (ota_info.firmware_bin_size >> 8) & 0xFF;
	buf[index++] = ota_info.firmware_bin_size & 0xFF;
	// 固件的CRC16校验值
	buf[index++] = (ota_info.firmware_bin_crc >> 24) & 0xFF;
	buf[index++] = (ota_info.firmware_bin_crc >> 16) & 0xFF;
	buf[index++] = (ota_info.firmware_bin_crc >> 8) & 0xFF;
	buf[index++] = ota_info.firmware_bin_crc & 0xFF;
	//[NID8升级标志]..[NID1升级标志]
	buf[index++] = ota_info.node_ota_bitmap & 0xFF;
	//[NID16升级标志]..[NID9升级标志]
	buf[index++] = (ota_info.node_ota_bitmap >> 8) & 0xFF;

	return fnode_can_write(node, buf, index);
}

#define PACKAGE_SIZE 56 // 分块读取大小：56字节
static uint8_t package_buffer[PACKAGE_SIZE];
static uint8_t protocol_buf[64] = {0};
static uint8_t protocol_len;

static int fnode_task_OTA_PACKAGE_handler(int fd, fnode_t *node, fnode_desc_t desc[FNODE_NID_MAX])
{
	(void)desc;

	// 1.读取固件包
	ssize_t read_len = read(fd, package_buffer, PACKAGE_SIZE);
	// printf("OTA -- read %d\r\n", read_len);
	if (read_len < 0) // 文件读取失败
	{

		perror("Failed to read BIN file");

		return -1;
	}

	if (read_len == 0) // 文件读取完毕 == 空包
	{
		printf("File transmission completed\n");
		ota_info.firmware_bin_package_len = 0;

		return 1;
	}

	// 清发送缓存
	memset(protocol_buf, 0, sizeof(protocol_buf));

	// 更新本次固件包长度
	ota_info.firmware_bin_package_len = read_len;

	// 2.按协议打包
	int index = 0;

	// 下发OTA开始命令
	protocol_buf[index++] = FNODE_CMD_OTA_OTA_PACKAGE;

	// 固件包偏移
	protocol_buf[index++] = (ota_info.firmware_bin_offset >> 24) & 0xFF;
	protocol_buf[index++] = (ota_info.firmware_bin_offset >> 16) & 0xFF;
	protocol_buf[index++] = (ota_info.firmware_bin_offset >> 8) & 0xFF;
	protocol_buf[index++] = ota_info.firmware_bin_offset & 0xFF;

	// 固件包大小
	protocol_buf[index++] = ota_info.firmware_bin_package_len;

	// 固件包CRC16校验
	unsigned short crc_cal = my_crc16((char *)package_buffer,
					 ota_info.firmware_bin_package_len);

	protocol_buf[index++] = (crc_cal >> 8) & 0xFF;
	protocol_buf[index++] = crc_cal & 0xFF;

	// 固件包（最大56字节）
	memcpy(&protocol_buf[index], package_buffer, ota_info.firmware_bin_package_len);
	index += ota_info.firmware_bin_package_len;
	protocol_len = (uint8_t)index;

	// 3.CAN FD发送
	if (0 != fnode_can_write(node, protocol_buf, index))
	{
		perror("Failed to send package by CANFD");

		return -1;
	}

	// 更新OTA发送固件长度、下一包固件偏移位置
	ota_info.firmware_bin_tx_len += ota_info.firmware_bin_package_len;
	ota_info.firmware_bin_offset += ota_info.firmware_bin_package_len;

	return 0;
}

static int fnode_task_OTA_PACKAGE_resend_handler(fnode_t *node)
{
	if (!node || protocol_len == 0 ||
	    protocol_buf[0] != FNODE_CMD_OTA_OTA_PACKAGE)
		return -1;

	return fnode_can_write(node, protocol_buf, protocol_len);
}

static int fnode_task_OTA_FINISH_handler(fnode_t *node, fnode_desc_t desc[FNODE_NID_MAX])
{
	uint8_t buf[64] = {0};

	(void)desc;

	int index = 0;
	// 下发OTA开始命令
	buf[index++] = FNODE_CMD_OTA_OTA_FINISH;

	return fnode_can_write(node, buf, index);
}

static void fnode_ota_put_be32(uint8_t *data, uint32_t value)
{
	data[0] = (uint8_t)(value >> 24);
	data[1] = (uint8_t)(value >> 16);
	data[2] = (uint8_t)(value >> 8);
	data[3] = (uint8_t)value;
}

static int fnode_ota_report_progress(uint32_t bitmap)
{
	uint8_t msg[12] = {
		(uint8_t)(FUSB_OTA_PROGRESS >> 8),
		(uint8_t)FUSB_OTA_PROGRESS,
	};

	/* FUSB_OTA_PROGRESS body: bitmap LE16, sent BE32, total BE32. */
	msg[2] = (uint8_t)bitmap;
	msg[3] = (uint8_t)(bitmap >> 8);
	fnode_ota_put_be32(msg + 4, ota_info.firmware_bin_tx_len);
	fnode_ota_put_be32(msg + 8, ota_info.firmware_bin_size);
	return fusb_factory_ringbuf_enqueue(msg, sizeof(msg));
}

static int fnode_ota_report_result(uint32_t bitmap)
{
	uint8_t msg[6] = {
		(uint8_t)(FUSB_OTA_RESULT >> 8),
		(uint8_t)FUSB_OTA_RESULT,
		(uint8_t)(bitmap >> 24),
		(uint8_t)(bitmap >> 16),
		(uint8_t)(bitmap >> 8),
		(uint8_t)bitmap,
	};
	int rc;

	/*
	 * The deployed GOLGI OTA serial tool reads FUSB_OTA_RESULT as the
	 * original four-byte big-endian bitmap.  Keep that wire format here:
	 * sending only the two bytes shown in the newer requirement makes the
	 * tool consume the trailing A5 A5 frame marker as bitmap data.
	 */
	rc = fusb_factory_ringbuf_enqueue(msg, sizeof(msg));
	WLOGI("OTA RESULT queued bitmap=0x%08x payload=4 rc=%d\r\n",
	      bitmap, rc);
	return rc;
}

#ifdef FNIRS_EV_IO
#include "ev_can.h"

#define FNODE_EV_OTA_OFF          0
#define FNODE_EV_OTA_START_WAIT   1
#define FNODE_EV_OTA_PKG_SEND     2
#define FNODE_EV_OTA_PKG_WAIT     3
#define FNODE_EV_OTA_FINISH_SEND  4
#define FNODE_EV_OTA_FINISH_WAIT  5
#define FNODE_EV_OTA_RESET_WAIT   6

static void fnode_ev_ota_disarm(fnode_t *node)
{
	if (!node)
		return;

	if (node->ev_ota_fd >= 0) {
		close(node->ev_ota_fd);
		node->ev_ota_fd = -1;
	}

	node->ev_ota_phase = FNODE_EV_OTA_OFF;
	node->ev_ota_package_retry = 0;
	node->ota_in_flag = 0;
	ev_can_ota_disarm();
}

static void fnode_ev_ota_error(fnode_t *node)
{
	if (!node)
		return;

	ota_info.status = FNODE_OTA_STATUS_ERROR;
	node->ota_start_flag = 0;
	(void)fnode_ota_report_result(0);
	fnode_ev_ota_disarm(node);
	node->reset = 1;
	WLOGW("fnode_ev_ota_error\r\n");
}

static void fnode_ev_ota_begin_package(fnode_t *node)
{
	ota_info.status = FNODE_OTA_STATUS_PACKAGE;
	ota_info.node_ota_package_handling = 1;
	ota_info.firmware_bin_tx_len = 0;
	ota_info.firmware_bin_offset = 0;
	ota_info.firmware_bin_package_len = 0;

	node->ev_ota_fd = open(ota_info.bin_path, O_RDONLY);
	if (node->ev_ota_fd < 0) {
		perror("OTA -- open bin file failed");
		ota_info.status = FNODE_OTA_STATUS_ERROR;
		ota_info.node_ota_package_handling = 0;
		fnode_ev_ota_error(node);
		return;
	}

	node->ev_ota_phase = FNODE_EV_OTA_PKG_SEND;
}

void fnode_ev_ota_begin(fnode_handler_t fh)
{
	fnode_t *node = (fnode_t *)fh;

	if (!node || node->state != FHUB_STATE_OTA ||
	    node->ev_ota_phase != FNODE_EV_OTA_OFF)
		return;

	node->ev_ota_fd = -1;
	node->ev_ota_package_retry = 0;
	node->ota_in_flag = 0;
	protocol_len = 0;
	ota_info.node_ack_ota_start_bitmap = 0;
	ota_info.node_ack_ota_package_bitmap = 0;
	ota_info.node_ack_ota_finish_bitmap = 0;
	ota_info.node_ota_current_bitmap = 0;
	fnode_desc(node, node->desc);

	if (fnode_task_OTA_START_handler(node) != 0) {
		WLOGW("OTA START CAN-FD transmit failed\r\n");
		fnode_ev_ota_error(node);
		return;
	}
	ota_info.status = FNODE_OTA_STATUS_START;
	node->ota_in_flag = 1;
	ota_info.node_ack_ota_package_bitmap = 0;

	node->ev_ota_deadline_ms = wos_system_clock_ms() + FNODE_OTA_START_WAIT_TIME;
	node->ev_ota_phase = FNODE_EV_OTA_START_WAIT;
	WLOGI("fnode_ev_ota_begin: event mode (bitmap=0x%X)\r\n",
	      ota_info.node_ota_bitmap);

	ev_can_ota_arm_deferred();
}

static void fnode_ev_ota_enter(fnode_t *node)
{
	if (!node || node->ev_ota_phase != FNODE_EV_OTA_OFF)
		return;

	WLOGW("*** FHUB_STATE_OTA ***\r\n");
	ota_info.status = FNODE_OTA_STATUS_IDLE;
	node->state = FHUB_STATE_OTA;
	fnode_ev_ota_begin(node);
}

int fnode_ev_ota_active(fnode_handler_t fh)
{
	fnode_t *node = (fnode_t *)fh;

	if (!node)
		return 0;

	return node->ev_ota_phase != FNODE_EV_OTA_OFF;
}

void fnode_ev_ota_tick(fnode_handler_t fh)
{
	fnode_t *node = (fnode_t *)fh;
	uint64_t now;
	int ret;

	if (!node || node->ev_ota_phase == FNODE_EV_OTA_OFF)
		return;

	now = wos_system_clock_ms();

	switch (node->ev_ota_phase) {
	case FNODE_EV_OTA_START_WAIT:
		if (ota_info.node_ack_ota_start_bitmap == ota_info.node_ota_bitmap ||
		    now >= node->ev_ota_deadline_ms) {
			printf("OTA -- get [OTA START] ack: 0x%X, 0x%X\r\n",
			       ota_info.node_ack_ota_start_bitmap,
			       ota_info.node_ota_bitmap);
			ota_info.node_ota_current_bitmap =
				ota_info.node_ack_ota_start_bitmap;

			if (ota_info.node_ota_current_bitmap == 0) {
				fnode_ev_ota_error(node);
				return;
			}

			fnode_ev_ota_begin_package(node);
		}
		break;

	case FNODE_EV_OTA_PKG_SEND:
		ota_info.node_ack_ota_package_bitmap = 0;
		node->ev_ota_package_retry = 0;
		ret = fnode_task_OTA_PACKAGE_handler(node->ev_ota_fd, node,
						     node->desc);
		if (ret < 0) {
			perror("OTA -- package failed");
			ota_info.status = FNODE_OTA_STATUS_ERROR;
			ota_info.node_ota_package_handling = 0;
			fnode_ev_ota_error(node);
			return;
		}

		if (ret == 1) {
			if (node->ev_ota_fd >= 0) {
				close(node->ev_ota_fd);
				node->ev_ota_fd = -1;
			}
			ota_info.node_ota_package_handling = 0;
			node->ev_ota_phase = FNODE_EV_OTA_FINISH_SEND;
			return;
		}

		node->ev_ota_deadline_ms = now + FNODE_OTA_PACKAGE_WAIT_TIME;
		node->ev_ota_phase = FNODE_EV_OTA_PKG_WAIT;
		break;

	case FNODE_EV_OTA_PKG_WAIT:
		if (ota_info.node_ota_current_bitmap !=
		    ota_info.node_ack_ota_package_bitmap &&
		    now < node->ev_ota_deadline_ms)
			break;

		if (ota_info.node_ota_current_bitmap !=
		    ota_info.node_ack_ota_package_bitmap &&
		    node->ev_ota_package_retry < FNODE_OTA_PACKAGE_MAX_RETRY) {
			WLOGW("OTA PACKAGE timeout offset=%u ack=0x%X/0x%X; retry %u/%u\r\n",
			      ota_info.firmware_bin_offset -
				ota_info.firmware_bin_package_len,
			      ota_info.node_ack_ota_package_bitmap,
			      ota_info.node_ota_current_bitmap,
			      node->ev_ota_package_retry + 1,
			      FNODE_OTA_PACKAGE_MAX_RETRY);
			if (fnode_task_OTA_PACKAGE_resend_handler(node) != 0) {
				fnode_ev_ota_error(node);
				return;
			}
			node->ev_ota_package_retry++;
			node->ev_ota_deadline_ms =
				now + FNODE_OTA_PACKAGE_WAIT_TIME;
			break;
		}

		if (ota_info.node_ota_current_bitmap !=
		    ota_info.node_ack_ota_package_bitmap) {
			WLOGW("OTA PACKAGE dropping nodes: active=0x%X ack=0x%X\r\n",
			      ota_info.node_ota_current_bitmap,
			      ota_info.node_ack_ota_package_bitmap);
			ota_info.node_ota_current_bitmap &=
				ota_info.node_ack_ota_package_bitmap;
		}

		if (ota_info.node_ota_current_bitmap == 0) {
			fnode_ev_ota_error(node);
			return;
		}

		(void)fnode_ota_report_progress(
			ota_info.node_ota_current_bitmap);

		if (ota_info.firmware_bin_tx_len >= ota_info.firmware_bin_size) {
			if (node->ev_ota_fd >= 0) {
				close(node->ev_ota_fd);
				node->ev_ota_fd = -1;
			}
			ota_info.node_ota_package_handling = 0;
			node->ev_ota_phase = FNODE_EV_OTA_FINISH_SEND;
			return;
		}

		node->ev_ota_phase = FNODE_EV_OTA_PKG_SEND;
		break;

	case FNODE_EV_OTA_FINISH_SEND:
		ota_info.node_ack_ota_finish_bitmap = 0;
		ota_info.status = FNODE_OTA_STATUS_FINISH;
		if (fnode_task_OTA_FINISH_handler(node, node->desc) != 0) {
			fnode_ev_ota_error(node);
			return;
		}
		node->ev_ota_deadline_ms = now + FNODE_OTA_FINISH_WAIT_TIME;
		node->ev_ota_phase = FNODE_EV_OTA_FINISH_WAIT;
		break;

	case FNODE_EV_OTA_FINISH_WAIT:
		if (ota_info.node_ota_current_bitmap ==
		    ota_info.node_ack_ota_finish_bitmap ||
		    now >= node->ev_ota_deadline_ms) {
			printf("OTA -- ota finish: 0x%X\r\n",
			       ota_info.node_ack_ota_finish_bitmap);

			(void)fnode_ota_report_result(
				ota_info.node_ack_ota_finish_bitmap);

			node->ota_start_flag = 0;
			node->reset = 1;
			node->ev_ota_deadline_ms = now + 200;
			node->ev_ota_phase = FNODE_EV_OTA_RESET_WAIT;
		}
		break;

	case FNODE_EV_OTA_RESET_WAIT:
		if (now < node->ev_ota_deadline_ms)
			return;

		ota_info.status = FNODE_OTA_STATUS_GONE;
		fnode_ev_ota_disarm(node);
		WLOGI("fnode_ev_ota_finish\r\n");
		break;

	default:
		fnode_ev_ota_error(node);
		break;
	}
}
#endif /* FNIRS_EV_IO */

#ifndef FNIRS_EV_IO
/********************************************************************************************
 *  NODE任务状态机 -- OTA过程处理
 *
 ********************************************************************************************/
static int fnode_task_OTA_handler(fnode_t *node)
{
	fnode_desc_t desc[FNODE_NID_MAX];

	// 按照当前老设计方案，单线执行
	if (ota_info.status == FNODE_OTA_STATUS_IDLE)
	{
		node->ota_in_flag = 0;

		// 1.统计节点bitmap
		fnode_desc(node, desc);

		// 2.OTA_START
		if (fnode_task_OTA_START_handler(node) != 0) {
			ota_info.status = FNODE_OTA_STATUS_ERROR;
			(void)fnode_ota_report_result(0);
			return -1;
		}

		ota_info.status = FNODE_OTA_STATUS_START;
		node->ota_in_flag = 1;
		ota_info.node_ack_ota_package_bitmap = 0;

		// 启动定时器等待OTA START响应
		wos_timer_countdown_ms(&node->ota_timer, FNODE_OTA_START_WAIT_TIME);

		// 等待OTA_START ACK定时器超时
		while (!wos_timer_is_expired(&node->ota_timer))
		{
			usleep(1000);

			// 等到所有的节点ACK
			if (ota_info.node_ack_ota_start_bitmap == ota_info.node_ota_bitmap)
			{
				break;
			}
		}

		// 更新参与OTA的节点统计信息s
		printf("OTA -- get [OTA START] ack: 0x%X, 0x%X\r\n",
			   ota_info.node_ack_ota_start_bitmap, ota_info.node_ota_bitmap);
		ota_info.node_ota_current_bitmap = ota_info.node_ack_ota_start_bitmap;

		if (ota_info.node_ota_current_bitmap == 0)
		{
			ota_info.status = FNODE_OTA_STATUS_ERROR;

			return -1;
		}

		// 3.OTA_PACKAGE
		ota_info.status = FNODE_OTA_STATUS_PACKAGE;
		ota_info.node_ota_package_handling = 1;
		// 更新OTA发送固件长度、下一包固件偏移位置
		ota_info.firmware_bin_tx_len = 0;
		ota_info.firmware_bin_offset = 0;
		ota_info.firmware_bin_package_len = 0;

		// 打开文件（只读、二进制模式）
		int fd = open(ota_info.bin_path, O_RDONLY);
		if (fd < 0)
		{
			perror("OTA -- open bin file failed");
			ota_info.status = FNODE_OTA_STATUS_ERROR;
			ota_info.node_ota_package_handling = 0;

			return -1;
		}

		// 分块读取固件并发送
		while (1 == ota_info.node_ota_package_handling)
		{
			// printf("OTA PACKAGE -- from %d, sent %d\r\n",
			//	ota_info.firmware_bin_offset,
			//	ota_info.firmware_bin_tx_len);

			// 更新本次固件包ACK bitmap为空
			ota_info.node_ack_ota_package_bitmap = 0;

			// 下发固件包
			int ret = fnode_task_OTA_PACKAGE_handler(fd, node, desc);
			if (-1 == ret)
			{
				perror("OTA -- package failed");
				ota_info.status = FNODE_OTA_STATUS_ERROR;
				ota_info.node_ota_package_handling = 0; // 异常结束

				close(fd);
				(void)fnode_ota_report_result(0);

				return -1;
			}

			if (1 == ret) // 最后一个
			{
				close(fd);
				ota_info.node_ota_package_handling = 0; // 正常结束
				break;
			}

			// 启动定时器等待每包OTA PACKAGE响应
			wos_timer_countdown_ms(&node->ota_timer, FNODE_OTA_PACKAGE_WAIT_TIME);

			// 等待OTA_START ACK定时器超时
			while (!wos_timer_is_expired(&node->ota_timer))
			{
				usleep(1000);

				// 等到所有的节点ACK
				if (ota_info.node_ota_current_bitmap == ota_info.node_ack_ota_package_bitmap)
				{
					break;
				}
			}

			if (ota_info.node_ota_current_bitmap != ota_info.node_ack_ota_package_bitmap)
			{ // 丢失响应情况
				WLOGW("OTA PACKAGE timeout ack=0x%X/0x%X; retry 1/1\r\n",
				      ota_info.node_ack_ota_package_bitmap,
				      ota_info.node_ota_current_bitmap);

				if (fnode_task_OTA_PACKAGE_resend_handler(node) != 0) {
					close(fd);
					ota_info.status = FNODE_OTA_STATUS_ERROR;
					ota_info.node_ota_package_handling = 0;
					(void)fnode_ota_report_result(0);
					return -1;
				}

				wos_timer_countdown_ms(&node->ota_timer,
						       FNODE_OTA_PACKAGE_WAIT_TIME);
				while (!wos_timer_is_expired(&node->ota_timer)) {
					usleep(1000);
					if (ota_info.node_ota_current_bitmap ==
					    ota_info.node_ack_ota_package_bitmap)
						break;
				}
			}

			if (ota_info.node_ota_current_bitmap !=
			    ota_info.node_ack_ota_package_bitmap)
			{
				WLOGW("OTA PACKAGE dropping nodes: active=0x%X ack=0x%X\r\n",
				      ota_info.node_ota_current_bitmap,
				      ota_info.node_ack_ota_package_bitmap);

				// 更新OTA参与节点信息
				ota_info.node_ota_current_bitmap &=
					ota_info.node_ack_ota_package_bitmap;
			}

			if (ota_info.node_ota_current_bitmap == 0)
			{
				ota_info.status = FNODE_OTA_STATUS_ERROR;
				close(fd);
				ota_info.node_ota_package_handling = 0;
				(void)fnode_ota_report_result(0);

				return -1;
			}

			(void)fnode_ota_report_progress(
				ota_info.node_ota_current_bitmap);

			if (ota_info.firmware_bin_tx_len >= ota_info.firmware_bin_size) // 当前为最后一包
			{
				WLOGI("OTA -- package last\r\n");
				close(fd);
				ota_info.node_ota_package_handling = 0; // 正常结束
			}
		}

		// 4.OTA_FINISH
		ota_info.node_ack_ota_finish_bitmap = 0;
		ota_info.status = FNODE_OTA_STATUS_FINISH;
		if (fnode_task_OTA_FINISH_handler(node, desc) != 0) {
			ota_info.status = FNODE_OTA_STATUS_ERROR;
			(void)fnode_ota_report_result(0);
			return -1;
		}

		// 启动定时器等待每包OTA FINISH响应
		wos_timer_countdown_ms(&node->ota_timer, FNODE_OTA_FINISH_WAIT_TIME);

		// 等待OTA_START FINISH定时器超时
		while (!wos_timer_is_expired(&node->ota_timer))
		{
			usleep(1000);

			// 等到所有的节点ACK
			if (ota_info.node_ota_current_bitmap == ota_info.node_ack_ota_finish_bitmap)
			{
				break;
			}
		}

		printf("OTA -- ota finish: 0x%X\r\n", ota_info.node_ack_ota_finish_bitmap);

		(void)fnode_ota_report_result(
			ota_info.node_ack_ota_finish_bitmap);

		// 清除OTA标志
		node->ota_start_flag = 0;

		// 启动扫描
		node->reset = 1;
		usleep(1000 * 200);
		ota_info.status = FNODE_OTA_STATUS_GONE;
	}

	return -1;
}
#endif /* !FNIRS_EV_IO */

#ifndef FNIRS_EV_IO
/********************************************************************************************
 *  NODE任务状态机 -- 采样过程处理
 *
 ********************************************************************************************/
static void *fnode_task_SAMPLE_handler(fnode_t *node)
{
	uint8_t addr = FNODE_ADDR_RESET;

	// 1.节点关闭LED
	addr = FNODE_ADDR_RESET;
	fnode_can_write(node, &addr, 1);

// 2.=== 内部循环采样 ===
// fnode_sample_handler(node);
	fnode_10HZ_sample_and_upload_handler(node);

	// 3.结束采样循环
	fnode_snd_event(node, FNODE_EVT_SP_STOP, 0);

	// 采样停止指示
	led_sample_nodes(0);

	// 4.节点关闭LED
	addr = FNODE_ADDR_RESET;
	fnode_can_write(node, &addr, 1);

	fnode_snd_event(node, FNODE_EVT_SP_RESET, 0);

	// 5.重新启动扫描节点过程
	node->state = FHUB_STATE_RESET;
	wos_timer_countdown_ms(&node->timer, FNODE_SCAN_RESCAN_MS);
}
#endif /* !FNIRS_EV_IO */

#ifdef FNIRS_EV_IO
#include "ev_can.h"

#define FNODE_EV_SP_OFF        0
#define FNODE_EV_SP_CYCLE      1
#define FNODE_EV_SP_LED_SETUP  2
#define FNODE_EV_SP_WAIT_LED   3
#define FNODE_EV_SP_LED_SAMPLE 4
#define FNODE_EV_SP_WAIT_ADC   5
#define FNODE_EV_SP_WAIT_UPLOAD 6
#define FNODE_EV_SP_SAVEDATA   7
#define FNODE_EV_SP_LED_ON_US  3000
#define FNODE_EV_SP_ADC_WAIT_US 200
#define FNODE_EV_SP_LAST_ACK_WAIT_US 4000

static const sample_node_info_t *fnode_ev_sp_array(const fnode_t *node)
{
	if (node->array_from_host_is_used)
		return node->sample_node_array_from_host;

	return sample_node_array;
}

static void fnode_ev_sample_finish(fnode_t *node)
{
	uint8_t addr = FNODE_ADDR_RESET;

	fnode_snd_event(node, FNODE_EVT_SP_STOP, 0);
	led_sample_nodes(0);
	fnode_can_write(node, &addr, 1);
	fnode_snd_event(node, FNODE_EVT_SP_RESET, 0);
	node->ev_sp_phase = FNODE_EV_SP_OFF;
	ev_can_sample_disarm();
	WLOGI("fnode_ev_sample_finish\r\n");

	if (node->ota_start_flag) {
		node->ota_start_flag = 0;
		fnode_ev_ota_enter(node);
		return;
	}

	node->state = FHUB_STATE_RESET;
}

void fnode_ev_sample_begin(fnode_handler_t fh)
{
	fnode_t *node = (fnode_t *)fh;
	uint8_t addr = FNODE_ADDR_RESET;
	const sample_node_info_t *current_array;
	uint32_t array_size;
	uint8_t i;

	if (!node || node->state != FHUB_STATE_SAMPLE)
		return;

	fnode_can_write(node, &addr, 1);

	if (node->array_from_host_is_used) {
		current_array = node->sample_node_array_from_host;
		array_size = node->led_num_from_host;
		node->sample_peroid_ms =
			(FNODE_EV_SP_LED_ON_US * array_size * 2 + 15000) / 1000;
	} else {
		current_array = sample_node_array;
		array_size = sizeof(sample_node_array) / sizeof(sample_node_array[0]);
		node->sample_peroid_ms = 90;
	}

	fdatalog_write_led_array((const uint8_t *)current_array, (uint8_t)array_size,
		node->array_from_host_is_used ? 1 : 0);

	fnode_desc(node, node->desc);
	fhub_start(node->hub, node->desc);
	node->ev_sp_gain_735 = node->gain_735;
	node->ev_sp_gain_850 = node->gain_850;
	node->ev_sp_array_size = array_size;
	node->ev_sp_fnum = 0;
	node->ev_sp_alive = 0;

	for (i = 0; i < FNODE_NID_MAX; i++) {
		if (node->desc[i].alive)
			node->ev_sp_alive++;
	}

	memset((void *)node->poll_frame_cnt, 0, sizeof(node->poll_frame_cnt));
	node->ev_sp_phase = FNODE_EV_SP_CYCLE;
	WLOGI("fnode_ev_sample_begin: upload mode (%u alive, %u LEDs, %u ms)\r\n",
	      node->ev_sp_alive, node->ev_sp_array_size,
	      node->sample_peroid_ms);

	ev_can_sample_arm_deferred();
}

int fnode_ev_sample_active(fnode_handler_t fh)
{
	fnode_t *node = (fnode_t *)fh;

	if (!node)
		return 0;

	return node->ev_sp_phase != FNODE_EV_SP_OFF;
}

uint32_t fnode_ev_sample_next_delay_us(fnode_handler_t fh)
{
	fnode_t *node = (fnode_t *)fh;
	uint64_t now_us;
	uint64_t delay_us;

	if (!node || node->ev_sp_phase == FNODE_EV_SP_OFF)
		return 0;

	switch (node->ev_sp_phase) {
	case FNODE_EV_SP_WAIT_LED:
	case FNODE_EV_SP_WAIT_ADC:
	case FNODE_EV_SP_WAIT_UPLOAD:
		now_us = wos_system_clock_us();
		if (now_us >= node->ev_sp_deadline_us)
			return 1;
		delay_us = node->ev_sp_deadline_us - now_us;
		return delay_us > UINT32_MAX ? UINT32_MAX : (uint32_t)delay_us;

	default:
		return 1;
	}
}

void fnode_ev_sample_tick(fnode_handler_t fh)
{
	fnode_t *node = (fnode_t *)fh;
	const sample_node_info_t *current_array;
	uint8_t buf[64];
	uint64_t now_us;
	uint8_t nid;
	uint8_t led_id;
	uint8_t chn;
	uint32_t i;
	uint32_t j;

	if (!node || node->ev_sp_phase == FNODE_EV_SP_OFF)
		return;

	if (!node->sample) {
		fnode_ev_sample_finish(node);
		return;
	}

	now_us = wos_system_clock_us();
	current_array = fnode_ev_sp_array(node);

process_phase:
	switch (node->ev_sp_phase) {
	case FNODE_EV_SP_CYCLE:
		led_sample_nodes(1);
		node->ev_sp_i = 0;
		node->ev_sp_j = 0;
		node->ev_sp_cycle_deadline_us = now_us +
			(uint64_t)node->sample_peroid_ms * 1000;
		node->ev_sp_phase = FNODE_EV_SP_LED_SETUP;
		break;

	case FNODE_EV_SP_LED_SETUP:
		for (i = node->ev_sp_i; i < node->ev_sp_array_size; i++) {
			nid = current_array[i].node_id - 1;
			led_id = current_array[i].node_led_id - 1;

			if (!node->desc[nid].alive) {
				node->ev_sp_i = i + 1;
				node->ev_sp_j = 0;
				continue;
			}

			for (j = node->ev_sp_j; j < 2; j++) {
				chn = FNODE_CHN(nid + 1, 2 * led_id + j);

				buf[0] = FNODE_ADDR_SETUP_AND_UPLOAD;
				buf[1] = node->ev_sp_fnum;
				buf[2] = chn;
				if (node->array_from_host_is_used) {
					buf[3] = (j == 0) ? current_array[i].power_735 :
						current_array[i].power_850;
				} else {
					buf[3] = (j == 0) ? node->ev_sp_gain_735 :
						node->ev_sp_gain_850;
				}
				fnode_can_write(node, buf, 4);

				node->ev_sp_i = i;
				node->ev_sp_j = j;
				node->ev_sp_deadline_us =
					now_us + FNODE_EV_SP_LED_ON_US;
				node->ev_sp_phase = FNODE_EV_SP_WAIT_LED;
				return;
			}

			node->ev_sp_j = 0;
		}

		buf[0] = FNODE_ADDR_DATA_LAST_AND_UPLOAD;
		fnode_can_write(node, buf, 1);
		led_sample_nodes(0);
		node->ev_sp_deadline_us =
			now_us + FNODE_EV_SP_LAST_ACK_WAIT_US;
		if (node->ev_sp_deadline_us <
		    node->ev_sp_cycle_deadline_us)
			node->ev_sp_deadline_us =
				node->ev_sp_cycle_deadline_us;
		node->ev_sp_phase = FNODE_EV_SP_WAIT_UPLOAD;
		return;

	case FNODE_EV_SP_WAIT_LED:
		if (now_us < node->ev_sp_deadline_us)
			return;

		i = node->ev_sp_i;
		j = node->ev_sp_j;
		nid = current_array[i].node_id - 1;
		led_id = current_array[i].node_led_id - 1;
		chn = FNODE_CHN(nid + 1, 2 * led_id + j);

		buf[0] = FNODE_ADDR_SAMPLE_AND_UPLOAD;
		buf[1] = node->ev_sp_fnum;
		buf[2] = chn;
		fnode_can_write(node, buf, 3);

		node->ev_sp_deadline_us = now_us + FNODE_EV_SP_ADC_WAIT_US;
		node->ev_sp_phase = FNODE_EV_SP_WAIT_ADC;
		break;

	case FNODE_EV_SP_WAIT_ADC:
		if (now_us < node->ev_sp_deadline_us)
			return;

		node->ev_sp_j++;
		if (node->ev_sp_j >= 2) {
			node->ev_sp_j = 0;
			node->ev_sp_i++;
		}
		node->ev_sp_phase = FNODE_EV_SP_LED_SETUP;
		/* The ADC wait has elapsed; send the next LED setup in this tick. */
		goto process_phase;

	case FNODE_EV_SP_WAIT_UPLOAD:
		if (now_us < node->ev_sp_deadline_us)
			return;
		node->ev_sp_phase = FNODE_EV_SP_SAVEDATA;
		goto process_phase;

	case FNODE_EV_SP_SAVEDATA:
		fnode_snd_event(node, FNODE_EVT_SP_SAVEDATA, node->ev_sp_fnum);
		/*
		 * Finish aggregation before the next LED is configured.  This keeps
		 * the once-per-frame work out of the following LED's 3ms window.
		 */
		fnode_ev_hub_service(node);
		led_sample_nodes(0);
		node->ev_sp_fnum++;

		if (!node->sample) {
			fnode_ev_sample_finish(node);
			return;
		}

		node->ev_sp_phase = FNODE_EV_SP_CYCLE;
		break;

	default:
		node->ev_sp_phase = FNODE_EV_SP_OFF;
		ev_can_sample_disarm();
		break;
	}
}

#define FNODE_EV_FT_OFF           0
#define FNODE_EV_FT_RUN           1
#define FNODE_EV_FT_MS_MIN        7
#define FNODE_EV_FT_ALT_MS_MIN    19
#define FNODE_EV_FT_CALI_WAIT_MS  10000

static void fnode_ev_factory_disarm(fnode_t *node)
{
	if (!node)
		return;

	node->ev_ft_phase = FNODE_EV_FT_OFF;
	node->ev_ft_substep = 0;
	ev_can_factory_disarm();
}

static uint32_t fnode_ev_factory_delay_ms(uint32_t min_ms)
{
	if (factory_test.sample_interval > min_ms)
		return factory_test.sample_interval;
	return min_ms;
}

static void fnode_ev_factory_measure_done(fnode_t *node, uint8_t *buf, const char *tag)
{
	printf("Finish work -- %s\r\n", tag);
	factory_test.status = FNODE_FACTORY_STATUS_ENTER;
	buf[0] = FNODE_ADDR_RESET;
	fnode_can_write(node, buf, 1);
	led_sample_nodes(0);
	fnode_ev_factory_disarm(node);
}

static void fnode_ev_factory_tick_measure(fnode_t *node, uint8_t *buf, uint64_t now)
{
	uint32_t sid;
	uint8_t chn_tx;
	uint8_t chn_rx;
	uint32_t gap_ms;
	int j;

	sid = (factory_test.led_id - 1) * 2 + (factory_test.led_735_850 - 1);
	chn_tx = FNODE_CHN(factory_test.nid_tx, sid);
	chn_rx = FNODE_CHN(factory_test.nid_rx, factory_test.rx_pd - 1);
	gap_ms = fnode_ev_factory_delay_ms(FNODE_EV_FT_MS_MIN);

	switch (node->ev_ft_substep) {
	case 0:
		led_sample_nodes(1);
		node->ev_ft_index = 0;
		node->ev_ft_substep = 1;
		break;

	case 1:
		node->ev_ft_index++;
		buf[0] = FNODE_ADDR_SETUP;
		buf[1] = node->ev_ft_seq;
		buf[2] = chn_tx;
		buf[3] = factory_test.tx_ma;
		fnode_can_write(node, buf, 4);
		node->ev_ft_deadline_ms = now + 3;
		node->ev_ft_substep = 2;
		break;

	case 2:
		if (now < node->ev_ft_deadline_ms)
			return;

		j = 0;
		buf[j++] = FNODE_ADDR_SAMPLE_and_REPORT;
		buf[j++] = node->ev_ft_seq;
		buf[j++] = chn_tx;
		buf[j++] = chn_rx;
		buf[j++] = (node->ev_ft_index >> 24) & 0xFF;
		buf[j++] = (node->ev_ft_index >> 16) & 0xFF;
		buf[j++] = (node->ev_ft_index >> 8) & 0xFF;
		buf[j++] = node->ev_ft_index & 0xFF;
		fnode_can_write(node, buf, j);
		node->ev_ft_deadline_ms = now + 3;
		node->ev_ft_substep = 3;
		break;

	case 3:
		if (now < node->ev_ft_deadline_ms)
			return;

		buf[0] = FNODE_ADDR_RESET;
		fnode_can_write(node, buf, 1);
		node->ev_ft_deadline_ms = now + (gap_ms > 6 ? gap_ms - 6 : 0);
		node->ev_ft_substep = 4;
		break;

	case 4:
		if (now < node->ev_ft_deadline_ms)
			return;

		if (node->ev_ft_index < factory_test.sample_count) {
			node->ev_ft_substep = 1;
			return;
		}

		fnode_ev_factory_measure_done(node, buf, "measure");
		break;

	default:
		fnode_ev_factory_disarm(node);
		break;
	}
}

static void fnode_ev_factory_tick_measure_alternate(fnode_t *node, uint8_t *buf, uint64_t now)
{
	uint32_t sid_735;
	uint32_t sid_850;
	uint8_t chn_tx_735;
	uint8_t chn_tx_850;
	uint8_t chn_rx;
	uint32_t gap_ms;
	int j;

	sid_735 = (factory_test.led_id - 1) * 2;
	sid_850 = (factory_test.led_id - 1) * 2 + 1;
	chn_tx_735 = FNODE_CHN(factory_test.nid_tx, sid_735);
	chn_tx_850 = FNODE_CHN(factory_test.nid_tx, sid_850);
	chn_rx = FNODE_CHN(factory_test.nid_rx, factory_test.rx_pd - 1);
	gap_ms = fnode_ev_factory_delay_ms(FNODE_EV_FT_ALT_MS_MIN);

	switch (node->ev_ft_substep) {
	case 0:
		led_sample_nodes(1);
		node->ev_ft_index = 0;
		node->ev_ft_substep = 1;
		break;

	case 1:
		node->ev_ft_index++;
		buf[0] = FNODE_ADDR_SETUP;
		buf[1] = node->ev_ft_seq;
		buf[2] = chn_tx_735;
		buf[3] = factory_test.tx_ma;
		fnode_can_write(node, buf, 4);
		node->ev_ft_deadline_ms = now + 3;
		node->ev_ft_substep = 2;
		break;

	case 2:
		if (now < node->ev_ft_deadline_ms)
			return;

		j = 0;
		buf[j++] = FNODE_ADDR_SAMPLE_and_REPORT;
		buf[j++] = node->ev_ft_seq;
		buf[j++] = chn_tx_735;
		buf[j++] = chn_rx;
		buf[j++] = (node->ev_ft_index >> 24) & 0xFF;
		buf[j++] = (node->ev_ft_index >> 16) & 0xFF;
		buf[j++] = (node->ev_ft_index >> 8) & 0xFF;
		buf[j++] = node->ev_ft_index & 0xFF;
		fnode_can_write(node, buf, j);
		node->ev_ft_deadline_ms = now + 9;
		node->ev_ft_substep = 3;
		break;

	case 3:
		if (now < node->ev_ft_deadline_ms)
			return;

		buf[0] = FNODE_ADDR_SETUP;
		buf[1] = node->ev_ft_seq;
		buf[2] = chn_tx_850;
		buf[3] = factory_test.tx_ma;
		fnode_can_write(node, buf, 4);
		node->ev_ft_deadline_ms = now + 3;
		node->ev_ft_substep = 4;
		break;

	case 4:
		if (now < node->ev_ft_deadline_ms)
			return;

		j = 0;
		buf[j++] = FNODE_ADDR_SAMPLE_and_REPORT;
		buf[j++] = node->ev_ft_seq;
		buf[j++] = chn_tx_850;
		buf[j++] = chn_rx;
		buf[j++] = (node->ev_ft_index >> 24) & 0xFF;
		buf[j++] = (node->ev_ft_index >> 16) & 0xFF;
		buf[j++] = (node->ev_ft_index >> 8) & 0xFF;
		buf[j++] = node->ev_ft_index & 0xFF;
		fnode_can_write(node, buf, j);
		node->ev_ft_deadline_ms = now + 3;
		node->ev_ft_substep = 5;
		break;

	case 5:
		if (now < node->ev_ft_deadline_ms)
			return;

		buf[0] = FNODE_ADDR_RESET;
		fnode_can_write(node, buf, 1);
		node->ev_ft_deadline_ms = now + (gap_ms > 18 ? gap_ms - 18 : 0);
		node->ev_ft_substep = 6;
		break;

	case 6:
		if (now < node->ev_ft_deadline_ms)
			return;

		if (node->ev_ft_index < factory_test.sample_count) {
			node->ev_ft_substep = 1;
			return;
		}

		fnode_ev_factory_measure_done(node, buf, "measure alternate");
		break;

	default:
		fnode_ev_factory_disarm(node);
		break;
	}
}

static void fnode_ev_factory_tick_led_on_measure(fnode_t *node, uint8_t *buf, uint64_t now)
{
	uint32_t sid;
	uint8_t chn_tx;
	uint8_t chn_rx;
	uint32_t gap_ms;
	int j;

	sid = (factory_test.led_id - 1) * 2 + (factory_test.led_735_850 - 1);
	chn_tx = FNODE_CHN(factory_test.nid_tx, sid);
	chn_rx = FNODE_CHN(factory_test.nid_rx, factory_test.rx_pd - 1);
	gap_ms = fnode_ev_factory_delay_ms(FNODE_EV_FT_MS_MIN);

	switch (node->ev_ft_substep) {
	case 0:
		led_sample_nodes(1);
		buf[0] = FNODE_ADDR_SETUP;
		buf[1] = node->ev_ft_seq;
		buf[2] = chn_tx;
		buf[3] = factory_test.tx_ma;
		fnode_can_write(node, buf, 4);
		node->ev_ft_index = 0;
		node->ev_ft_deadline_ms = now + gap_ms;
		node->ev_ft_substep = 1;
		break;

	case 1:
		if (now < node->ev_ft_deadline_ms)
			return;

		node->ev_ft_index++;
		j = 0;
		buf[j++] = FNODE_ADDR_SAMPLE_and_REPORT;
		buf[j++] = node->ev_ft_seq;
		buf[j++] = chn_tx;
		buf[j++] = chn_rx;
		buf[j++] = (node->ev_ft_index >> 24) & 0xFF;
		buf[j++] = (node->ev_ft_index >> 16) & 0xFF;
		buf[j++] = (node->ev_ft_index >> 8) & 0xFF;
		buf[j++] = node->ev_ft_index & 0xFF;
		fnode_can_write(node, buf, j);
		node->ev_ft_deadline_ms = now + gap_ms;
		node->ev_ft_substep = 2;
		break;

	case 2:
		if (now < node->ev_ft_deadline_ms)
			return;

		if (node->ev_ft_index < factory_test.sample_count) {
			node->ev_ft_substep = 1;
			return;
		}

		fnode_ev_factory_measure_done(node, buf, "measure");
		break;

	default:
		fnode_ev_factory_disarm(node);
		break;
	}
}

static void fnode_ev_factory_tick_cali_pd(fnode_t *node, uint8_t *buf, uint64_t now)
{
	switch (node->ev_ft_substep) {
	case 0:
		printf("CALI NODE PD - %d\r\n", factory_test.cali_node_id);
		buf[0] = FNODE_ADDR_CALI_PD_and_REPORT;
		buf[1] = node->ev_ft_seq;
		buf[2] = factory_test.cali_node_id;
		fnode_can_write(node, buf, 3);
		node->ev_ft_deadline_ms = now + FNODE_EV_FT_CALI_WAIT_MS;
		node->ev_ft_substep = 1;
		break;

	case 1:
		if (now < node->ev_ft_deadline_ms)
			return;

		printf("Finish work -- cali pd!\r\n");
		factory_test.status = FNODE_FACTORY_STATUS_ENTER;
		fnode_ev_factory_disarm(node);
		break;

	default:
		fnode_ev_factory_disarm(node);
		break;
	}
}

static void fnode_ev_factory_tick_led_on(fnode_t *node, uint8_t *buf)
{
	uint32_t sid;
	uint8_t chn_tx;

	sid = (factory_test.led_id - 1) * 2 + (factory_test.led_735_850 - 1);
	chn_tx = FNODE_CHN(factory_test.nid_tx, sid);
	printf("LED - [%d, %d]\r\n", factory_test.nid_tx, sid);

	switch (factory_test.onoff_value) {
	case 0:
		buf[0] = FNODE_ADDR_SETUP;
		buf[1] = node->ev_ft_seq;
		buf[2] = chn_tx;
		buf[3] = 0;
		fnode_can_write(node, buf, 4);
		printf("Finish work -- LED OFF!\r\n");
		break;

	case 1:
		node->ev_ft_index++;
		buf[0] = FNODE_ADDR_SETUP;
		buf[1] = node->ev_ft_seq;
		buf[2] = chn_tx;
		buf[3] = factory_test.tx_ma;
		fnode_can_write(node, buf, 4);
		printf("Finish work -- LED ON!\r\n");
		break;

	case 0xFF:
		buf[0] = FNODE_ADDR_RESET;
		fnode_can_write(node, buf, 1);
		printf("Finish work -- 1 node LED ALL OFF!\r\n");
		break;

	default:
		break;
	}

	factory_test.status = FNODE_FACTORY_STATUS_ENTER;
	fnode_ev_factory_disarm(node);
}

void fnode_ev_factory_tick(fnode_handler_t fh)
{
	fnode_t *node = (fnode_t *)fh;
	uint8_t buf[64];
	uint64_t now;

	if (!node || node->ev_ft_phase != FNODE_EV_FT_RUN)
		return;

	now = wos_system_clock_ms();
	memset(buf, 0, sizeof(buf));

	switch (factory_test.status) {
	case FNODE_FACTORY_STATUS_FUSB_MEASURE:
		fnode_ev_factory_tick_measure(node, buf, now);
		break;

	case FNODE_FACTORY_STATUS_FUSB_MEASURE_ALTERNATE:
		fnode_ev_factory_tick_measure_alternate(node, buf, now);
		break;

	case FNODE_FACTORY_STATUS_FUSB_LED_ON_MEASURE_LED_OFF:
		fnode_ev_factory_tick_led_on_measure(node, buf, now);
		break;

	case FNODE_FACTORY_STATUS_FUSB_CALI_PD_OFFSET:
		fnode_ev_factory_tick_cali_pd(node, buf, now);
		break;

	case FNODE_FACTORY_STATUS_FUSB_LED_ON:
		if (node->ev_ft_substep == 0) {
			node->ev_ft_substep = 1;
			fnode_ev_factory_tick_led_on(node, buf);
		}
		break;

	default:
		fnode_ev_factory_disarm(node);
		break;
	}
}

int fnode_ev_factory_active(fnode_handler_t fh)
{
	fnode_t *node = (fnode_t *)fh;

	if (!node)
		return 0;

	return node->ev_ft_phase != FNODE_EV_FT_OFF;
}

static void fnode_ev_factory_begin(fnode_t *node)
{
	if (!node || node->ev_ft_phase != FNODE_EV_FT_OFF)
		return;

	node->ev_ft_index = 0;
	node->ev_ft_seq = 0;
	node->ev_ft_substep = 0;
	node->ev_ft_phase = FNODE_EV_FT_RUN;
	WLOGI("fnode_ev_factory_begin: status=%u\r\n", factory_test.status);
	ev_can_factory_arm_deferred();
}

void fnode_ev_factory_kick(fnode_handler_t fh)
{
	fnode_t *node = (fnode_t *)fh;

	if (!node)
		return;

	if (factory_test.status == FNODE_FACTORY_STATUS_ENTER ||
	    factory_test.status == FNODE_FACTORY_STATUS_IDLE ||
	    factory_test.status == FNODE_FACTORY_STATUS_ERROR)
		return;

	if (node->ev_ft_phase != FNODE_EV_FT_OFF)
		return;

	fnode_ev_factory_begin(node);
}

void fnode_ev_factory_stop(fnode_handler_t fh)
{
	fnode_t *node = (fnode_t *)fh;

	if (!node)
		return;

	led_sample_nodes(0);
	fnode_ev_factory_disarm(node);
}

/* OTA entry waits for a few IDLE handler ticks (see fnode_task_IDLE_handler). */
static int g_fnode_ota_scan_count;
#endif

/********************************************************************************************
 *  NODE任务状态机 -- 空闲状态下的处理，负责状态机切换
 *
 ********************************************************************************************/
static int fnode_task_IDLE_handler(fnode_t *node)
{
#ifndef FNIRS_EV_IO
	static int scan_count = 0;
	uint8_t addr = FNODE_ADDR_SCAN;
#endif
	fnode_desc_t desc[FNODE_NID_MAX];

	// 需要进行采样，则切换状态机
	if (node->sample)
	{
#ifndef FNIRS_EV_IO
		scan_count = 0;
#else
		g_fnode_ota_scan_count = 0;
#endif

		node->state = FHUB_STATE_SAMPLE;
		node->node_num = fnode_num(node);

		// 重新计算上报数据量
#if 1
		fnode_desc(node, desc);

		// 发光LED数量
		int sample_led_count = 0;

		for (uint32_t i = 0; i < sizeof(sample_node_array) / sizeof(sample_node_array[0]); i++)
		{
			uint8_t nid = sample_node_array[i].node_id - 1; // 节点位置[1, 12]

			if (1 == desc[nid].alive)
			{
				printf("*** nid = %d ***\r\n", nid);
				sample_led_count += 2;
			}
		}
		printf("*** sample_led_count = %d ***\r\n", sample_led_count); // 22

		// 每个节点需要上报的帧数
		int canfd_pack_count = (sample_led_count + FNODE_DATA_BLOCK_MAX - 1) / FNODE_DATA_BLOCK_MAX;
		printf("*** canfd_pack_count = %d ***\r\n", canfd_pack_count); // 8

		// 一个周期所有节点上报的总帧数S
		node->node_sp = canfd_pack_count * node->node_num;
		printf("*** node->node_sp = %d ***\r\n", node->node_sp); // 96

#else
		node->node_sp = node->node_num * FNODE_SRCID_MAX / FNODE_DATA_BLOCK_MAX * node->node_num;
#endif

		fnode_snd_event(node, FNODE_EVT_SP_START, 0); // 开始采样

#ifndef FNIRS_EV_IO
		sleep(1); // delay 1sec
#else
		fnode_ev_sample_begin(node);
#endif

		return -1;
	}

	// 需要对节点进行OTA升级，则切换状态机
	if (node->ota_start_flag)
	{
#ifndef FNIRS_EV_IO
		scan_count++;

		if (scan_count > 2)
		{
			scan_count = 0;
#else
		g_fnode_ota_scan_count++;

		if (g_fnode_ota_scan_count > 2)
		{
			g_fnode_ota_scan_count = 0;
#endif

			WLOGW("*** FHUB_STATE_OTA ***\r\n");

			ota_info.status = FNODE_OTA_STATUS_IDLE;
			node->state = FHUB_STATE_OTA;

#ifdef FNIRS_EV_IO
			fnode_ev_ota_begin(node);
#endif
			return -1;
		}
	}
	else
	{
#ifndef FNIRS_EV_IO
		scan_count = 0;
#else
		g_fnode_ota_scan_count = 0;
#endif

		// 需要进入工厂测试模式，则切换状态机
		if (node->factory_mode_flag)
		{
			WLOGW("*** FUSB_FACTORY_MODE ***\r\n");

			// factory_test.status = FNODE_FACTORY_STATUS_ENTER;
			node->state = FHUB_STATE_FACTORY_TEST;

#ifdef FNIRS_EV_IO
			fnode_ev_factory_kick(node);
#endif
			return -1;
		}
	}

#ifndef FNIRS_EV_IO
	// WLOGW("[FNODE_ADDR_SCAN] -- 1s\r\n");

	// 扫描LED指示
	led_scan_nodes();

	addr = FNODE_ADDR_SCAN;
	fnode_can_write(node, &addr, 1);
	wos_timer_countdown_ms(&node->timer, FNODE_SCAN_PEROID);
#endif

	return 0;
}

#ifdef FNIRS_EV_IO
static void fnode_ev_apply_reset(fnode_t *node)
{
	if (!node || !node->reset)
		return;

	fnode_snd_event(node, FNODE_EVT_SP_RESET, 0);
	node->state = FHUB_STATE_RESET;
	node->reset = 0;
	g_fnode_ota_scan_count = 0;
	fnode_ev_hub_service(node);
}

void fnode_ev_ble_scan_pulse(fnode_handler_t fh)
{
	fnode_t *node = (fnode_t *)fh;
	uint8_t addr = FNODE_ADDR_SCAN;

	if (!node || !node->run)
		return;

	fnode_ev_hub_service(node);

	if (node->state != FHUB_STATE_RESET)
		return;

	fnode_can_write(node, &addr, 1);
}

void fnode_ev_led_scan_tick(fnode_handler_t fh)
{
	fnode_t *node = (fnode_t *)fh;

	if (!node || !node->run || node->state != FHUB_STATE_RESET)
		return;

	led_scan_nodes();
}

void fnode_ev_idle_service(fnode_handler_t fh)
{
	fnode_t *node = (fnode_t *)fh;
	uint8_t addr = FNODE_ADDR_SCAN;

	if (!node || !node->run)
		return;

	fnode_ev_apply_reset(node);

	if (node->state != FHUB_STATE_RESET)
		return;

	if (fnode_task_IDLE_handler(node) != 0)
		return;

	fnode_can_write(node, &addr, 1);
}

void fnode_ev_kick_idle(fnode_handler_t fh)
{
	fnode_ev_idle_service(fh);
}
#endif

#define ADC_SAMPLE_INTERVAL_US_2 (1000 * 10)

#define ADC_SAMPLE_INTERVAL_MS_MIN (1 + 6)
#define ADC_SAMPLE_ALTERNATE_INTERVAL_MS_MIN (1 + 18)

#ifndef FNIRS_EV_IO
static void fnode_task_factory_mode_FUSB_MEASURE_handler(fnode_t *node)
{
	fnode_desc_t desc[FNODE_NID_MAX];
	uint8_t buf[64] = {0};
	uint8_t gain = 0; //
	static uint8_t seq = 0;
	uint32_t sid = 0; // sid < FNODE_SRC_MAX;
	uint32_t index = 0;

	int period_ms = 1000;

	fnode_desc(node, desc);

	wos_timer_init(&node->timer); /* 强制过期 */

	while (factory_test.status == FNODE_FACTORY_STATUS_FUSB_MEASURE)
	{ //<NIDx, LED, 0/1, mA, NIDy, PD, sample_interval, sample_count> -- 单通道3厘米发射与接收
		wos_timer_countdown_ms(&node->timer, period_ms);

		//===1.执行命令===
		// LED指示
		led_sample_nodes(1);

		sid = (factory_test.led_id - 1) * 2 + (factory_test.led_735_850 - 1);

		uint8_t chn_tx = FNODE_CHN(factory_test.nid_tx, sid);
		uint8_t chn_rx = FNODE_CHN(factory_test.nid_rx, factory_test.rx_pd - 1);

		printf("[%d - %d led on] -- [%d - PD: %d], %d ms, %d\r\n",
			   factory_test.nid_tx, sid,
			   factory_test.nid_rx, factory_test.rx_pd - 1,
			   factory_test.sample_interval, factory_test.sample_count);

		uint32_t sample_delay_ms = ADC_SAMPLE_INTERVAL_MS_MIN;
		if (factory_test.sample_interval > ADC_SAMPLE_INTERVAL_MS_MIN)
			sample_delay_ms = factory_test.sample_interval;

		while (index < factory_test.sample_count)
		{
			index++; // 第几次采样次数

			// 通知某节点亮灯
			buf[0] = FNODE_ADDR_SETUP;
			buf[1] = seq;
			buf[2] = chn_tx;
			buf[3] = factory_test.tx_ma;
			fnode_can_write(node, buf, 4);

			usleep(3 * 1000); // 点亮 3 毫秒后，再采样

			int j = 0;
			// 通知某节点采样
			buf[j++] = FNODE_ADDR_SAMPLE_and_REPORT;
			buf[j++] = seq;
			buf[j++] = chn_tx; //[2]
			buf[j++] = chn_rx; //[3]

			buf[j++] = (index >> 24) & 0xFF; //[4]
			buf[j++] = (index >> 16) & 0xFF;
			buf[j++] = (index >> 8) & 0xFF;
			buf[j++] = (index) & 0xFF;
			fnode_can_write(node, buf, j);

			usleep(3 * 1000); // 下发采样命令后等待 3 毫秒[留足节点采样时间、传输时间]

			// 节点关闭LED
			buf[0] = FNODE_ADDR_RESET;
			fnode_can_write(node, buf, 1);

			usleep((sample_delay_ms - 6) * 1000);
		}

		if (index >= factory_test.sample_count)
		{
			printf("Finish work -- measure!\r\n");
			factory_test.status = FNODE_FACTORY_STATUS_ENTER;

			// 节点关闭LED
			buf[0] = FNODE_ADDR_RESET;
			fnode_can_write(node, buf, 1);
		}

		led_sample_nodes(0);
	}

	// seq++;
}

// 3cm 735nm和850nm 交替发射接受，2026.4.22 huang
static void fnode_task_factory_mode_FUSB_MEASURE_ALTERNATE_handler(fnode_t *node)
{
	fnode_desc_t desc[FNODE_NID_MAX];
	uint8_t buf[64] = {0};
	uint8_t gain = 0; //
	static uint8_t seq = 0;
	uint32_t sid_735 = 0; // sid < FNODE_SRC_MAX;
	uint32_t sid_850 = 0; // sid < FNODE_SRC_MAX;
	uint32_t index = 0;

	int period_ms = 1000;

	fnode_desc(node, desc);

	wos_timer_init(&node->timer); /* 强制过期 */

	while (factory_test.status == FNODE_FACTORY_STATUS_FUSB_MEASURE_ALTERNATE)
	{ //<NIDx, LED, mA, NIDy, PD, sample_interval, sample_count> -- 单通道3厘米 交替发射与接收
		wos_timer_countdown_ms(&node->timer, period_ms);

		//===1.执行命令===
		// LED指示
		led_sample_nodes(1);

		// 先亮735nm
		// sid = (factory_test.led_id - 1) * 2 + (factory_test.led_735_850 - 1);
		sid_735 = (factory_test.led_id - 1) * 2 + (1 - 1);
		sid_850 = (factory_test.led_id - 1) * 2 + (2 - 1);

		uint8_t chn_tx_735 = FNODE_CHN(factory_test.nid_tx, sid_735);
		uint8_t chn_tx_850 = FNODE_CHN(factory_test.nid_tx, sid_850);
		uint8_t chn_rx = FNODE_CHN(factory_test.nid_rx, factory_test.rx_pd - 1);

		printf("[%d - %d and %d led on] -- [%d - PD: %d], %d ms, %d\r\n",
			   factory_test.nid_tx, sid_735, sid_850,
			   factory_test.nid_rx, factory_test.rx_pd - 1,
			   factory_test.sample_interval, factory_test.sample_count);

		uint32_t sample_delay_ms = ADC_SAMPLE_ALTERNATE_INTERVAL_MS_MIN;
		if (factory_test.sample_interval > ADC_SAMPLE_ALTERNATE_INTERVAL_MS_MIN)
			sample_delay_ms = factory_test.sample_interval;

		while (index < factory_test.sample_count)
		{
			index++; // 第几次采样次数

			/*****  735nm 采样开始 ********/
			// 通知某节点亮灯
			buf[0] = FNODE_ADDR_SETUP;
			buf[1] = seq;
			buf[2] = chn_tx_735;
			buf[3] = factory_test.tx_ma;
			fnode_can_write(node, buf, 4);

			usleep(3 * 1000); // 点亮 3 毫秒后，再采样

			int j = 0;
			// 通知某节点采样
			buf[j++] = FNODE_ADDR_SAMPLE_and_REPORT;
			buf[j++] = seq;
			buf[j++] = chn_tx_735; //[2]
			buf[j++] = chn_rx;	   //[3]

			buf[j++] = (index >> 24) & 0xFF; //[4]
			buf[j++] = (index >> 16) & 0xFF;
			buf[j++] = (index >> 8) & 0xFF;
			buf[j++] = (index) & 0xFF;
			fnode_can_write(node, buf, j);

			usleep(9 * 1000); // 下发采样命令后等待 3 毫秒[留足节点采样时间、传输时间]

			/*****  850nm 采样开始 ********/
			// 通知某节点亮灯
			buf[0] = FNODE_ADDR_SETUP;
			buf[1] = seq;
			buf[2] = chn_tx_850;
			buf[3] = factory_test.tx_ma;
			fnode_can_write(node, buf, 4);

			usleep(3 * 1000); // 点亮 3 毫秒后，再采样

			j = 0;
			// 通知某节点采样
			buf[j++] = FNODE_ADDR_SAMPLE_and_REPORT;
			buf[j++] = seq;
			buf[j++] = chn_tx_850; //[2]
			buf[j++] = chn_rx;	   //[3]

			buf[j++] = (index >> 24) & 0xFF; //[4]
			buf[j++] = (index >> 16) & 0xFF;
			buf[j++] = (index >> 8) & 0xFF;
			buf[j++] = (index) & 0xFF;
			fnode_can_write(node, buf, j);

			usleep(3 * 1000); // 下发采样命令后等待 3 毫秒[留足节点采样时间、传输时间]

			// 节点关闭LED
			buf[0] = FNODE_ADDR_RESET;
			fnode_can_write(node, buf, 1);

			usleep((sample_delay_ms - 18) * 1000);
		}

		if (index >= factory_test.sample_count)
		{
			printf("Finish work -- measure alternate!\r\n");
			factory_test.status = FNODE_FACTORY_STATUS_ENTER;

			// 节点关闭LED
			buf[0] = FNODE_ADDR_RESET;
			fnode_can_write(node, buf, 1);
		}

		led_sample_nodes(0);
	}

	// seq++;
}

static void fnode_task_factory_mode_LED_ON_FUSB_MEASURE_handler(fnode_t *node)
{
	fnode_desc_t desc[FNODE_NID_MAX];
	uint8_t buf[64] = {0};
	uint8_t gain = 0; //
	static uint8_t seq = 0;
	uint32_t sid = 0; // sid < FNODE_SRC_MAX;
	uint32_t index = 0;

	int period_ms = 1000;

	fnode_desc(node, desc);

	wos_timer_init(&node->timer); /* 强制过期 */

	while (factory_test.status == FNODE_FACTORY_STATUS_FUSB_LED_ON_MEASURE_LED_OFF)
	{ //<NIDx, LED, 0/1, mA, NIDy, PD, sample_interval, sample_count> -- 单通道3厘米发射与接收
		wos_timer_countdown_ms(&node->timer, period_ms);

		//===1.执行命令===
		// LED指示
		led_sample_nodes(1);

		sid = (factory_test.led_id - 1) * 2 + (factory_test.led_735_850 - 1);

		uint8_t chn_tx = FNODE_CHN(factory_test.nid_tx, sid);
		uint8_t chn_rx = FNODE_CHN(factory_test.nid_rx, factory_test.rx_pd - 1);

		printf("[%d - %d led on] -- [%d - PD: %d], %d ms, %d\r\n",
			   factory_test.nid_tx, sid,
			   factory_test.nid_rx, factory_test.rx_pd - 1,
			   factory_test.sample_interval, factory_test.sample_count);

		// 通知某节点亮灯
		buf[0] = FNODE_ADDR_SETUP;
		buf[1] = seq;
		buf[2] = chn_tx;
		buf[3] = factory_test.tx_ma;
		fnode_can_write(node, buf, 4);

		uint32_t sample_delay_ms = ADC_SAMPLE_INTERVAL_MS_MIN;
		if (factory_test.sample_interval > ADC_SAMPLE_INTERVAL_MS_MIN)
			sample_delay_ms = factory_test.sample_interval;

		usleep(sample_delay_ms * 1000);

		while (index < factory_test.sample_count)
		{
			index++; // 第几次采样次数

			int j = 0;
			// 通知某节点采样
			buf[j++] = FNODE_ADDR_SAMPLE_and_REPORT;
			buf[j++] = seq;
			buf[j++] = chn_tx; //[2]
			buf[j++] = chn_rx; //[3]

			buf[j++] = (index >> 24) & 0xFF; //[4]
			buf[j++] = (index >> 16) & 0xFF;
			buf[j++] = (index >> 8) & 0xFF;
			buf[j++] = (index) & 0xFF;

			fnode_can_write(node, buf, j);

			// usleep(ADC_SAMPLE_INTERVAL_US_2);
			usleep(sample_delay_ms * 1000);
		}

		if (index >= factory_test.sample_count)
		{
			printf("Finish work -- measure!\r\n");
			factory_test.status = FNODE_FACTORY_STATUS_ENTER;

			// 节点关闭LED
			buf[0] = FNODE_ADDR_RESET;
			fnode_can_write(node, buf, 1);
		}

		led_sample_nodes(0);
	}

	// seq++;
}

#define ADC_SAMPLE_INTERVAL_US_3 (1000 * 1000 * 10)

static void fnode_task_factory_mode_FUSB_CALI_PD_handler(fnode_t *node)
{
	fnode_desc_t desc[FNODE_NID_MAX];
	uint8_t buf[64] = {0};
	uint8_t gain = 0; //
	static uint8_t seq = 0;
	uint32_t sid = 0; // sid < FNODE_SRC_MAX;
	uint32_t index = 0;

	// seq++;

	printf("CALI NODE PD - %d\r\n", factory_test.cali_node_id);

	// 通知某节点校准PD偏置
	buf[0] = FNODE_ADDR_CALI_PD_and_REPORT;
	buf[1] = seq;
	buf[2] = factory_test.cali_node_id;

	fnode_can_write(node, buf, 3);

	usleep(ADC_SAMPLE_INTERVAL_US_3);

	printf("Finish work -- cali pd!\r\n");
	factory_test.status = FNODE_FACTORY_STATUS_ENTER;
}

static void fnode_task_factory_mode_FUSB_LED_ON_handler(fnode_t *node)
{
	uint8_t buf[64] = {0};
	static uint8_t seq = 0;
	uint32_t sid = 0; // sid < FNODE_SRC_MAX;
	uint32_t index = 0;

	// seq++;

	sid = (factory_test.led_id - 1) * 2 + (factory_test.led_735_850 - 1);
	uint8_t chn_tx = FNODE_CHN(factory_test.nid_tx, sid);

	printf("LED - [%d, %d]\r\n", factory_test.nid_tx, sid);

	switch (factory_test.onoff_value)
	{
	case 0:
		// 通知某节点关闭LED
		buf[0] = FNODE_ADDR_SETUP;
		buf[1] = seq;
		buf[2] = chn_tx;
		buf[3] = 0; // 电流值为0，节点内部认为通道关断
		fnode_can_write(node, buf, 4);

		printf("Finish work -- LED OFF!\r\n");
		break;

	case 1:
		// 通知某节点亮灯
		buf[0] = FNODE_ADDR_SETUP;
		buf[1] = seq;
		buf[2] = chn_tx;
		buf[3] = factory_test.tx_ma;
		fnode_can_write(node, buf, 4);

		printf("Finish work -- LED ON!\r\n");
		break;

	case 0xFF:
		// 通知某节点关闭LED -- 复位某节点
		buf[0] = FNODE_ADDR_RESET;
		fnode_can_write(node, buf, 1);

		printf("Finish work -- 1 node LED ALL OFF!\r\n");
		break;
	}

	factory_test.status = FNODE_FACTORY_STATUS_ENTER;
}

/********************************************************************************************
 *  NODE任务状态机 -- 设备测试模式
 *
 ********************************************************************************************/
static int fnode_task_FACTORY_TEST_handler(fnode_t *node)
{
	switch (factory_test.status)
	{
	case FNODE_FACTORY_STATUS_ENTER:
		break;

	case FNODE_FACTORY_STATUS_FUSB_MEASURE: // 单通道3厘米发射与接收
		fnode_task_factory_mode_FUSB_MEASURE_handler(node);
		break;

	case FNODE_FACTORY_STATUS_FUSB_MEASURE_ALTERNATE: // 单通道3厘米 735nm和850nm交替 发射与接收
		fnode_task_factory_mode_FUSB_MEASURE_ALTERNATE_handler(node);
		break;

	case FNODE_FACTORY_STATUS_FUSB_CALI_PD_OFFSET: // 校准PD OFFSET
		fnode_task_factory_mode_FUSB_CALI_PD_handler(node);
		break;

	case FNODE_FACTORY_STATUS_FUSB_LED_ON: // 以互斥方式点亮某个LED
		fnode_task_factory_mode_FUSB_LED_ON_handler(node);
		break;

	case FNODE_FACTORY_STATUS_FUSB_LED_ON_MEASURE_LED_OFF: // LED常亮，单通道3厘米发射与接收
		fnode_task_factory_mode_LED_ON_FUSB_MEASURE_handler(node);
		break;
	}

	return 0;
}

#else /* FNIRS_EV_IO */

static int fnode_task_FACTORY_TEST_handler(fnode_t *node)
{
	fnode_ev_factory_kick(node);
	return 0;
}

#endif /* !FNIRS_EV_IO */

#ifndef FNIRS_EV_IO
/********************************************************************************************
 *  NODE任务状态机
 *		根据当前状态（空闲、采样）进行分类处理；
 *
 *
 ********************************************************************************************/
static void *fnode_task(void *args)
{
	fnode_t *node = (fnode_t *)args;

	while (node->run) // 线程主循环
	{
		// 需要复位处理
		if (node->reset)
		{
			fnode_snd_event(node, FNODE_EVT_SP_RESET, 0);

			node->state = FHUB_STATE_RESET; // 改状态为空闲
			node->reset = 0;

			wos_timer_countdown_ms(&node->timer, FNODE_SCAN_RESCAN_MS);
		}

		// 等待当前操作定时器超时
		if (!wos_timer_is_expired(&node->timer))
		{
			usleep(10000);

			continue;
		}

		// 状态机处理
		switch (node->state)
		{
		case FHUB_STATE_RESET: // 空闲状态--扫描NODE
			if (-1 == fnode_task_IDLE_handler(node))
				break;
			break;

		case FHUB_STATE_SAMPLE: // 采样状态--控制NODE进行发光和采样
			fnode_task_SAMPLE_handler(node);
			break;

		case FHUB_STATE_OTA: // OTA升级状态
			if (-1 == fnode_task_OTA_handler(node))
			{
				// 清除OTA标志
				node->ota_start_flag = 0;
			}
			break;

		case FHUB_STATE_FACTORY_TEST: // 设备测试状态
			if (-1 == fnode_task_FACTORY_TEST_handler(node))
			{
				// 清除工厂模式标志
				node->factory_mode_flag = 0;
			}
			break;

		default:
			break;
		}
	}

	pthread_exit(NULL);
}
#endif /* !FNIRS_EV_IO */

// 辅助函数：创建高优先级 FIFO 线程
int create_realtime_thread(pthread_t *tid, void *(*func)(void *), void *arg, int priority)
{
	pthread_attr_t attr;
	struct sched_param param;

	pthread_attr_init(&attr);

	// 设置调度策略为 SCHED_FIFO（实时、可抢占）
	pthread_attr_setschedpolicy(&attr, SCHED_FIFO);

	// 设置优先级（1～99，建议 70～85）
	param.sched_priority = priority;
	pthread_attr_setschedparam(&attr, &param);

	// 关键：显式使用 attr 中的调度设置（否则被忽略！）
	pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);

	int ret = pthread_create(tid, &attr, func, arg);

	pthread_attr_destroy(&attr);
	return ret;
}

/********************************************************************************************
 *  初始化HUB任务和NODE任务
 ********************************************************************************************/
int32_t fnode_init(fnode_handler_t *fh, uint32_t group)
{
	fnode_t *node = fnode_obj(); // 获取句柄
	int32_t rc = -1;

	if (NULL == fh || group >= FNODE_GROUP_MAX)
	{
		return -1;
	}

	// node = (fnode_t *)malloc(sizeof(fnode_t));

	if (NULL == node)
	{
		WLOGW("No mem\r\n");
		return -1;
	}

	memset(node, 0, sizeof(fnode_t));
#ifdef FNIRS_EV_IO
	node->ev_ota_fd = -1;
#endif
#if 0
    node->gain = FNODE_GAIN_DEF;
#else
	node->gain_735 = 0;
	node->gain_850 = 0;
#endif
	node->group = group;

	// 获取或连接消息队列
	rc = msgq_get(group);
	if (rc < 0)
	{
		return -1;
	}
	// 记录消息队列ID
	node->qid = rc;

	rc = fnode_can_init(node);
	if (0 != rc)
	{
		return -1;
	}

	node->run = 1;

	WLOGW("create task node and hub 2222\r\n");
#ifdef FNIRS_EV_IO
	fhub_open(&node->hub);
	node->hub_fnum = 0;
	node->tid[0] = 0;
	node->tid[1] = 0;
#else
	pthread_attr_t attr;
	size_t stacksize = 256 * 1024;

	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, stacksize);
	pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
	pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
	{
		struct sched_param param = { .sched_priority = 80 };
		pthread_attr_setschedparam(&attr, &param);
		pthread_create(&node->tid[1], &attr, fnode_task, node);
	}
	{
		struct sched_param param = { .sched_priority = 85 };
		pthread_attr_setschedparam(&attr, &param);
		pthread_create(&node->tid[0], &attr, fhub_task, node);
	}
	pthread_attr_destroy(&attr);
#endif

	// 线程同步初始化
	pthread_mutex_init(&node->event_mtx, NULL);
	pthread_cond_init(&node->event_cond, NULL);
	memset((void *)node->node_ack_ready, 0, sizeof(node->node_ack_ready));

	*fh = node;

	return 0;
}

int32_t fnode_exit(fnode_handler_t fh)
{
	fnode_t *node = fh;

	if (NULL == node)
	{
		return -1;
	}

	node->run = 0;

#ifdef FNIRS_EV_IO
	if (node->hub)
		fhub_close(node->hub);
#else
	pthread_join(node->tid[0], NULL);
	pthread_join(node->tid[1], NULL);
#endif

	return 0;
}

// 获取当前活跃node列表的
int32_t fnode_desc(fnode_handler_t fh, fnode_desc_t desc[FNODE_NID_MAX])
{
	fnode_t *node = fh;

	if (NULL == node)
	{
		return -1;
	}
	memcpy(desc, node->desc, sizeof(node->desc));

	return 0;
}

uint32_t fnode_num(fnode_handler_t fh)
{
	fnode_desc_t desc[FNODE_NID_MAX];
	uint32_t sum = 0;
	uint32_t i = 0;

	if (NULL == fh)
	{
		return -1;
	}

	fnode_desc(fh, desc);

	for (i = 0; i < FNODE_NID_MAX; i++)
	{
		if (desc[i].alive)
		{
			sum++;
		}
	}

	return sum;
}

// 状态重置
int32_t fnode_reset(fnode_handler_t fh)
{
	fnode_t *node = fh;

	if (NULL == node)
	{
		return -1;
	}

	if (node->sample)
	{
		WLOGW("Need to sample off\r\n");

		return -1;
	}

	node->reset = 1;

#ifdef FNIRS_EV_IO
	fnode_ev_kick_idle(node);
#endif

	return 0;
}

// 设置增益
int32_t fnode_s_gain(fnode_handler_t fh, uint8_t gain)
{
	fnode_t *node = fh;

	if (NULL == node)
	{
		return -1;
	}

	if (gain > FNODE_GAIN_MAX)
	{
		gain = FNODE_GAIN_MAX;
	}

	node->gain_735 = gain;
	node->gain_850 = gain;

	WLOGW("fnode_s_gain() -- [gain_735:%d, gain_850:%d]\r\n", node->gain_735, node->gain_850);

	return 0;
}

static int16_t fnode_stream_clamp_i32(int32_t v)
{
	if (v > 32767)
		return 32767;
	if (v < -32768)
		return -32768;
	return (int16_t)v;
}

/* PD block stores [24-bit sample | chn in top byte]; mask like fnode_gain_ok(). */
static uint32_t fnode_stream_pd24(uint32_t raw)
{
	return raw & 0x7fffffu;
}

static uint32_t fnode_stream_read_block_max(
	uint32_t spdata[FNODE_SAMPLE_SRC_MAX][FNODE_SAMPLE_DET_MAX],
	uint32_t src_idx, uint32_t det_idx)
{
	uint32_t best = 0;
	uint32_t pd;

	for (pd = 0; pd < FNODE_DETID_MAX; pd++) {
		uint32_t v = fnode_stream_pd24(spdata[src_idx][det_idx + pd]);

		if (v > best)
			best = v;
	}

	return best;
}

static uint32_t fnode_stream_read_pd_value(
	uint32_t spdata[FNODE_SAMPLE_SRC_MAX][FNODE_SAMPLE_DET_MAX],
	uint32_t src_idx, uint32_t det_idx, uint8_t pd_id)
{
	if (pd_id >= FNODE_DETID_MAX)
		pd_id = 0;

	return fnode_stream_pd24(spdata[src_idx][det_idx + pd_id]);
}

static uint32_t fnode_stream_read_pd(
	uint32_t spdata[FNODE_SAMPLE_SRC_MAX][FNODE_SAMPLE_DET_MAX],
	uint8_t src_node, uint8_t led_id, uint8_t det_node, uint8_t det_id,
	uint8_t wl735)
{
	uint8_t src_nid_0;
	uint8_t src_sid;
	uint8_t pd_id;
	uint32_t src_idx;
	uint32_t det_idx;

	if (src_node < FNODE_NID_MIN || src_node > FNODE_NID_MAX ||
	    det_node < FNODE_NID_MIN || det_node > FNODE_NID_MAX ||
	    led_id < 1 || led_id > 3)
		return 0;

	src_nid_0 = src_node - 1;
	src_sid = (uint8_t)(2 * (led_id - 1) + (wl735 ? 0 : 1));
	src_idx = src_nid_0 + (uint32_t)src_sid * FNODE_NID_MAX;
	det_idx = (uint32_t)(det_node - 1) * FNODE_DETID_MAX;
	pd_id = (det_id >= 1 && det_id <= FNODE_DETID_MAX) ? (uint8_t)(det_id - 1) : 0;

	return fnode_stream_read_pd_value(spdata, src_idx, det_idx, pd_id);
}

int32_t fnode_stream_push_channels(
	fnode_desc_t desc[FNODE_NID_MAX],
	uint32_t spdata[FNODE_SAMPLE_SRC_MAX][FNODE_SAMPLE_DET_MAX],
	const fnirs_ble_stream_ch_t *channels, uint8_t count,
	void (*cb)(int16_t v, void *ctx), void *ctx)
{
	uint8_t i;
	static uint32_t log_count;

	if (!channels || !cb || !spdata)
		return -1;

	(void)desc;

	for (i = 0; i < count; i++) {
		uint32_t raw735;
		uint32_t raw850;
		int16_t v735;
		int16_t v850;

		raw735 = fnode_stream_read_pd(spdata, channels[i].src_node,
					      channels[i].led_id,
					      channels[i].det_node,
					      channels[i].det_id, 1);
		raw850 = fnode_stream_read_pd(spdata, channels[i].src_node,
					      channels[i].led_id,
					      channels[i].det_node,
					      channels[i].det_id, 0);
		v735 = fnode_stream_clamp_i32((int32_t)(raw735 / 10u));
		v850 = fnode_stream_clamp_i32((int32_t)(raw850 / 10u));
		cb(v735, ctx);
		cb(v850, ctx);
	}

	if (++log_count % 10 == 0) {
		uint32_t r0 = fnode_stream_read_pd(spdata, channels[0].src_node,
						   channels[0].led_id,
						   channels[0].det_node,
						   channels[0].det_id, 1);
		WLOGI("BLE stream ch=%u raw735=%u -> %d\r\n",
		      count, r0,
		      fnode_stream_clamp_i32((int32_t)(r0 / 10u)));
	}

	return (int32_t)(count * 2);
}

int32_t fnode_stream_foreach_channel(
	fnode_desc_t desc[FNODE_NID_MAX],
	uint32_t spdata[FNODE_SAMPLE_SRC_MAX][FNODE_SAMPLE_DET_MAX],
	void (*cb)(int16_t v, void *ctx), void *ctx)
{
	uint32_t src_nid, src_sid, det_nid, pd;
	uint32_t count = 0;
	static uint32_t log_count;
	static uint32_t last_count;

	if (!desc || !spdata || !cb)
		return -1;

	for (src_nid = 0; src_nid < FNODE_NID_MAX; src_nid++) {
		uint32_t src_idx;

		if (!desc[src_nid].alive)
			continue;

		for (src_sid = 0; src_sid < FNODE_SRCID_MAX; src_sid++) {
			src_idx = src_nid + src_sid * FNODE_NID_MAX;

			for (det_nid = 0; det_nid < FNODE_NID_MAX; det_nid++) {
				uint32_t det_idx;

				if (!desc[det_nid].alive)
					continue;

				det_idx = det_nid * FNODE_DETID_MAX;

				for (pd = 0; pd < FNODE_DETID_MAX; pd++) {
					uint32_t raw = fnode_stream_pd24(
						spdata[src_idx][det_idx + pd]);
					int16_t v = fnode_stream_clamp_i32((int32_t)(raw / 10u));

					cb(v, ctx);
					count++;
				}
			}
		}
	}

	if (++log_count % 10 == 0 || count != last_count) {
		WLOGI("BLE stream frame: %u channels (alive matrix)\r\n", count);
		last_count = count;
	}

	return (int32_t)count;
}

int32_t fnode_stream_pick_int16(fnode_handler_t fh,
	uint32_t spdata[FNODE_SAMPLE_SRC_MAX][FNODE_SAMPLE_DET_MAX],
	int16_t *out_dc, int16_t *out_ac)
{
	fnode_t *node = fh;
	const sample_node_info_t *current_array;
	uint32_t array_size;
	uint32_t best_raw = 0;
	uint32_t i;
	static uint32_t last_raw;
	static uint32_t log_count;

	if (!out_dc || !out_ac || !spdata)
		return -1;

	*out_dc = 0;
	*out_ac = 0;

	if (node && node->array_from_host_is_used) {
		current_array = node->sample_node_array_from_host;
		array_size = node->led_num_from_host;
	} else {
		current_array = sample_node_array;
		array_size = sizeof(sample_node_array) / sizeof(sample_node_array[0]);
	}

	for (i = 0; i < array_size; i++) {
		uint8_t node_id = current_array[i].node_id;
		uint8_t led_id = current_array[i].node_led_id;
		uint8_t src_nid_0;
		uint8_t src_sid;
		uint32_t src_idx;
		uint32_t det_idx;
		uint32_t raw;

		if (node_id < FNODE_NID_MIN || node_id > FNODE_NID_MAX)
			continue;
		if (led_id < 1 || led_id > 3)
			continue;

		src_nid_0 = node_id - 1;
		src_sid = (uint8_t)(2 * (led_id - 1));
		src_idx = src_nid_0 + (uint32_t)src_sid * FNODE_NID_MAX;
		det_idx = (uint32_t)src_nid_0 * FNODE_DETID_MAX;

		raw = fnode_stream_read_block_max(spdata, src_idx, det_idx);
		if (raw > best_raw)
			best_raw = raw;
	}

	if (best_raw == 0) {
		uint32_t src_idx;
		uint32_t det_idx;

		for (src_idx = 0; src_idx < FNODE_SAMPLE_SRC_MAX; src_idx++) {
			for (det_idx = 0; det_idx < FNODE_SAMPLE_DET_MAX;
			     det_idx += FNODE_DETID_MAX) {
				uint32_t raw = fnode_stream_read_block_max(spdata, src_idx, det_idx);

				if (raw > best_raw)
					best_raw = raw;
			}
		}
	}

	*out_dc = fnode_stream_clamp_i32((int32_t)(best_raw / 1000u));
	*out_ac = fnode_stream_clamp_i32((int32_t)((int64_t)best_raw - (int64_t)last_raw) / 500);

	if (++log_count % 20 == 0)
		WLOGI("BLE stream pick: raw=%u dc=%d ac=%d\r\n",
		      best_raw, *out_dc, *out_ac);

	last_raw = best_raw;
	return 0;
}

// 开始采样
int32_t fnode_sample_on(fnode_handler_t fh)
{
	fnode_t *node = fh;

	if (NULL == node)
	{
		return -1;
	}

	node->sample = 1;

#ifdef FNIRS_EV_IO
	fnode_ev_kick_idle(node);
#endif

	return 0;
}

int32_t fnode_sample_off(fnode_handler_t fh)
{
	fnode_t *node = fh;

	if (NULL == node)
	{
		return -1;
	}

	node->sample = 0;

	return 0;
}

uint32_t fnode_sample_ison(fnode_handler_t fh)
{
	fnode_t *node = fh;

	if (NULL == node)
	{
		return 0;
	}

	return node->sample;
}

// 开始OTA升级
int32_t fnode_ota_start(fnode_handler_t fh)
{
	fnode_t *node = fh;

	if (NULL == node)
	{
		return -1;
	}

#ifdef FNIRS_EV_IO
	if (node->state == FHUB_STATE_OTA ||
	    node->state == FHUB_STATE_FACTORY_TEST ||
	    node->ev_ota_phase != FNODE_EV_OTA_OFF) {
		WLOGW("fnode_ota_start rejected: hub_state=%u ota_phase=%u\r\n",
		      node->state, node->ev_ota_phase);
		return -1;
	}
#endif

	node->sample = 0;
	node->factory_mode_flag = 0;
	node->ota_start_flag = 1;

#ifdef FNIRS_EV_IO
	g_fnode_ota_scan_count = 0;
	WLOGI("fnode_ota_start: event mode hub_state=%u\r\n", node->state);

	if (node->state == FHUB_STATE_RESET) {
		node->ota_start_flag = 0;
		fnode_ev_ota_enter(node);
	} else if (node->state == FHUB_STATE_SAMPLE) {
		WLOGI("fnode_ota_start: defer until sample stops\r\n");
	}
#endif

	return 0;
}

int32_t fnode_ota_cancel(fnode_handler_t fh)
{
	fnode_t *node = fh;
	uint8_t cmd = FNODE_CMD_OTA_OTA_CANCEL;
	int32_t rc = 0;

	if (NULL == node)
		return -1;

	node->ota_start_flag = 0;
	ota_info.node_ota_package_handling = 0;

#ifdef FNIRS_EV_IO
	if (node->state == FHUB_STATE_OTA ||
	    node->ev_ota_phase != FNODE_EV_OTA_OFF) {
		rc = fnode_can_write(node, &cmd, 1);
		if (node->ev_ota_phase != FNODE_EV_OTA_OFF)
			fnode_ev_ota_disarm(node);
		node->reset = 1;
		fnode_ev_kick_idle(node);
	}
#else
	if (node->state == FHUB_STATE_OTA) {
		rc = fnode_can_write(node, &cmd, 1);
		node->reset = 1;
	}
#endif

	ota_info.node_ack_ota_start_bitmap = 0;
	ota_info.node_ack_ota_package_bitmap = 0;
	ota_info.node_ack_ota_finish_bitmap = 0;
	ota_info.node_ota_current_bitmap = 0;
	ota_info.status = FNODE_OTA_STATUS_IDLE;

	return rc;
}

// 进入工厂测试模式
int32_t fnode_factory_test_mode_enter(fnode_handler_t fh, uint8_t flag)
{
	fnode_t *node = fh;

	if (NULL == node)
	{
		return -1;
	}

	node->sample = 0;
	node->ota_start_flag = 0;
	node->factory_mode_flag = flag; // 主要设置工厂模式标志

#ifdef FNIRS_EV_IO
	if (!flag) {
		factory_test.status = FNODE_FACTORY_STATUS_ENTER;
		fnode_ev_factory_stop(node);
	} else {
		fnode_ev_kick_idle(node);
	}
#endif

	return 0;
}

// 清除工厂模式暂存配置数据
void fnode_factory_test_mode_clear_var(void)
{
	factory_test.info_valid = 0;

	factory_test.nid_tx = 0;
	factory_test.led_id = 0;
	factory_test.led_735_850 = 0;
	factory_test.tx_ma = 0;

	factory_test.nid_rx = 0;
	factory_test.rx_pd = 0;

	factory_test.sample_interval = 0;
	factory_test.sample_count = 0;

	factory_test.cali_node_id = 0;
}

// 进入工厂单通道3厘米发射与接收测试模式
int32_t fnode_factory_test_mode_FUSB_MEASURE(fnode_handler_t fh, uint8_t *data, uint8_t data_len)
{
	fnode_t *node = fh;
	int rc = -1;

	if (NULL == node)
	{
		return -1;
	}

	printf("MEASURE %d\r\n", node->factory_mode_flag);
	if (node->factory_mode_flag == 0) // 检查是否在工厂模式
	{
		return -1;
	}

	// 参数检查
	// 发射节点
	factory_test.nid_tx = data[0];
	if (factory_test.nid_tx < FNODE_NID_MIN || factory_test.nid_tx > FNODE_NID_MAX)
	{
		return -1;
	}

	// 发射LED [1, 3]
	factory_test.led_id = data[1];
	if (factory_test.led_id < 1 || factory_test.led_id > 3)
	{
		return -1;
	}
	// 发射波长 [1, 2]
	factory_test.led_735_850 = data[2];
	if (factory_test.led_735_850 < 1 || factory_test.led_735_850 > 2)
	{
		return -1;
	}
	// 发射电流mA
	factory_test.tx_ma = data[3];

	// 接收节点
	factory_test.nid_rx = data[4];
	if (factory_test.nid_rx < FNODE_NID_MIN || factory_test.nid_rx > FNODE_NID_MAX)
	{
		return -1;
	}

	// 接收PD [1, 4]
	factory_test.rx_pd = data[5];
	if (factory_test.rx_pd < 1 || factory_test.rx_pd > 4)
	{
		return -1;
	}

	// 采样间隔
	factory_test.sample_interval = ((uint32_t)data[6] << 24) + ((uint32_t)data[7] << 16) + ((uint32_t)data[8] << 8) + (uint32_t)data[9];

	// 采样次数
	factory_test.sample_count = ((uint32_t)data[10] << 24) + ((uint32_t)data[11] << 16) + ((uint32_t)data[12] << 8) + (uint32_t)data[13];

	// 切换工厂模式测试状态
	factory_test.status = FNODE_FACTORY_STATUS_FUSB_MEASURE;

#ifdef FNIRS_EV_IO
	fnode_ev_factory_kick(node);
#endif

	return 0;
}

// 进入工厂单通道3厘米 735和 850 交替发射与接收测试模式
int32_t fnode_factory_test_mode_FUSB_MEASURE_ALTERNATE(fnode_handler_t fh, uint8_t *data, uint8_t data_len)
{
	fnode_t *node = fh;
	int rc = -1;

	if (NULL == node)
	{
		return -1;
	}

	printf("MEASURE ALTERNATE %d\r\n", node->factory_mode_flag);
	if (node->factory_mode_flag == 0) // 检查是否在工厂模式
	{
		return -1;
	}

	// 参数检查
	// 发射节点
	factory_test.nid_tx = data[0];
	if (factory_test.nid_tx < FNODE_NID_MIN || factory_test.nid_tx > FNODE_NID_MAX)
	{
		return -1;
	}

	// 发射LED [1, 3]
	factory_test.led_id = data[1];
	if (factory_test.led_id < 1 || factory_test.led_id > 3)
	{
		return -1;
	}

	// 发射电流mA
	factory_test.tx_ma = data[2];

	// 接收节点
	factory_test.nid_rx = data[3];
	if (factory_test.nid_rx < FNODE_NID_MIN || factory_test.nid_rx > FNODE_NID_MAX)
	{
		return -1;
	}

	// 接收PD [1, 4]
	factory_test.rx_pd = data[4];
	if (factory_test.rx_pd < 1 || factory_test.rx_pd > 4)
	{
		return -1;
	}

	// 采样间隔
	factory_test.sample_interval = ((uint32_t)data[5] << 24) + ((uint32_t)data[6] << 16) + ((uint32_t)data[7] << 8) + (uint32_t)data[8];

	// 采样次数
	factory_test.sample_count = ((uint32_t)data[9] << 24) + ((uint32_t)data[10] << 16) + ((uint32_t)data[11] << 8) + (uint32_t)data[12];

	// 切换工厂模式测试状态
	factory_test.status = FNODE_FACTORY_STATUS_FUSB_MEASURE_ALTERNATE;

#ifdef FNIRS_EV_IO
	fnode_ev_factory_kick(node);
#endif

	return 0;
}

int32_t fnode_factory_test_mode_LED_ON_FUSB_MEASURE(fnode_handler_t fh, uint8_t *data, uint8_t data_len)
{
	fnode_t *node = fh;
	int rc = -1;

	if (NULL == node)
	{
		return -1;
	}

	printf("LED_ON MEASURE %d\r\n", node->factory_mode_flag);
	if (node->factory_mode_flag == 0) // 检查是否在工厂模式
	{
		return -1;
	}

	// 参数检查
	// 发射节点
	factory_test.nid_tx = data[0];
	if (factory_test.nid_tx < FNODE_NID_MIN || factory_test.nid_tx > FNODE_NID_MAX)
	{
		return -1;
	}

	// 发射LED [1, 3]
	factory_test.led_id = data[1];
	if (factory_test.led_id < 1 || factory_test.led_id > 3)
	{
		return -1;
	}
	// 发射波长 [1, 2]
	factory_test.led_735_850 = data[2];
	if (factory_test.led_735_850 < 1 || factory_test.led_735_850 > 2)
	{
		return -1;
	}
	// 发射电流mA
	factory_test.tx_ma = data[3];

	// 接收节点
	factory_test.nid_rx = data[4];
	if (factory_test.nid_rx < FNODE_NID_MIN || factory_test.nid_rx > FNODE_NID_MAX)
	{
		return -1;
	}

	// 接收PD [1, 4]
	factory_test.rx_pd = data[5];
	if (factory_test.rx_pd < 1 || factory_test.rx_pd > 4)
	{
		return -1;
	}

	// 采样间隔
	factory_test.sample_interval = ((uint32_t)data[6] << 24) + ((uint32_t)data[7] << 16) + ((uint32_t)data[8] << 8) + (uint32_t)data[9];

	// 采样次数
	factory_test.sample_count = ((uint32_t)data[10] << 24) + ((uint32_t)data[11] << 16) + ((uint32_t)data[12] << 8) + (uint32_t)data[13];

	// 切换工厂模式测试状态
	factory_test.status = FNODE_FACTORY_STATUS_FUSB_LED_ON_MEASURE_LED_OFF;

#ifdef FNIRS_EV_IO
	fnode_ev_factory_kick(node);
#endif

	return 0;
}

// 对节点4个PD校准偏置测试模式
int32_t fnode_factory_test_mode_FUSB_CALI_PD(fnode_handler_t fh, uint8_t *data, uint8_t data_len)
{
	fnode_t *node = fh;
	int rc = -1;

	if (NULL == node)
	{
		return -1;
	}

	// 检查参数
	factory_test.cali_node_id = data[0];
	if (factory_test.cali_node_id < FNODE_NID_MIN || factory_test.cali_node_id > FNODE_NID_MAX)
	{
		return -1;
	}

	// 切换工厂模式测试状态
	factory_test.status = FNODE_FACTORY_STATUS_FUSB_CALI_PD_OFFSET;

#ifdef FNIRS_EV_IO
	fnode_ev_factory_kick(node);
#endif

	return 0;
}

//
int32_t fnode_factory_test_mode_FUSB_LED_ON(fnode_handler_t fh, uint8_t *data, uint8_t data_len)
{
	fnode_t *node = fh;
	int rc = -1;

	if (NULL == node)
	{
		return -1;
	}

	printf("LED flag=%d\r\n", node->factory_mode_flag);
	if (node->factory_mode_flag == 0) // 检查是否在工厂模式
	{
		// return -1;
	}

	// 参数检查
	// 发射节点
	factory_test.nid_tx = data[0];
	if (factory_test.nid_tx < FNODE_NID_MIN || factory_test.nid_tx > FNODE_NID_MAX)
	{
		return -1;
	}

	// 发射LED [1, 3]
	factory_test.led_id = data[1];
	if (factory_test.led_id < 1 || factory_test.led_id > 3)
	{
		return -1;
	}
	// 发射波长 [1, 2]
	factory_test.led_735_850 = data[2];
	if (factory_test.led_735_850 < 1 || factory_test.led_735_850 > 2)
	{
		return -1;
	}
	// 发射电流mA
	factory_test.tx_ma = data[3];

	// 单开/单关/全关
	factory_test.onoff_value = data[4];

	// 切换工厂模式测试状态
	factory_test.status = FNODE_FACTORY_STATUS_FUSB_LED_ON;

#ifdef FNIRS_EV_IO
	fnode_ev_factory_kick(node);
#endif

	return 0;
}

// 设置发光LED序列
int32_t fnode_set_array(uint8_t *data, uint16_t size)
{
	fnode_t *node = fnode_obj();
	uint8_t led_count = 0; // 记录本数组有多少个led

	if (NULL == node)
	{
		return -1;
	}

	/*
		//新增，用于从主机设置发光序列
		sample_node_info_t sample_node_array_from_host[MAX_SAMPLE_NODES_LED]; //记录从上位机传下来的发光序列
		uint8_t led_num_from_host;// 总共发光led的数量
		volatile bool array_from_host_is_used;//标志位，是否启用设置发光序列功能，0不启用，1启用
	*/
	if (size % 4 != 0)
	{
		return -1;
	}
	led_count = size / 4; // 因为每项占4字节
	if (led_count > MAX_SAMPLE_NODES_LED)
	{
		return -1; // 或截断处理
	}

	// 清空标志位
	memset(&node->array_from_host_is_used, 0, sizeof(node->array_from_host_is_used));
	// 清空原数组
	memset(&node->sample_node_array_from_host, 0, sizeof(node->sample_node_array_from_host));
	// 清空发光LED数量
	memset(&node->led_num_from_host, 0, sizeof(node->led_num_from_host));
	// 清空采样周期ms存储字节
	memset(&node->sample_peroid_ms, 0, sizeof(node->sample_peroid_ms));

	node->led_num_from_host = led_count;
	node->sample_peroid_ms = 11 * led_count; // 估算，11个节点是120ms
	for (int i = 0; i < led_count; i++)
	{
		node->sample_node_array_from_host[i].node_id = data[4 * i];
		node->sample_node_array_from_host[i].node_led_id = data[4 * i + 1];
		node->sample_node_array_from_host[i].power_735 = data[4 * i + 2];
		node->sample_node_array_from_host[i].power_850 = data[4 * i + 3];
	}

	node->array_from_host_is_used = 1;

	// ====== 新增：打印整个数组 ======
	WLOGI("Set LED sequence (total: %d):\r\n", led_count);
	WLOGI("Sample period: %d ms):\r\n", node->sample_peroid_ms);
	for (int i = 0; i < MAX_SAMPLE_NODES_LED; i++)
	{
		if (i < led_count)
		{
			WLOGW("  [%d] = {nid=%d, led=%d,735=0x%x,850=0x%x}\r\n",
				  i,
				  node->sample_node_array_from_host[i].node_id,
				  node->sample_node_array_from_host[i].node_led_id,
				  node->sample_node_array_from_host[i].power_735,
				  node->sample_node_array_from_host[i].power_850
				  );
		}
		else
		{
			// 可选：打印未使用的项（显示为 {0,0} 或跳过）
			// WLOGW("  [%d] = {0, 0} (unused)\r\n", i);
		}
	}
	// ===============================

	return node->sample_peroid_ms;
}
