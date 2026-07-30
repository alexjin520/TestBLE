#ifndef _FNIRS_STREAM_H_
#define _FNIRS_STREAM_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "stdint.h"
#include "fnode.h"

#define FNIRS_STREAM_CTL_PATH   "/tmp/fnirs_stream.ctl"
#define FNIRS_STREAM_DATA_PATH  "/tmp/fnirs_stream.data"

#define FNIRS_STREAM_CTL_START  'S'
#define FNIRS_STREAM_CTL_STOP   'T'
#define FNIRS_STREAM_CTL_ABORT  'A'

int32_t fnirs_stream_init(void);
void fnirs_stream_exit(void);

void fnirs_stream_ctl_local_start(void);
void fnirs_stream_ctl_local_stop(void);
void fnirs_stream_arm_writer(void);

#ifdef FNIRS_EMBEDDED
void fnirs_stream_ring_reset(void);
int fnirs_stream_ring_read(int16_t *out, int max);
#endif

void fnirs_stream_push_frame(
    uint32_t spdata[FNODE_SAMPLE_SRC_MAX][FNODE_SAMPLE_DET_MAX],
    fnode_desc_t desc[FNODE_NID_MAX]);

#ifdef __cplusplus
}
#endif

#endif
