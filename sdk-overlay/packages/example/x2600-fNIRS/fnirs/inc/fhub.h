#ifndef _FHUB_H_
#define _FHUB_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "stdint.h"
#include "fnode.h"
#include "my_interface.h"

#define FHUB_SW_MAJOR  2
#define FHUB_SW_MINOR  0
#define FHUB_SW_PATCH  1

typedef struct
{
    uint8_t hub_sw[4]; //magic, major, minor, patch
    uint8_t padding1[4];
    uint64_t rpf; //recordings_per_frame
    uint64_t nframe; //frame num;
    uint8_t padding2[24];
}fhub_head_t; //sizeof(fhub_head_t) = 48 bytes

typedef struct
{
    //FILE *fp;
	
    uint32_t fnum; //采样周期数
	
    uint8_t sample_data[FNODE_TIME_STAMP_SIZE
        + FNODE_NID_MAX * FNODE_NID_MAX * FNODE_DATA_SIZE
        + FNODE_NID_MAX * sizeof(fnode_sensor_t)]; //时间戳+采样数据+传感器数据

	uint8_t sensor_data[FNODE_NID_MAX * sizeof(fnode_sensor_t)]; // 每个node一个传感器数据
	
    fnode_desc_t desc[FNODE_NID_MAX];
	
    //uint64_t rpf; //当前通道数
	
    uint8_t nodes; //node num;
}fhub_t;

typedef void *fhub_handler_t;

int32_t fhub_open(fhub_handler_t *h);
int32_t fhub_close(fhub_handler_t h);

int32_t fhub_start(fhub_handler_t h, fnode_desc_t desc[FNODE_NID_MAX]);
int32_t fhub_append(fhub_handler_t h, uint32_t spdata[FNODE_SAMPLE_SRC_MAX][FNODE_SAMPLE_DET_MAX], fnode_sensor_t sens_data[FNODE_NID_MAX]);
int32_t fhub_done(fhub_handler_t h);

#ifdef __cplusplus
}
#endif

#endif
