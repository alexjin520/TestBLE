
#ifndef _FHUB_MSG_H_
#define _FHUB_MSG_H_

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */

#include "stdint.h"
#include "my_interface.h"



//使用消息队列api

#define FHUB_MSGQ_KEY    (0x2600 | 0x10)

typedef enum {
    MSGQ_MTYPE_TOP,//invalid msgtype
    MSGQ_MTYPE_NORMAL,
    MSGQ_MTYPE_MAX,
}msgq_type_t;

typedef struct {
    long mtype; //must greater than 0
    uint32_t datasz;
    void *data;
}msgq_t;

/*
* 获取消息队列 qid, # ipcs -q  #查看消息队列
*/
int32_t msgq_get(uint32_t key);

/*
* 删除消息队列， ipcrm -Q  0x2610
*/
int32_t msgq_rm(int32_t qid);

int32_t msgq_snd(int32_t qid, msgq_t *msg);

int32_t msgq_rcv(int32_t qid, long mtype, msgq_t *msg);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif
