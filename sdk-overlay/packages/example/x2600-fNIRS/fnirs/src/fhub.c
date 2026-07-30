
#include "stdio.h"
#include "string.h"
#include "errno.h"
#include "stdlib.h"
#include "unistd.h"
#include "signal.h"
#include "pthread.h"

#include "fnode.h"
#include "fhub.h"
#include "fusb.h"
#include "fdatalog.h"
#include "fnirs_stream.h"

//#include "dtu.h"

int32_t fhub_open(fhub_handler_t *h)
{
    fhub_t *hub = NULL;

    if(NULL == h)
	{
        return -1;
    }
	
    hub = (fhub_t *)malloc(sizeof(fhub_t));
    if(NULL == hub)
	{
        WLOGW("No mem\r\n");
		
        return -1;
    }
	
    memset(hub, 0, sizeof(fhub_t));
    *h = hub;
	
    return 0;
}

int32_t fhub_close(fhub_handler_t h)
{
    fhub_t *hub = h;

    if(NULL == hub)
	{
        return -1;
    }
	
    free(hub);
	
    return 0;
}

#if 0
/********************************************************************************************
 *  计算当前通道数
 *		计算方法 = （节点数 * 6光源） * （节点数 * PD数）
 *
 ********************************************************************************************/
static uint64_t fhub_rpf(fnode_desc_t desc[FNODE_NID_MAX])
{
    uint32_t i = 0;
    uint64_t sum = 0;
	
    for(i = 0; i < FNODE_NID_MAX; i++)
	{
        if(desc[i].alive)
		{
            sum++;
        }
    }
    return (sum * FNODE_SRCID_MAX  * sum * FNODE_DETID_MAX);
}
#endif

/********************************************************************************************
 *  计算当前有效节点数
 *
 ********************************************************************************************/
static uint8_t fhub_nodes(fnode_desc_t desc[FNODE_NID_MAX])
{
    uint32_t i = 0;
    uint8_t sum = 0;
	
    for(i = 0; i < FNODE_NID_MAX; i++)
	{
        if(desc[i].alive)
		{
            sum++;
        }
    }
	
    return sum;
}

/********************************************************************************************
 *  启动数据采集前的准备工作
 *
 ********************************************************************************************/
int32_t fhub_start(fhub_handler_t h, fnode_desc_t desc[FNODE_NID_MAX])
{
    fhub_t *hub = h;

    if(NULL == hub)
	{
        return -1;
    }
	
    memset(hub, 0, sizeof(fhub_t));

    memcpy(hub->desc, desc, sizeof(fnode_desc_t) * FNODE_NID_MAX);

	//计算当前有效通道数
    //hub->rpf = fhub_rpf(desc);

	//计算当前有效节点数
    hub->nodes = fhub_nodes(desc);
	
    return 0;
}

/********************************************************************************************
 *  功能：将本周期采样数据进行打包，并加入缓存队列，等待被发送
 *		1.按协议将采样数据进行打包；
 *		2.将数据包加入缓存队列，等待串口处理任务取出发送；
 *
 ********************************************************************************************/
/**
 * @brief 将一个完整采样周期的数据打包并提交给上报模块（如 USB/串口）
 *
 * 本函数负责：
 * 1. 按协议格式重组来自多个节点的原始采样数据（spdata）和传感器数据（sens_data）
 * 2. 将重组后的完整帧放入 hub->sample_data 缓冲区
 * 3. 通过 fusb_ringbuf_enqueue 提交到发送队列，等待底层通信任务发送
 *
 * @param h          fhub 句柄（上下文）
 * @param spdata     原始采样数据数组 [FNODE_SAMPLE_SRC_MAX][FNODE_SAMPLE_DET_MAX]
 *                   - 第一维：光源索引（src_nid + src_sid * FNODE_NID_MAX）
 *                   - 第二维：探测器索引（det_nid * FNODE_DETID_MAX）
 * @param sens_data  每个节点的传感器数据数组 [FNODE_NID_MAX]
 * @return           0: 成功；-1: 失败（如数据长度校验失败）
 */
int32_t fhub_append(
    fhub_handler_t h,
    uint32_t spdata[FNODE_SAMPLE_SRC_MAX][FNODE_SAMPLE_DET_MAX],
    fnode_sensor_t sens_data[FNODE_NID_MAX]
)
{
    fhub_t *hub = h;
    uint32_t offset = 0;        // 当前写入 sample_data 的偏移量（字节），用于拼接采样数据
    uint32_t sens_offset = 0;   // 当前写入 sensor_data 的偏移量（字节），用于拼接传感器数据
    uint64_t timestamp_ms;

    // 参数合法性检查
    if (NULL == hub) {
        return -1;
    }

    // 清空目标缓冲区，避免残留旧数据
    memset(hub->sample_data, 0, sizeof(hub->sample_data)); // 最终上报的完整数据帧
    memset(hub->sensor_data, 0, sizeof(hub->sensor_data)); // 临时存放传感器数据

    // 帧格式从这里开始为：[8字节单调毫秒时间戳][采样数据][传感器数据]。
    timestamp_ms = wos_system_clock_ms();
    hub->sample_data[0] = (uint8_t)(timestamp_ms >> 56);
    hub->sample_data[1] = (uint8_t)(timestamp_ms >> 48);
    hub->sample_data[2] = (uint8_t)(timestamp_ms >> 40);
    hub->sample_data[3] = (uint8_t)(timestamp_ms >> 32);
    hub->sample_data[4] = (uint8_t)(timestamp_ms >> 24);
    hub->sample_data[5] = (uint8_t)(timestamp_ms >> 16);
    hub->sample_data[6] = (uint8_t)(timestamp_ms >> 8);
    hub->sample_data[7] = (uint8_t)timestamp_ms;
    offset = FNODE_TIME_STAMP_SIZE;

    // === 第一步：遍历所有可能的“发光节点”（Source Node）===
    for (uint32_t src_nid = 0; src_nid < FNODE_NID_MAX; src_nid++) {
        // 跳过不在线的节点
        if (0 == hub->desc[src_nid].alive) {
            continue;
        }

        // === 保存该发光节点的传感器数据 ===
        // 每个节点的传感器数据固定大小（如温度、电压等）
        memcpy(
            hub->sensor_data + sens_offset,      // 目标地址（追加到末尾）
            &sens_data[src_nid],                 // 源地址（当前节点的传感器数据）
            FNODE_SENSOR_DATA_SIZE               // 单个节点传感器数据大小（字节）
        );
        sens_offset += FNODE_SENSOR_DATA_SIZE;   // 更新偏移量

        // === 第二步：遍历该发光节点控制的所有光源（LED）===
        for (uint32_t src_sid = 0; src_sid < FNODE_SRCID_MAX; src_sid++) {

            // === 第三步：遍历所有“探测节点”（Detector Node）===
            for (uint32_t det_nid = 0; det_nid < FNODE_NID_MAX; det_nid++) {
                // 跳过不在线的探测节点
                if (0 == hub->desc[det_nid].alive) {
                    continue;
                }

                // 计算在 spdata 中的索引：
                // - src_idx: 表示“哪个光源被点亮”
                //   公式：src_nid + src_sid * FNODE_NID_MAX
                //   （因为每个 sid 有 FNODE_NID_MAX 个 nid）
                uint32_t src_idx = src_nid + (src_sid * FNODE_NID_MAX);

                // - det_idx: 表示“从哪个探测器读取数据”
                //   公式：det_nid * FNODE_DETID_MAX
                //   （因为每个 nid 有 FNODE_DETID_MAX 个 PD 通道）
                uint32_t det_idx = det_nid * FNODE_DETID_MAX;

                // 从原始数据中取出 4 通道 PD 值（共 4 * sizeof(uint32_t) = 16 字节）
                // 并追加到最终上报缓冲区 hub->sample_data 的末尾
                memcpy(
                    hub->sample_data + offset,           // 目标地址
                    &spdata[src_idx][det_idx],           // 源地址（4 个 uint32_t）
                    FNODE_DATA_BLOCK_SIZE                // 16 字节（4 通道 * 4 字节）
                );
                offset += FNODE_DATA_BLOCK_SIZE;         // 更新采样数据偏移量
            }
        }
    }

    // === 数据完整性校验 ===
    // 理论上，有效数据总长度应为：
    //   (有效节点数) * (有效节点数) * (每对 (src,det) 的数据大小)
    // 因为：每个发光节点 × 每个探测节点 = N×N 对，每对 16 字节
#if 1
    if (offset != FNODE_TIME_STAMP_SIZE +
                  hub->nodes * hub->nodes * FNODE_DATA_SIZE) {
        WLOGE("spdata size wrong, actual=%u, expected=%u (nodes=%u)\r\n",
              offset,
              FNODE_TIME_STAMP_SIZE +
                  hub->nodes * hub->nodes * FNODE_DATA_SIZE,
              hub->nodes);
        return -1; // 数据不完整或格式错误，丢弃本帧
    }
#endif

    // === 将传感器数据追加到采样数据末尾 ===
    // 最终帧结构：[采样数据][传感器数据]
    memcpy(
        hub->sample_data + offset,   // 采样数据之后的位置
        hub->sensor_data,            // 已拼接好的所有传感器数据
        sens_offset                  // 传感器数据总长度
    );
    offset += sens_offset;           // 更新总长度（用于后续发送）

    // FIXME: 可在此处添加数据加密逻辑
    // fhub_encrypt(hub->sample_data, offset);

    // === 提交数据到发送队列 ===
#if !defined(FNIRS_EMBEDDED) || defined(FNIRS_EV_IO)
    fusb_ringbuf_enqueue(hub->sample_data, offset, hub->nodes);
#endif

    //   提交数据到写入emmc队列
    fdatalog_ringbuf_enqueue(hub->sample_data, offset, hub->nodes);

    // 帧计数器递增（用于调试或协议序号）
    hub->fnum++;

#ifdef FNIRS_EV_IO
    {
        static uint32_t ble_frames;

        if ((++ble_frames % 40) == 1)
            WLOGI("fhub_append BLE stream frame #%u nodes=%u\r\n",
                  ble_frames, hub->nodes);
    }
#endif

    fnirs_stream_push_frame(spdata, hub->desc);

    return 0; // 成功
}
