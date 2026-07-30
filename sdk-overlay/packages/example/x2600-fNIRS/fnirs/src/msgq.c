
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "unistd.h"
#include "errno.h"
#include "sys/types.h"
#include "sys/ipc.h"
#include "sys/msg.h"

//#include "wos_log.h"
#include "msgq.h"

#if 1 //新实现

/* 获取 or 连接已存在队列 */
int32_t msgq_get(uint32_t key)
{
    key_t k = key + FHUB_MSGQ_KEY;
    int qid = msgget(k, 0);               // 先尝试打开已存在
    if (qid == -1 && errno == ENOENT)     // 不存在才创建
        qid = msgget(k, IPC_CREAT | 0666);
	
    if (qid == -1)
        WLOGW("msgget failed: %s\n", strerror(errno));

	//ipcs -q  看到队列泄漏
	
    return qid;
}

/* 删除队列 */
int32_t msgq_rm(int32_t qid)
{
    if (qid < 0)
		return -1;
    return (msgctl(qid, IPC_RMID, NULL) == 0) ? 0 : -1;
}

/* 非阻塞发，满时重试一次（10 ms）*/
int32_t msgq_snd(int32_t qid, msgq_t *msg)
{
    if (qid < 0 || !msg)
		return -1;

    ssize_t sz = sizeof(msgq_t) - sizeof(long);
    if (msgsnd(qid, msg, sz, IPC_NOWAIT) == 0)
        return 0;

    if (errno == EAGAIN) {          // 队列满
        usleep(10 * 1000);          // 让出 CPU
        if (msgsnd(qid, msg, sz, IPC_NOWAIT) == 0)
            return 0;
    }
	
    //WLOGW("msgsnd: %s\n", strerror(errno));
	
    return -1;
}

/* 非阻塞收，正确处理长度 */
int32_t msgq_rcv(int32_t qid, long mtype, msgq_t *msg)
{
    if (qid < 0 || !msg)
		return -1;

    ssize_t exp = sizeof(msgq_t) - sizeof(long);
    ssize_t n   = msgrcv(qid, msg, exp, mtype, IPC_NOWAIT | MSG_NOERROR);
    if (n == exp)
		return 0;         // 完整帧
		
    if (n == -1 && errno == ENOMSG)
		return -1; // 无数据，不打印
		
    //WLOGW("msgrcv: got %zd bytes, expect %zd (%s)\n", n, exp, strerror(errno));
	
    return -1;
}

#else
int32_t msgq_get(uint32_t key)
{
    int32_t qid = -1;

    qid = msgget(key + FHUB_MSGQ_KEY, IPC_CREAT | 0666);
    if(qid < 0)
	{
        WLOGW("msgget failed, %s\r\n", strerror(errno));
		
        return -1;
    }
	
    return qid;
}

int32_t msgq_rm(int32_t qid)
{
    int32_t rc = -1;

    rc = msgctl(qid, IPC_RMID, 0);
    if(0 != rc)
	{
        WLOGW("msgctl IPC_RMID failed, %s\r\n", strerror(errno));
		
        return -1;
    }
	
    return 0;
}

int32_t msgq_snd(int32_t qid, msgq_t *msg)
{
    int32_t rc = -1;
    
    if(qid < 0 || NULL == msg)
	{
        return -1;
    }
	
    rc = msgsnd(qid, (void *)msg, sizeof(msgq_t) - sizeof(long), IPC_NOWAIT);
    if(0 != rc)
	{
        WLOGW("msgsnd failed, %s\r\n", strerror(errno));
		
        return -1;
    }
    return 0;
}

int32_t msgq_rcv(int32_t qid, long mtype, msgq_t *msg)
{
    int32_t rc = -1;

    if(qid < 0 || NULL == msg)
	{
        return -1;
    }
	
    rc = msgrcv(qid, (void *)msg, sizeof(msgq_t) - sizeof(long), mtype, MSG_NOERROR | IPC_NOWAIT);
    if(-1 == rc)
	{
        if (errno != ENOMSG)
		{
            WLOGW("haas_msgrcv failed, %s\r\n", strerror(errno));
        }
		
        return -1;
    }
	
    if(rc != sizeof(msgq_t) - sizeof(long))
	{
        WLOGW("haas_msgrcv rc = %d\r\n", rc);
		
        return -1;
    }
	
    return 0;
}
#endif

