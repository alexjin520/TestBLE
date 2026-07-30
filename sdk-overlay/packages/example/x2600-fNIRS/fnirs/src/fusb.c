#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "unistd.h"
#include "errno.h"
#include "pthread.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/select.h>
#include "endian.h"
#include "linux/list.h"
#include <time.h>
#include "fusb.h"
#include "fNIRS.h"
#include "fnode.h"
#include <inttypes.h>
#include "fdatalog.h"
#include "led.h"
#include <stdatomic.h>

#define FUSB_DEV                "/dev/ttyS1"

#define MY_UART_BAUD    		2000000 //230400

#define STATUS_PATH   "/sys/class/power_supply/bq25890-charger/status"
#define VOLTAGE_PATH  "/sys/class/power_supply/bq25890-charger/voltage_now"
#define VOL_POWER_OFF_THRESHOLD 3200000L //低于3.6V关机
#define VOL_LOWVOLTAGE_THRESHOLD 3700000L //低于3.7V发送低电压警告

static fusb_obj_t *fusb_obj(void)
{
    static fusb_obj_t obj = {};
    return &obj;
}

#ifdef FNIRS_EV_IO
static void fusb_ev_trace(const char *step)
{
    (void)step;
}
#endif

static int32_t fusb_open(fusb_obj_t *obj)
{
    int32_t rc = -1;

    if(0 == wos_timer_is_expired(&obj->op_tm))
	{
        return -1;
    }
	
    wos_timer_countdown_ms(&obj->op_tm, 2000);
	
    rc = wos_uart_fopen(FUSB_DEV, MY_UART_BAUD, 8, 'N', 1);
	
    if(rc < 0)
	{
        WLOGW("fusb_open %s @ %d failed: %s\r\n", FUSB_DEV, MY_UART_BAUD, strerror(errno));
        return -1;
    }
	
    obj->fd = rc;
	
    return 0;
}

static int32_t fusb_close(fusb_obj_t *obj)
{
    wos_uart_close(obj->fd);
    obj->fd = -1;
	
    return 0;
}

/* ev_uart sets O_NONBLOCK; large SPDATA frames need a retrying write. */
static int32_t fusb_uart_write_all(int32_t fd, const uint8_t *buf, uint32_t len)
{
    uint32_t off = 0;

    while (off < len) {
        ssize_t nw = write(fd, buf + off, len - off);

        if (nw > 0) {
            off += (uint32_t)nw;
            continue;
        }
        if (nw < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            fd_set wfds;
            struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };

            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            if (select(fd + 1, NULL, &wfds, NULL, &tv) <= 0)
                return -1;
            continue;
        }
        return -1;
    }

    return (int32_t)len;
}

/********************************************************************************************
 *  功能：按串口通信协议进行数据打包后，通过串口发送数据包
 *		1.按协议打包数据；
 *		2.通过串口发送数据包；
 *
 ********************************************************************************************/
// 向上位机（Host）发送一条符合 FUSB 协议格式的数据帧
// 参数说明：
//   obj   : 指向 FUSB 模块全局上下文对象的指针
//   addr  : 16 位命令地址（如 FUSB_SPDATA、FUSB_SCAN 等），标识数据类型
//   data  : 指向要发送的有效载荷数据缓冲区（可为 NULL）
//   size  : 有效载荷的字节长度（若 data 为 NULL，则 size 应为 0）
static int32_t fusb_write(fusb_obj_t *obj, uint16_t addr, uint8_t *data, uint16_t size)
{
    // 用于记录整个协议帧的总长度（单位：字节）
    uint32_t len = 0;
    uint16_t payload_size = size;
    
    // 用于接收底层串口写函数的返回值（实际写入的字节数）
    int32_t rc = -1;
    uint8_t *txbuf;

    // 安全性检查：确保 obj 不为空、临时读写缓冲区 rwbuff 已分配、串口已成功打开（fd >= 0）
    if(NULL == obj || NULL == obj->txbuff || obj->fd < 0)
    {
        WLOGW("fusb_write() -- [EMP]\r\n");
        return -1;
    }

    if (size > FUSB_RWBUFF_SIZE - 8) {
        WLOGW("fusb_write() size %u too large\r\n", (unsigned)size);
        return -1;
    }

    /*
     * fhub_append() 内部统一保存时间戳。旧的 FUSB_SPDATA 地址仍可
     * 兼容无时间戳格式：保留 nodes，跳过其后的 8 字节时间戳。
     */
    if (addr == FUSB_SPDATA) {
        if (!data || size < 1 + FNODE_TIME_STAMP_SIZE) {
            WLOGW("legacy spdata missing timestamp: size=%u\r\n",
                  (unsigned)size);
            return -1;
        }
        payload_size = size - FNODE_TIME_STAMP_SIZE;
    }

    pthread_mutex_lock(&obj->write_mtx);
    txbuf = obj->txbuff;

    // 构造 FUSB 协议帧头（共 5 字节）：
    // 第 0 字节：源/方向标识符（SID）
    //   FUSB_HUB2HOST (0x10) 表示数据由设备（HUB）发往上位机（HOST）
    txbuf[0] = FUSB_HUB2HOST;
    
    // 第 1-2 字节：16 位命令地址（addr），采用大端序（Big-Endian）存储
    txbuf[1] = (addr >> 8);      // 高 8 位
    txbuf[2] = (addr & 0xff);    // 低 8 位
    
    // 第 3-4 字节：16 位实际有效载荷长度，采用大端序。
    txbuf[3] = (payload_size >> 8);
    txbuf[4] = (payload_size & 0xff);

    if (data) {
        if (addr == FUSB_SPDATA) {
            txbuf[5] = data[0]; // nodes
            memcpy(txbuf + 6,
                   data + 1 + FNODE_TIME_STAMP_SIZE,
                   size - 1 - FNODE_TIME_STAMP_SIZE);
        } else {
            /*
             * FUSB_SPDATA_WITH_TIME 的 data 已经是
             * [nodes][timestamp][sample][sensor]，不得再次插入时间戳。
             */
            memcpy(txbuf + 5, data, size);
        }
    }

    txbuf[5 + payload_size] = FUSB_EOT;
    txbuf[6 + payload_size] = FUSB_EOT;
    len = 7 + payload_size;


    if (obj->ev_mode)
        rc = fusb_uart_write_all(obj->fd, txbuf, len);
    else
        rc = wos_uart_write(obj->fd, txbuf, len);

    if (len != (uint32_t)rc) {
        WLOGW("fusb_write() addr=0x%04X len=%u rc=%d\r\n",
              addr, len, rc);

        if (!obj->ev_mode)
            fusb_close(obj);

        pthread_mutex_unlock(&obj->write_mtx);
        return -1;
    }

    pthread_mutex_unlock(&obj->write_mtx);

    // 所有字节成功发送，返回 0 表示成功
    return 0;
}




static int32_t fusb_hbeat_handler(fusb_obj_t *obj)
{
    int32_t rc = -1;
	
    if(0 == wos_timer_is_expired(&obj->hb_tm))
	{
        return -1;
    }
	
    wos_timer_countdown_ms(&obj->hb_tm, 3000);
	
    rc = fusb_write(obj, FUSB_HBEAT, NULL, 0);
	
    return rc;
}


//huang,电池电压检测函数
static int32_t fusb_voltage_detect_handler(fusb_obj_t *obj)
{
    int32_t rc = -1;

    FILE *fp;
    char status[32] = {0};
    long voltage = 0;
    char cmd[128];
	
    if(0 == wos_timer_is_expired(&obj->vd_tm)) //检查定时器是否到期
	{
        return -1;
    }
	

    // 1. 读取 status
    fp = fopen(STATUS_PATH, "r");
    if (!fp) {
        wos_timer_countdown_ms(&obj->vd_tm, 5000);
        return 1;
    }

    if (fgets(status, sizeof(status), fp) == NULL) {
        fclose(fp);
        wos_timer_countdown_ms(&obj->vd_tm, 5000);
        return 1;
    }
    fclose(fp);


    // 去掉换行符（fgets 会保留 \n）
    size_t len = strlen(status);
    if (len > 0 && status[len - 1] == '\n') {
        status[len - 1] = '\0';
    }


    /*==============================================================================
    * 上报电池电压（单位：μV）
    *============================================================================*/
    
     // 读取 voltage_now
    fp = fopen(VOLTAGE_PATH, "r");
    if (!fp) {
        wos_timer_countdown_ms(&obj->vd_tm, 5000);
        return 1;
    }
    if (fscanf(fp, "%ld", &voltage) != 1) {
        fclose(fp);
        wos_timer_countdown_ms(&obj->vd_tm, 5000);
        return 1;
    }
    fclose(fp);

    /* sysfs 读数异常时不做关机判断（台架/无电池时常为 0） */
    if (voltage < 1000000L || voltage > 6000000L) {
        WLOGW("Battery voltage invalid: %ld uV, skip detect\n", voltage);
        wos_timer_countdown_ms(&obj->vd_tm, 5000);
        return 1;
    }

    // voltage_now 单位 是 微伏（μV）
    WLOGI("Battery voltage: %ld uV (%ld mV)\n", voltage, voltage / 1000);


    // 1. 将 long 转为 uint32_t（确保 4 字节）
    uint32_t voltage_u32 = (uint32_t)voltage;

    // 2. 准备 4 字节大端序缓冲区
    uint8_t data[4];
    data[0] = (voltage_u32 >> 24) & 0xFF;  // MSB
    data[1] = (voltage_u32 >> 16) & 0xFF;
    data[2] = (voltage_u32 >> 8)  & 0xFF;
    data[3] = (voltage_u32 >> 0)  & 0xFF;  // LSB



    // 2. 判断充电状态
    if (strcmp(status, "Charging") == 0) {
        //WLOGW("is charging , pass voltage detect\n");
        
        rc = fusb_write(obj, FUSB_CHARGING, data, sizeof(data)); //处于充电状态，串口上报信息
        WLOGI("Sent Charging voltage: %ld μV\n", voltage);

    } 
    else if (strcmp(status, "Discharging") == 0) { //没有插充电器

        if(voltage >= VOL_LOWVOLTAGE_THRESHOLD)
        {
            /*==============================================================================
            * 未充电状态：上报电池电压（单位：μV）
            *============================================================================*/
                
            //调用 fusb_write 发送
            int32_t ret = fusb_write(obj, FUSB_DISCHARGING, data, sizeof(data));

            if (ret == 0) {
                //WLOGI("Sent discharging voltage: %ld μV\n", voltage);
            } else {
                    WLOGE("Failed to send voltage!\n");
            }
            
        }
        else if(voltage < VOL_LOWVOLTAGE_THRESHOLD && voltage >= VOL_POWER_OFF_THRESHOLD)
        {
            /*==============================================================================
            * 未充电状态,且低于报警电压（3.7V）（单位：μV）
            *============================================================================*/
            //调用 fusb_write 发送
            int32_t ret = fusb_write(obj, FUSB_LOWVOLTAGE, data, sizeof(data));

            WLOGI("Battery voltage too low (%ld mV < 3700 mV).\n",
                voltage / 1000);
        }
        else
        {  
            /*==============================================================================
            * 未充电状态,且低于关机电压（3.6V）（单位：μV）
            *============================================================================*/
           
            WLOGE("Battery voltage too low (%ld mV). Executing poweroff...\n", voltage / 1000);
                
            rc = fusb_write(obj, FUSB_POWEROFF, data, sizeof(data)); //向HOST发送通知，自己准备关机

            led_all_off();//关指示灯

#if defined(FNIRS_EMBEDDED) || defined(FNIRS_EV_IO)
            WLOGW("auto poweroff suppressed (embedded/ev mode)\n");
#else
            sync(); // 确保文件系统写入完成
            snprintf(cmd, sizeof(cmd), "poweroff"); // 直接关机（断电）
            int ret = system(cmd);
            if (ret == -1) {
                WLOGE("Failed to execute poweroff command!\n");
            }
#endif

        }
     
    }
    else if (strcmp(status, "Full") == 0) { //没有插充电器
    
        //WLOGW("Battery Full\n");
        rc = fusb_write(obj, FUSB_BAT_FULL, NULL, 0); //电池已经充满，串口上报信息
        WLOGI("Battery Full Voltage: %ld μV\n", voltage);
    }
    else{
    
        WLOGW("Other status: %s\n", status);
        
    }


    wos_timer_countdown_ms(&obj->vd_tm, 5000); //10s检测一次电压


    //rc = fusb_write(obj, FUSB_HBEAT, NULL, 0);
	
    return rc;
}





static void fusb_handler(fusb_obj_t *obj, uint16_t addr, uint8_t *data, uint16_t size);

#ifdef FNIRS_EV_IO
/*
 * ttyS1 is non-blocking in event mode.  A FUSB frame, especially an OTA
 * package, is commonly split across several read(2) calls.  Keep a stream
 * buffer so partial frames survive until the next EV_READ callback and so
 * several coalesced frames can be handled in one pass.
 */
static void fusb_ev_consume_rx(fusb_obj_t *obj, uint32_t count)
{
    if (count >= obj->rxlen) {
        obj->rxlen = 0;
        return;
    }

    memmove(obj->rwbuff, obj->rwbuff + count, obj->rxlen - count);
    obj->rxlen -= count;
}

static void fusb_ev_parse_rx(fusb_obj_t *obj)
{
    while (obj->rxlen != 0) {
        uint8_t *sid;
        uint16_t addr;
        uint16_t size;
        uint32_t frame_len;
        uint32_t skipped;

        sid = memchr(obj->rwbuff, FUSB_HOST2HUB, obj->rxlen);
        if (sid == NULL) {
            WLOGW("UART RX dropped %u byte(s) before SID\r\n",
                  (unsigned)obj->rxlen);
            obj->rxlen = 0;
            return;
        }

        skipped = (uint32_t)(sid - obj->rwbuff);
        if (skipped != 0) {
            WLOGW("UART RX resync skipped %u byte(s)\r\n",
                  (unsigned)skipped);
            fusb_ev_consume_rx(obj, skipped);
        }

        /* SID + ADDR + LEN */
        if (obj->rxlen < 5)
            return;

        addr = ((uint16_t)obj->rwbuff[1] << 8) | obj->rwbuff[2];
        size = ((uint16_t)obj->rwbuff[3] << 8) | obj->rwbuff[4];
        if (size > FUSB_RWBUFF_SIZE - 8) {
            WLOGW("UART RX frame size %u too large\r\n", (unsigned)size);
            fusb_ev_consume_rx(obj, 1);
            continue;
        }

        frame_len = (uint32_t)size + 7;
        if (obj->rxlen < frame_len)
            return;

        if (obj->rwbuff[5 + size] != FUSB_EOT ||
            obj->rwbuff[6 + size] != FUSB_EOT) {
            WLOGW("UART RX EOT error addr=0x%04x size=%u: %#x %#x\r\n",
                  addr, (unsigned)size, obj->rwbuff[5 + size],
                  obj->rwbuff[6 + size]);
            fusb_ev_consume_rx(obj, 1);
            continue;
        }

        {
            char note[32];

            snprintf(note, sizeof(note), "uart cmd %#x", addr);
            fusb_ev_trace(note);
        }
        fusb_handler(obj, addr, obj->rwbuff + 5, size);
        fusb_ev_consume_rx(obj, frame_len);
    }
}

static void fusb_ev_read_handler(fusb_obj_t *obj)
{
    for (;;) {
        ssize_t nr;

        fusb_ev_parse_rx(obj);
        if (obj->rxlen >= FUSB_RWBUFF_SIZE) {
            WLOGW("UART RX buffer full; resetting parser\r\n");
            obj->rxlen = 0;
        }

        nr = read(obj->fd, obj->rwbuff + obj->rxlen,
                  FUSB_RWBUFF_SIZE - obj->rxlen);
        if (nr > 0) {
            obj->rxlen += (uint32_t)nr;
            continue;
        }
        if (nr < 0 && errno == EINTR)
            continue;
        if (nr < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            WLOGW("UART RX read failed: %s\r\n", strerror(errno));
        break;
    }

    fusb_ev_parse_rx(obj);
}
#endif

// 定义一个静态函数，用于处理从 UART 接收的 FUSB 协议帧（主机 → HUB）
static void fusb_read_handler(fusb_obj_t *obj)
{
    uint8_t buf[2];
    uint16_t addr = 0;
    uint16_t size = 0;
    int32_t rc = -1;
    uint32_t tout = FUSB_FRAME_TOUT;

#ifdef FNIRS_EV_IO
    if (obj && obj->ev_mode) {
        if (obj->rwbuff != NULL && obj->fd >= 0)
            fusb_ev_read_handler(obj);
        return;
    }
#endif

    if(NULL == obj || NULL == obj->rwbuff || obj->fd < 0)
    {
        return;
    }

    rc = wos_uart_read(obj->fd, buf, 1, 0);

    if(1 != rc)
    {
        return;
    }

    if(FUSB_HOST2HUB != buf[0])
    {
        WLOGW("SID wrong, %#x\r\n", buf[0]);
        return;
    }

    rc = wos_uart_read(obj->fd, buf, 2, tout);

    if(2 != rc)
    {
        WLOGW("Loss addr\r\n");
        return;
    }

    addr = ((buf[0] << 8) | buf[1]);
    rc = wos_uart_read(obj->fd, buf, 2, tout);

    if(2 != rc)
    {
        WLOGW("Loss size\r\n");
        return;
    }

    size = ((buf[0] << 8) | buf[1]);

    if (size > FUSB_RWBUFF_SIZE - 8) {
        WLOGW("frame size %u too large\r\n", (unsigned)size);
        return;
    }

    if(0 != size)
    {
        rc = wos_uart_read(obj->fd, obj->rwbuff, size, tout);

        if(size != rc)
        {
            WLOGW("Loss data\r\n");
            return;
        }
    }

    rc = wos_uart_read(obj->fd, buf, 2, tout);

    if(2 != rc)
    {
        WLOGW("Loss eot\r\n");
        return;
    }

    if(FUSB_EOT != buf[0] || FUSB_EOT != buf[1])
    {
        WLOGW("EOT ERR. %#x, %#x\r\n", buf[0], buf[1]);
        return;
    }

#ifdef FNIRS_EV_IO
    {
        char note[32];

        snprintf(note, sizeof(note), "uart cmd %#x", addr);
        fusb_ev_trace(note);
    }
#endif

    fusb_handler(obj, addr, obj->rwbuff, size);
}






static void fusb_read_task_TEST(fusb_obj_t *obj)
{
    uint8_t buf[2];
    uint16_t addr = 0;
    uint16_t size = 0;
    int32_t rc = -1;
	uint32_t read_len  = 100;

    if(NULL == obj || NULL == obj->rwbuff || obj->fd < 0)
	{
        return;
    }
	
    rc = wos_uart_read(obj->fd, buf, read_len, FUSB_FRAME_TOUT);
	
    if(rc <= 0)
	{
        return;
    }
	printf("UART RX %d bytes: ", rc);
	for(int i = 0; i < rc; i++)
	{
		printf("%#x ", buf[i]);
	}
	printf("\r\n");
	
    if(FUSB_HOST2HUB != buf[0])
	{
        WLOGW("SID wrong, %#x\r\n", buf[0]);
		
        return;
    }
	
    rc = wos_uart_read(obj->fd, buf, 2, FUSB_FRAME_TOUT);
	
    if(2 != rc)
	{
        WLOGW("Loss addr\r\n");
		
        return;
    }
	
    addr = ((buf[0] << 8) | buf[1]);
    rc = wos_uart_read(obj->fd, buf, 2, FUSB_FRAME_TOUT);
	
    if(2 != rc)
	{
        WLOGW("Loss size\r\n");
		
        return;
    }
    size = ((buf[0] << 8) | buf[1]);
    if(0 != size)
	{
        rc = wos_uart_read(obj->fd, obj->rwbuff, size, FUSB_FRAME_TOUT);
		
        if(size != rc)
		{
            WLOGW("Loss data\r\n");
			
            return;
        }
    }
	
    rc = wos_uart_read(obj->fd, buf, 2, FUSB_FRAME_TOUT);
	
    if(2 != rc)
	{
        WLOGW("Loss eot\r\n");
		
        return;
    }
	
    if(FUSB_EOT != buf[0] || FUSB_EOT != buf[1])
	{
        WLOGW("EOT ERR. %#x, %#x\r\n", buf[0], buf[1]);
		
        return;
    }
	
    fusb_handler(obj, addr, obj->rwbuff, size);
}




/********************************************************************************************
 *	接口：将采样数据帧压入环形缓存队列
 *
********************************************************************************************/
// 将一帧新的采样数据入队到 FUSB 主环形缓冲区中
// 参数说明：
//   spdata : 指向待入队的原始采样数据（来自传感器驱动）
//   size   : 该帧数据的有效字节数
//   nodes  : 本帧涉及的有效节点数量（用于上位机解析）
// 返回值：0 成功，-1 失败（缓冲区满）
int32_t fusb_ringbuf_enqueue(uint8_t *spdata, uint32_t size, uint8_t nodes)
{
    fusb_obj_t *obj = fusb_obj();
    
    // acquire 读 front，确保读到消费者最新的释放位置
    uint32_t front = atomic_load_explicit(&obj->front, memory_order_acquire);
    // relaxed 读 tail，因为只有本线程写 tail
    uint32_t tail = atomic_load_explicit(&obj->tail, memory_order_relaxed);
    
    if (front == ((tail + 1) % FUSB_PKT_BUFSIZE)) {
        WLOGW("buf full\r\n");
        return -1;
    }
    
    fusb_pkt_t *pkt = &obj->pkt[tail];
    if (size > sizeof(pkt->spdata)) {
        WLOGW("enqueue size %u too large\r\n", size);
        return -1;
    }

    pkt->nodes = nodes;
    pkt->size = size;
    memcpy(pkt->spdata, spdata, size);

    /*
    // ⭐ 新增：立即回读校验
    const uint8_t *p = (const uint8_t *)pkt->spdata;
    uint32_t zero_run = 0;
    for (uint32_t i = 0; i < size; i++) {
        if (p[i] == 0x00) {
            zero_run++;
            if (zero_run >= 3) {
                // 发现连续3个0，记录位置并告警
                WLOGE("MEMCPY VERIFY FAIL: 3+ consecutive zeros at offset %u (size=%u)\r\n", 
                    i - 2, size);
                
                // 可选：打印前后上下文帮助定位
                // dump_hex(pkt->spdata, size); 
                break;
            }
        } else {
            zero_run = 0;
        }
    }*/

    
    // release 写 tail：保证 memcpy 完成后，消费者才能看到新的 tail
    atomic_store_explicit(&obj->tail, (tail + 1) % FUSB_PKT_BUFSIZE, memory_order_release);

#ifdef FNIRS_EV_IO
    if (obj->ev_mode) {
        pthread_mutex_lock(&obj->tx_wait_mtx);
        pthread_cond_signal(&obj->tx_wait_cond);
        pthread_mutex_unlock(&obj->tx_wait_mtx);
    }
#endif
    
    return 0;
}




/********************************************************************************************
 *	接口：从环形缓存队列取出一个采样数据帧
 *  功能：从 fusb_obj_t 的主采样数据环形缓冲区中取出最早入队的一帧数据（FIFO）
 *  线程安全说明：假设仅由单个消费者线程（fusb_task）调用，因此无需加锁
 ********************************************************************************************/
static fusb_pkt_t* fusb_ringbuf_dequeue(fusb_obj_t *obj)
{
    // relaxed 读 front，因为只有本线程写 front
    uint32_t front = atomic_load_explicit(&obj->front, memory_order_relaxed);
    // ⭐ acquire 读 tail：保证读到新 tail 后，一定能读到生产者写入的最新数据
    uint32_t tail = atomic_load_explicit(&obj->tail, memory_order_acquire);
    
    if (front == tail) {
        return NULL;
    }
    
    fusb_pkt_t *pkt = &obj->pkt[front];
    
    // release 写 front：保证对 pkt 的读取完成后，生产者才能复用该槽位
    atomic_store_explicit(&obj->front, (front + 1) % FUSB_PKT_BUFSIZE, memory_order_release);
    
    return pkt;
}





/********************************************************************************************
 * 功能：采样数据帧通过串口转发
 * 	1.从队列中取出一个数据；
 *	2.按协议打包后通过串口发送；
 * 
********************************************************************************************/
static void fusb_spdata_handler(fusb_obj_t *obj)
{
    fusb_pkt_t *pkt;
    int count = 0;

    do {
        pkt = fusb_ringbuf_dequeue(obj);
        if (!pkt)
            break;

        if (pkt->size > sizeof(pkt->spdata) || pkt->size > FUSB_RWBUFF_SIZE - 8) {
            WLOGW("spdata bad size %u\r\n", pkt->size);
            continue;
        }

#if defined(FNIRS_EMBEDDED) && !defined(FNIRS_EV_IO)
        (void)obj;
        continue;
#endif

            fusb_write(obj, FUSB_SPDATA_WITH_TIME,
                       (uint8_t *)pkt, pkt->size + 1);
#ifdef FNIRS_EV_IO
            if (obj->ev_mode && (count == 0)) {
                static uint32_t sp_tx;

                if ((++sp_tx % 40) == 1)
                    WLOGI("fusb SPDATA tx #%u nodes=%u size=%u\r\n",
                          sp_tx, pkt->nodes, pkt->size);
            }
#endif
    } while (++count < 20);
}

#ifdef FNIRS_EV_IO
static void *fusb_ev_tx_task(void *args)
{
    fusb_obj_t *obj = args;

    while (obj->run) {
        fusb_spdata_handler(obj);

        pthread_mutex_lock(&obj->tx_wait_mtx);
        if (obj->run &&
            atomic_load_explicit(&obj->front, memory_order_acquire) ==
            atomic_load_explicit(&obj->tail, memory_order_acquire))
            pthread_cond_wait(&obj->tx_wait_cond, &obj->tx_wait_mtx);
        pthread_mutex_unlock(&obj->tx_wait_mtx);
    }

    return NULL;
}
#endif


//工厂模式下上报数据环形缓存接口——取数据
fusb_factory_mode_pkg_t* fusb_factory_ringbuf_dequeue(fusb_obj_t *obj)
{
    fusb_factory_mode_pkg_t *pkt = NULL;
    uint32_t r_index = obj->q_read_index;
    uint32_t w_index = obj->q_write_index;

    if(r_index == w_index)
	{
        return NULL; //empty
    }
	
    pkt = &obj->factory_mode_notify_queue[r_index];
    obj->q_read_index = ((r_index + 1) % FUSB_FACTORY_PKT_BUF_COUNT);

    return pkt;
}

//工厂模式下上报数据环形缓存接口——添加数据
int32_t fusb_factory_ringbuf_enqueue(uint8_t *data, uint32_t size)
{
    fusb_obj_t *obj = fusb_obj();
    fusb_factory_mode_pkg_t *pkt = NULL;
    uint32_t r_index = 0;
    uint32_t w_index = 0;

    r_index = obj->q_read_index;
    w_index = obj->q_write_index;

    if(r_index == ((w_index + 1) % FUSB_FACTORY_PKT_BUF_COUNT))
	{
        WLOGW("buf full\r\n");
		
        return -1;
    }
	
    pkt = &obj->factory_mode_notify_queue[w_index];

    if (size > sizeof(pkt->buf)) {
        WLOGW("factory enqueue size %u too large\r\n", size);
        return -1;
    }

	//WLOGW("enqueue [%d]B, at [%d]\r\n", size, w_index);

	//填充节点数据
    pkt->size = size;
    memcpy(pkt->buf, data, size);

	//移动写索引
    obj->q_write_index = ((w_index + 1) % FUSB_FACTORY_PKT_BUF_COUNT);

	//WLOGW("fusb_ringbuf_enqueue(, %d, %d) -- [%d --> %d]\r\n", size, nodes, r_index, w_index);
	
    return 0;
}

static void fusb_factory_cmd_data_handler(fusb_obj_t *obj)
{
	int count = 0;
	do
	{
		uint16_t addr;
		int32_t rc;

		//从队列中取出一个数据
	    fusb_factory_mode_pkg_t *pkt = fusb_factory_ringbuf_dequeue(obj);

	    if(NULL == pkt)
		{
	        return;
	    }

	    if (pkt->size < 2 || pkt->size > sizeof(pkt->buf)) {
	        WLOGW("factory bad pkt size %u\r\n", pkt->size);
	        return;
	    }

		//按协议打包后通过串口发送
		addr = ((uint16_t)pkt->buf[0] << 8) |
			(uint16_t)pkt->buf[1];
		rc = fusb_write(obj, addr, &pkt->buf[2], pkt->size - 2);
		if (addr == FUSB_OTA_RESULT)
			WLOGI("OTA RESULT sent payload=%u rc=%d\r\n",
			      pkt->size - 2, rc);

		count++;
	}while(count < 20);
}

/********************************************************************************************
 *	串口监控任务
 *	功能：
 *	1.发送心跳命令包；
 *	2.读取PC上位机软件的命令；
 *	3.在采样过程中及时将采样数据帧发送出去
 *  4.检测电池电压
********************************************************************************************/
static void *fusb_task(void *args)
{
    fusb_obj_t *obj = (fusb_obj_t *)args;

    while(obj->run)
	{		
        usleep(20 * 1000);
		//usleep(5 * 1000);
		
        if(obj->fd < 0)
		{
            fusb_open(obj);
            continue;
        }

		//心跳数据上报
        fusb_hbeat_handler(obj);

		//获取PC上位机下发的命令
        fusb_read_handler(obj);

		//采样数据上报
        fusb_spdata_handler(obj);

        //检查电池电压,huang
        fusb_voltage_detect_handler(obj);

		//工厂模式测试结果数据上报
		fusb_factory_cmd_data_handler(obj);
    }
	
    pthread_exit(NULL);
}



/********************************************************************************************
 *  初始化串口通信任务
 ********************************************************************************************/
int32_t fusb_init(void)
{
    // 打印初始化日志，标识 fusb 模块开始初始化
    printf("fusb_init() -- \r\n");
    
    // 声明线程属性变量，用于配置新线程的栈大小等参数
    pthread_attr_t thread_attr;
    int ret;
    
    // 计算线程所需栈大小：
    // - 存放 FUSB_PKT_BUFSIZE 个采样数据包（用于可能的局部拷贝或深度调用）
    // - 加上读写缓冲区大小（FUSB_RWBUFF_SIZE）
    // - 再额外预留 64KB 作为安全余量，防止栈溢出
    size_t stacksize = sizeof(fusb_pkt_t) * FUSB_PKT_BUFSIZE + FUSB_RWBUFF_SIZE + 64 * 1024;
    
    // 将栈大小向上对齐到 4KB 页边界（Linux pthread 要求栈大小必须页对齐）
    stacksize = (stacksize + 4095) & ~4095;          /* 页对齐 */
    
    // 获取全局唯一的 fusb_obj_t 单例对象指针
    fusb_obj_t *obj = fusb_obj();
    
    // 将整个 fusb_obj_t 结构体清零，确保所有成员（指针、计数器、fd 等）处于初始状态
    memset(obj, 0, sizeof(fusb_obj_t));
    pthread_mutex_init(&obj->write_mtx, NULL);
    pthread_mutex_init(&obj->tx_wait_mtx, NULL);
    pthread_cond_init(&obj->tx_wait_cond, NULL);

    atomic_store_explicit(&obj->front, 0, memory_order_relaxed);
    atomic_store_explicit(&obj->tail, 0, memory_order_relaxed);


    // 动态分配主采样数据环形缓冲区：共 FUSB_PKT_BUFSIZE 个 fusb_pkt_t 数据包
    obj->pkt = (fusb_pkt_t *)malloc(sizeof(fusb_pkt_t) * FUSB_PKT_BUFSIZE);
    
    // 检查主数据缓冲区是否分配成功
    if (NULL == obj->pkt)
    {
        // 分配失败，打印警告日志
        WLOGW("Failed to allocate pkt buffer\r\n");
        
        // 返回“内存不足”错误码
        return -ENOMEM;
    }

    // 动态分配工厂模式专用通知队列：共 FUSB_FACTORY_PKT_BUF_COUNT 个工厂测试包
    obj->factory_mode_notify_queue = (fusb_factory_mode_pkg_t *)malloc(sizeof(fusb_factory_mode_pkg_t) * FUSB_FACTORY_PKT_BUF_COUNT);
    
    // 检查工厂队列是否分配成功
    if (NULL == obj->factory_mode_notify_queue)
    {
        // 分配失败，打印警告日志
        WLOGW("Failed to allocate FACTORY pkt buffer\r\n");
        
        // 返回“内存不足”错误码
        return -ENOMEM;
    }
    
    // 初始化工厂模式环形队列的读指针（消费者位置）
    obj->q_read_index = 0;
    
    // 初始化工厂模式环形队列的写指针（生产者位置）
    obj->q_write_index = 0;

    // 动态分配临时读写缓冲区，用于串口协议解析与组帧（如接收命令、组装发送帧）
    obj->rwbuff = (uint8_t *)malloc(FUSB_RWBUFF_SIZE);
    
    // 检查读写缓冲区是否分配成功
    if (NULL == obj->rwbuff)
    {
        // 分配失败，打印警告日志
        WLOGW("Failed to allocate rw buffer\r\n");
        
        // 释放已分配的主数据缓冲区，避免内存泄漏
        free(obj->factory_mode_notify_queue);
        free(obj->pkt);
    
        // 返回“内存不足”错误码
        return -ENOMEM;
    }

    // 发送线程使用独立缓冲区，避免与事件线程的串口接收缓冲区并发冲突。
    obj->txbuff = (uint8_t *)malloc(FUSB_RWBUFF_SIZE);
    if (NULL == obj->txbuff)
    {
        WLOGW("Failed to allocate tx buffer\r\n");
        free(obj->rwbuff);
        free(obj->factory_mode_notify_queue);
        free(obj->pkt);
        return -ENOMEM;
    }

    // 将主采样数据缓冲区内容清零（增强鲁棒性，避免残留垃圾数据）
    memset(obj->pkt, 0, sizeof(fusb_pkt_t) * FUSB_PKT_BUFSIZE);
    
    // 设置线程运行标志为 1，表示允许 fusb_task 线程持续运行
    obj->run = 1;
    
    // 初始化串口文件描述符为 -1，表示当前未打开串口设备
    obj->fd   = -1;

    wos_timer_countdown_ms(&obj->hb_tm, 3000);
    wos_timer_countdown_ms(&obj->vd_tm, 5000);

#ifdef FNIRS_EV_IO
    obj->ev_mode = 1;
    pthread_attr_init(&thread_attr);
    pthread_attr_setstacksize(&thread_attr, stacksize);
    ret = pthread_create(&obj->tid, &thread_attr, fusb_ev_tx_task, obj);
    pthread_attr_destroy(&thread_attr);
    if (ret != 0) {
        WLOGW("Failed to create fusb event TX thread\r\n");
        obj->run = 0;
        free(obj->factory_mode_notify_queue);
        free(obj->pkt);
        free(obj->rwbuff);
        free(obj->txbuff);
        return -EAGAIN;
    }
    pthread_setname_np(obj->tid, "fusb_ev_tx");
    WLOGI("fusb_init: event I/O mode with background TX\r\n");
    return 0;
#endif

    // 1. 初始化线程属性对象（必须先 init 才能设置属性）
    pthread_attr_init(&thread_attr);
    
    // 2. 设置新线程的栈大小（必须页对齐，否则 pthread_create 可能失败）
    pthread_attr_setstacksize(&thread_attr, stacksize);
    
    // 创建 fusb_task 后台线程，并将 obj 作为线程入口函数的参数传入
    ret = pthread_create(&obj->tid, &thread_attr, fusb_task, obj);
    
    // 立即销毁线程属性对象（线程已拷贝所需配置，attr 不再需要，及时释放资源）
    pthread_attr_destroy(&thread_attr);		  /* 3. 立即销毁属性，线程已拷贝所需数据 */

    // 检查线程创建是否成功（ret != 0 表示失败）
    if (0 != ret)
    {
        // 创建失败，打印警告日志
        WLOGW("Failed to create fusb_task thread\r\n");
        
        // 释放已分配的主数据缓冲区
        free(obj->pkt);
        
        // 释放已分配的读写缓冲区
        free(obj->rwbuff);

        free(obj->txbuff);
        free(obj->factory_mode_notify_queue);
        
        // 返回“资源暂时不可用”错误码（通常因系统资源不足）
        return -EAGAIN;
    }

    // 为新建线程设置名称为 "fusb"，便于在调试工具（如 top、htop、gdb）中识别
    pthread_setname_np(obj->tid, "fusb");   /* 调试友好 */
    
    // 初始化成功，返回 0 表示无错误
    return 0;
}





int32_t fusb_exit(void)
{
    fusb_obj_t *obj = fusb_obj();

    obj->run = 0;

#ifdef FNIRS_EV_IO
    if (obj->ev_mode) {
        pthread_mutex_lock(&obj->tx_wait_mtx);
        pthread_cond_signal(&obj->tx_wait_cond);
        pthread_mutex_unlock(&obj->tx_wait_mtx);
        pthread_join(obj->tid, NULL);
        fusb_ev_detach_fd();
        goto free_bufs;
    }
#endif

    pthread_join(obj->tid, NULL);

free_bufs:
	
    if(NULL != obj->pkt)
	{
        free(obj->pkt);
        obj->pkt = NULL;
    }
	
    if(NULL != obj->rwbuff)
    {
        free(obj->rwbuff);
        obj->rwbuff = NULL;
    }
    if(NULL != obj->txbuff)
    {
        free(obj->txbuff);
        obj->txbuff = NULL;
    }
    if(NULL != obj->factory_mode_notify_queue)
    {
        free(obj->factory_mode_notify_queue);
        obj->factory_mode_notify_queue = NULL;
    }
    pthread_cond_destroy(&obj->tx_wait_cond);
    pthread_mutex_destroy(&obj->tx_wait_mtx);
    pthread_mutex_destroy(&obj->write_mtx);
    return 0;
}

#ifdef FNIRS_EV_IO
void fusb_ev_attach_fd(int32_t fd)
{
    fusb_obj_t *obj = fusb_obj();

    if (!obj->ev_mode)
        return;

    if (obj->fd >= 0 && obj->fd != fd)
        wos_uart_close(obj->fd);

    obj->rxlen = 0;
    obj->fd = fd;
}

void fusb_ev_detach_fd(void)
{
    fusb_obj_t *obj = fusb_obj();

    if (!obj->ev_mode)
        return;

    if (obj->fd >= 0) {
        wos_uart_close(obj->fd);
        obj->fd = -1;
    }
    obj->rxlen = 0;
}

void fusb_ev_poll(void)
{
    fusb_obj_t *obj = fusb_obj();

    if (!obj->ev_mode || !obj->run || obj->fd < 0)
        return;

    fusb_ev_trace("poll");
    fusb_ev_trace("hb");
    fusb_hbeat_handler(obj);
    fusb_ev_trace("voltage");
    fusb_voltage_detect_handler(obj);
    fusb_ev_trace("factory");
    fusb_factory_cmd_data_handler(obj);
    fusb_ev_trace("poll done");
}

void fusb_ev_process_read(void)
{
    fusb_obj_t *obj = fusb_obj();

    if (!obj->ev_mode || !obj->run || obj->fd < 0)
        return;

    fusb_read_handler(obj);
}

void fusb_ev_drain_read(void)
{
    int i;

    for (i = 0; i < 32; i++)
        fusb_ev_process_read();
}
#endif




static void fusb_handler_reset(fusb_obj_t *obj, uint8_t *data, uint16_t size)
{
    int8_t rc = -1;
	
    rc = fNIRS_reset();
	
    fusb_write(obj, FUSB_RESET, (uint8_t *)&rc, 1);
}





// 定义扫描响应缓冲区的最大大小（字节），用于存放所有节点描述信息
#define FUSB_SCAN_BUFSIZE   512

/*
 * FUSB_SCAN 的线上记录保持旧版20字节布局：
 * [nid:4][mcu_hw:4][mcu_sw:4][dock_uid:8]。
 * fnode_desc_t 可以继续扩展，但不能隐式改变已经发布的上位机协议。
 */
typedef struct {
    uint8_t node_mcu_hw[4];
    uint8_t node_mcu_sw[4];
    uint8_t dock_uid[8];
} fusb_scan_desc_t;

// 处理上位机发来的“扫描设备”命令（FUSB_SCAN）
// 功能：查询当前系统中所有存活的 fNIRS 节点，并返回其基本信息
static void fusb_handler_scan(fusb_obj_t *obj, uint8_t *data, uint16_t size)
{
    // 声明一个本地数组，用于存储最多 FNODE_NID_MAX 个节点的描述信息
    // fnode_desc_t 包含节点 ID、类型、传感器配置、边界信息等
    fnode_desc_t desc[FNODE_NID_MAX];
    
    // 定义临时缓冲区 buf，用于组装要返回给上位机的数据帧
    // +1 是为了确保即使计算有微小误差也不会越界（安全余量）
    uint8_t buf[FUSB_SCAN_BUFSIZE+1]; //more than FNODE_NID_MAX * offsetof(fnode_desc_t, border)
    
    // 计算每个节点在响应帧中占用的字节数：
    // - offsetof(fnode_desc_t, border)：取 fnode_desc_t 中成员 'border' 的偏移量，
    //   表示我们只传输到 'border' 之前的数据（可能出于兼容性或效率考虑）
    // - +4：额外预留 4 字节用于存放节点编号（nid）
    uint16_t unit = sizeof(fusb_scan_desc_t) + 4;
    
    // 记录实际存活的节点数量（将作为响应帧的第一个字节）
    uint8_t nodes = 0;
    
    // 循环索引变量
    uint32_t i = 0;
    
    // 临时变量，用于存储节点编号（nid = index + 1）
    uint32_t nid = 0;
    
    // 函数返回码，初始化为错误状态
    int32_t rc = -1;

    // 检查：如果所有节点所需总空间超过缓冲区上限，则报错并退出
    if(unit * FNODE_NID_MAX > FUSB_SCAN_BUFSIZE)
    {
        // 打印致命错误日志（FATAL 级别），说明缓冲区太小
        WLOGF("Need more buf\r\n");
        
        // 直接返回，不发送任何有效数据
        return;
    }
    
    // 调用底层驱动函数 fNIRS_desc()，获取所有节点的描述信息
    // 成功时返回 0，失败时返回负值（如硬件未就绪）
    rc = fNIRS_desc(desc);
    
    // 如果获取节点描述失败
    if(0 != rc)
    {
        // 仍需向上位机回复一个合法的 FUSB_SCAN 响应帧：
        // 内容为 1 字节：nodes = 0（表示无可用节点）
        fusb_write(obj, FUSB_SCAN, &nodes, 1);
        
        // 提前返回
        return;
    }
    
    // 遍历所有可能的节点（0 到 FNODE_NID_MAX-1）
    for(i = 0; i < FNODE_NID_MAX; i++)
    {
        fusb_scan_desc_t wire_desc;

        // 如果当前节点未激活（alive == 0），跳过
        if(0 == desc[i].alive)
        {
            continue;
        }
        
        // 节点编号从 1 开始（协议约定），所以 nid = i + 1
        nid = i + 1;
        
        // 将节点编号（nid，4 字节）拷贝到 buf 中对应位置：
        // - buf[0] 保留给总节点数
        // - 每个节点数据块起始偏移 = 1 + nodes * unit
        // - 先写 nid（4 字节）
        memcpy(buf + 1 + nodes * unit, &nid, 4);

        memcpy(wire_desc.node_mcu_hw, desc[i].node_mcu_hw, 4);
        memcpy(wire_desc.node_mcu_sw, desc[i].node_mcu_sw, 4);
        memcpy(wire_desc.dock_uid, desc[i].dock_uid, 8);
        memcpy(buf + 1 + nodes * unit + 4,
               &wire_desc, sizeof(wire_desc));
        
        // 存活节点计数加一
        nodes++;
    }
    
    // 将实际存活节点数写入响应帧的第一个字节
    buf[0] = nodes;
    
    // 向上位机发送完整的 FUSB_SCAN 响应：
    // - 帧内容：buf[0] = 节点数，后续为 nodes 个节点的数据块
    // - 总长度 = 1（节点数） + nodes * unit（每个节点 unit 字节）
    WLOGI("FUSB_SCAN reply: nodes=%u unit=%u size=%u\r\n",
          nodes, unit, 1 + nodes * unit);
    fusb_write(obj, FUSB_SCAN, buf, nodes * unit + 1);
}



//设置LED发光序列
static void fusb_handler_set_sample_array(fusb_obj_t *obj, uint8_t *data, uint16_t size)
{   
    int32_t rc = -1;

    rc = fnode_set_array(data,size);

    fusb_write(obj, FUSB_SET_SAMPLE_ARRAY, (uint8_t *)&rc, 4);

    return rc;
}





//设置LED灯发光电流--命令处理
static void fusb_handler_s_gain(fusb_obj_t *obj, uint8_t *data, uint16_t size)
{
    int8_t rc = -1;

    if(NULL == data || size < 1)
	{
        WLOGW("Loss gain\r\n");
		
        return;
    }
	
    rc = fNIRS_s_gain(data[0]);
	
    fusb_write(obj, FUSB_S_GAIN, (uint8_t *)&rc, 1);
}



static void fusb_handler_sample_on(fusb_obj_t *obj, uint8_t *data, uint16_t size)
{
    int8_t rc = -1;
	
    rc = fNIRS_on();
	
    fusb_write(obj, FUSB_SAMPLE_ON, (uint8_t *)&rc, 1);
}

static void fusb_handler_sample_off(fusb_obj_t *obj, uint8_t *data, uint16_t size)
{
    int8_t rc = -1;
	
    rc = fNIRS_off();
	
    fusb_write(obj, FUSB_SAMPLE_OFF, (uint8_t *)&rc, 1);
}




static void fusb_handler_ntpdate(fusb_obj_t *obj, uint8_t *data, uint16_t size)
{
    uint64_t utc = 0;
    int8_t rc = -1;

    // 检查输入：必须是非空指针且数据长度为 8 字节
    if (NULL == data || size != 8) {
        WLOGW("invalid utc data: size=%" PRIu16 " (expected 8)\n", size);
        rc = -1; // 错误码：参数无效
        fusb_write(obj, FUSB_NTPDATE, (uint8_t *)&rc, 1);
        return;
    }

    // 从 data[0..7] 按大端序（Big-Endian）解析 64 位时间戳
    utc = ((uint64_t)data[0] << 56) |
          ((uint64_t)data[1] << 48) |
          ((uint64_t)data[2] << 40) |
          ((uint64_t)data[3] << 32) |
          ((uint64_t)data[4] << 24) |
          ((uint64_t)data[5] << 16) |
          ((uint64_t)data[6] << 8)  |
          ((uint64_t)data[7] << 0);

    // 调用 64 位时间设置函数
    int32_t ret = wos_rtc_set(utc);
    rc = (ret == 0) ? 0 : (int8_t)ret; // 成功返回 0，失败返回错误码（截断为 int8_t）

    // 回复主机：发送 1 字节结果状态
    fusb_write(obj, FUSB_NTPDATE, (uint8_t *)&rc, 1);

    // 打印日志（便于调试）
    if (rc == 0) {
        WLOGI("NTP time set successfully: UTC %" PRIu64 "\n", utc);
    } else {
        WLOGE("Failed to set NTP time: UTC %" PRIu64 ", error=%" PRId8 "\n", utc, rc);
    }
}




//工厂测试模式进入/离开
static void fusb_FUSB_FACTORY_MODE_handler(fusb_obj_t *obj, uint8_t *data, uint16_t size)
{
    int8_t rc = -1;
	
    if(NULL == data || size != 1)
	{
        WLOGW("invalid factory mode\r\n");
		
        return;
    }
	
    rc = fNIRS_factory_test_mode_enter(data[0]);
	if(rc == 0)
		rc = data[0];
	
    fusb_write(obj, FUSB_FACTORY_MODE, (uint8_t *)&rc, 1);
}




//单通道3厘米发射与接收
static void fusb_FUSB_MEASURE_handler(fusb_obj_t *obj, uint8_t *data, uint16_t size)
{//<NIDx, LED, 0/1, mA, NIDy, PD, sample_interval, sample_count>
	int8_t rc = -1;
	
	if(NULL == data || size != 14)
	{
        WLOGW("invalid factory mode -- measure\r\n");
		
        return;
    }

	rc = fNIRS_factory_test_mode_FUSB_MEASURE(data, size);
	
	fusb_write(obj, FUSB_MEASURE, (uint8_t *)&rc, 1);
}



//单通道3厘米 735nm 和 850nm 交替发射与接受，huang
static void fusb_FUSB_MEASURE_ALTERNATE_handler(fusb_obj_t *obj, uint8_t *data, uint16_t size)
{//<NIDx, LED, mA, NIDy, PD, sample_interval, sample_count>
	int8_t rc = -1;
	
	if(NULL == data || size != 13)
	{
        WLOGW("invalid factory mode -- measure alternate\r\n");
		
        return;
    }

	rc = fNIRS_factory_test_mode_FUSB_MEASURE_ALTERNATE(data, size);
	
	fusb_write(obj, FUSB_MEASURE_ALTERNATE, (uint8_t *)&rc, 1);
}






//校准PD OFFSET
static void fusb_FUSB_CALI_PD_OFFSET_handler(fusb_obj_t *obj, uint8_t *data, uint16_t size)
{
	int8_t rc = -1;
	
	if(NULL == data || size != 1)
	{
        WLOGW("invalid factory mode -- cali pd\r\n");
		
        return;
    }

	rc = fNIRS_factory_test_mode_FUSB_CALI_PD(data, size);
	
	fusb_write(obj, FUSB_CALI_PD_OFFSET, (uint8_t *)&rc, 1);
}

//单光源周边1厘米距离PD接收一致性???
static void fusb_FUSB_CHK_PD_UNIFORMITY_handler(fusb_obj_t *obj, uint8_t *data, uint16_t size)
{
}

static void fusb_FUSB_LED_ON_handler(fusb_obj_t *obj, uint8_t *data, uint16_t size)
{//<NIDx，LED, 0/1, mA, onoff>
	int8_t rc = -1;
	
	if(NULL == data || size != 5)
	{
        WLOGW("invalid factory mode -- led\r\n");
		
        return;
    }

	rc = fNIRS_factory_test_mode_LED_ON(data, size);
	
	fusb_write(obj, FUSB_LED_ON, (uint8_t *)&rc, 1);
}

//2025-12-17 maomao add fNIRS_factory_test_mode_LED_ON_FUSB_MEASURE
static void fusb_FUSB_LED_ON_MEASURE_handler(fusb_obj_t *obj, uint8_t *data, uint16_t size)
{//<NIDx, LED, 0/1, mA, NIDy, PD, sample_interval, sample_count>
	int8_t rc = -1;
	
	if(NULL == data || size != 14)
	{
        WLOGW("invalid factory mode -- led_on measure\r\n");
		
        return;
    }

	rc = fNIRS_factory_test_mode_LED_ON_FUSB_MEASURE(data, size);
	
	fusb_write(obj, FUSB_LED_ON_MEASURE, (uint8_t *)&rc, 1);
}

//查询控制器SN号
static void fusb_FUSB_HUB_SN_handler(fusb_obj_t *obj, uint8_t *data, uint16_t size)
{
}

//查询节点SN号
static void fusb_FUSB_NODE_SN_handler(fusb_obj_t *obj, uint8_t *data, uint16_t size)
{
}






static uint32_t fusb_ota_get_be32(const uint8_t *data)
{
	return ((uint32_t)data[0] << 24) |
	       ((uint32_t)data[1] << 16) |
	       ((uint32_t)data[2] << 8) |
	       (uint32_t)data[3];
}

static int fusb_ota_filename_valid(const uint8_t *name, size_t len)
{
	size_t i;

	if (!name || len == 0 || len > OTA_FILE_DOWNLAOD_MAX_LEN)
		return 0;

	for (i = 0; i < len; i++) {
		if (name[i] == '\0' || name[i] == '/' || name[i] == '\\')
			return 0;
	}

	if (len < 5 || memcmp(name + len - 4, ".bin", 4) != 0)
		return 0;

	return 1;
}

static int fusb_ota_make_path(char *path, size_t path_size,
			      const uint8_t *name, size_t name_len)
{
	static const char prefix[] = "/opt/golgi/";

	if (!path || path_size < sizeof(prefix) + name_len ||
	    !fusb_ota_filename_valid(name, name_len))
		return -1;

	memcpy(path, prefix, sizeof(prefix) - 1);
	memcpy(path + sizeof(prefix) - 1, name, name_len);
	path[sizeof(prefix) - 1 + name_len] = '\0';
	return 0;
}

static void fusb_ota_close_download(void)
{
	if (ota_info.downlaod_bin_fd >= 0) {
		close(ota_info.downlaod_bin_fd);
		ota_info.downlaod_bin_fd = -1;
	}
}

static void fusb_ota_abort_download(int remove_partial)
{
	fusb_ota_close_download();
	if (remove_partial && ota_info.bin_path[0] != '\0')
		unlink(ota_info.bin_path);
	ota_info.download_bin_offset = 0;
	ota_info.download_bin_size = 0;
	ota_info.download_bin_crc = 0;
	ota_info.download_bin_filename[0] = '\0';
	ota_info.bin_path[0] = '\0';
	ota_info.status = FNODE_OTA_STATUS_IDLE;
}

static int fusb_ota_pwrite_all(int fd, const uint8_t *data, uint32_t len,
			       uint32_t offset)
{
	uint32_t written = 0;

	while (written < len) {
		ssize_t rc = pwrite(fd, data + written, len - written,
				   (off_t)offset + written);

		if (rc > 0) {
			written += (uint32_t)rc;
			continue;
		}
		if (rc < 0 && errno == EINTR)
			continue;
		return -1;
	}

	return 0;
}

static uint16_t fusb_ota_crc16(const uint8_t *data, uint32_t len)
{
	return my_crc16((char *)data, (int)len);
}

//OTA命令处理：OTA BIN文件信息
static void fusb_FUSB_OTA_FILE_START_handler(fusb_obj_t *obj, uint8_t *data, uint16_t size)
{
	uint32_t file_size;
	uint32_t file_crc;
	uint32_t cmd_crc;
	uint16_t crc_calc;
	uint8_t filename_len;
	int8_t rc;
	int fd;

	if (!data || size < 1) {
		rc = FNODE_OTA_ERROR_FILE_NAME_SIZE;
		goto reply;
	}

	filename_len = data[0];
	if (filename_len == 0 || filename_len > OTA_FILE_DOWNLAOD_MAX_LEN) {
		rc = FNODE_OTA_ERROR_FILE_NAME_SIZE;
		goto reply;
	}
	if (size != (uint16_t)(9U + filename_len + 4U)) {
		rc = FNODE_OTA_ERROR_FILE_CMD_CRC;
		goto reply;
	}
	if (!fusb_ota_filename_valid(data + 9, filename_len)) {
		rc = FNODE_OTA_ERROR_FILE_NAME_SIZE;
		goto reply;
	}

	cmd_crc = fusb_ota_get_be32(data + 9 + filename_len);
	crc_calc = fusb_ota_crc16(data, 9U + filename_len);
	if ((uint16_t)cmd_crc != crc_calc) {
		WLOGW("OTA FILE START command CRC error: rx=0x%04x calc=0x%04x\r\n",
		      (uint16_t)cmd_crc, crc_calc);
		rc = FNODE_OTA_ERROR_FILE_CMD_CRC;
		goto reply;
	}

	file_size = fusb_ota_get_be32(data + 1);
	file_crc = fusb_ota_get_be32(data + 5);
	if (file_size == 0 || file_size > OTA_IMAGE_MAX_SIZE) {
		rc = FNODE_OTA_ERROR_FIRMWARE_TOOLARGE;
		goto reply;
	}
	if (ota_info.status == FNODE_OTA_STATUS_START ||
	    ota_info.status == FNODE_OTA_STATUS_PACKAGE ||
	    ota_info.status == FNODE_OTA_STATUS_FINISH) {
		rc = FNODE_OTA_ERROR_MAX;
		goto reply;
	}

	fusb_ota_abort_download(1);
	if (fusb_ota_make_path(ota_info.bin_path, sizeof(ota_info.bin_path),
			       data + 9, filename_len) != 0) {
		rc = FNODE_OTA_ERROR_FILE_NAME_SIZE;
		goto reply;
	}

	fd = open(ota_info.bin_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		WLOGW("OTA FILE START open %s failed: %s\r\n",
		      ota_info.bin_path, strerror(errno));
		fusb_ota_abort_download(0);
		rc = FNODE_OTA_ERROR_FILE_CREATE;
		goto reply;
	}

	ota_info.downlaod_bin_fd = fd;
	ota_info.download_bin_size = file_size;
	ota_info.download_bin_crc = file_crc;
	ota_info.download_bin_offset = 0;
	memset(ota_info.download_bin_filename, 0,
	       sizeof(ota_info.download_bin_filename));
	memcpy(ota_info.download_bin_filename, data + 9, filename_len);
	ota_info.status = FNODE_OTA_STATUS_FILE_START;
	rc = FNODE_OTA_ERROR_NO;

	WLOGI("OTA FILE START ok path=%s size=%u crc=0x%04x\r\n",
	      ota_info.bin_path, file_size, (uint16_t)file_crc);

reply:
	fusb_write(obj, FUSB_OTA_FILE_START, (uint8_t *)&rc, 1);
}

//OTA命令处理：
static void fusb_FUSB_OTA_FILE_CANCEL_handler(fusb_obj_t *obj, uint8_t *data, uint16_t size)
{
	(void)data;
	(void)size;

	(void)fNIRS_ota_cancel();
	fusb_ota_abort_download(1);
	fusb_write(obj, FUSB_OTA_FILE_CANCEL, NULL, 0);
	WLOGI("OTA cancelled by host\r\n");
}

//OTA命令处理：
static void fusb_FUSB_OTA_FILE_PACKAGE_handler(fusb_obj_t *obj, uint8_t *data, uint16_t size)
{
	uint32_t offset;
	uint32_t package_len;
	uint32_t package_crc;
	uint32_t cmd_crc;
	uint16_t crc_calc;
	int8_t rc;

	if (!data || size < 16) {
		rc = FNODE_OTA_ERROR_FILE_CMD_CRC;
		goto reply;
	}
	if ((ota_info.status != FNODE_OTA_STATUS_FILE_START &&
	     ota_info.status != FNODE_OTA_STATUS_FILE_PACKAGE) ||
	    ota_info.downlaod_bin_fd < 0) {
		rc = FNODE_OTA_ERROR_FILE_OPEN;
		goto reply;
	}

	offset = fusb_ota_get_be32(data);
	package_len = fusb_ota_get_be32(data + 4);
	package_crc = fusb_ota_get_be32(data + 8);

	if (package_len == 0 ||
	    package_len > OTA_FILE_DOWNLOAD_PACKAGE_MAX_LEN) {
		rc = FNODE_OTA_ERROR_FILE_PACKAGE_TOOLARGE;
		goto reply;
	}
	if (size != (uint16_t)(12U + package_len + 4U)) {
		rc = FNODE_OTA_ERROR_FILE_CMD_CRC;
		goto reply;
	}
	if (offset > ota_info.download_bin_size ||
	    package_len > ota_info.download_bin_size - offset) {
		rc = FNODE_OTA_ERROR_OFFSET;
		goto reply;
	}

	cmd_crc = fusb_ota_get_be32(data + 12 + package_len);
	crc_calc = fusb_ota_crc16(data, 12U + package_len);
	if ((uint16_t)cmd_crc != crc_calc) {
		WLOGW("OTA FILE PACKAGE command CRC error at %u\r\n", offset);
		rc = FNODE_OTA_ERROR_FILE_CMD_CRC;
		goto reply;
	}

	crc_calc = fusb_ota_crc16(data + 12, package_len);
	if ((uint16_t)package_crc != crc_calc) {
		WLOGW("OTA FILE PACKAGE data CRC error at %u\r\n", offset);
		rc = FNODE_OTA_ERROR_FILE_PACKAGE_CRC;
		goto reply;
	}

	if (offset == ota_info.download_bin_offset) {
		if (fusb_ota_pwrite_all(ota_info.downlaod_bin_fd, data + 12,
					package_len, offset) != 0) {
			WLOGW("OTA FILE PACKAGE write at %u failed: %s\r\n",
			      offset, strerror(errno));
			rc = FNODE_OTA_ERROR_FILE_SAVEDATA;
			goto reply;
		}
		ota_info.download_bin_offset += package_len;
		ota_info.status = FNODE_OTA_STATUS_FILE_PACKAGE;
		rc = FNODE_OTA_ERROR_NO;
	} else if (offset < ota_info.download_bin_offset &&
		   package_len <= ota_info.download_bin_offset - offset) {
		/* The previous response was lost. ACK an already persisted packet. */
		WLOGI("OTA FILE PACKAGE duplicate offset=%u len=%u next=%u\r\n",
		      offset, package_len, ota_info.download_bin_offset);
		rc = FNODE_OTA_ERROR_NO;
	} else {
		WLOGW("OTA FILE PACKAGE unexpected offset=%u len=%u next=%u\r\n",
		      offset, package_len, ota_info.download_bin_offset);
		rc = FNODE_OTA_ERROR_OFFSET;
	}

reply:
	fusb_write(obj, FUSB_OTA_FILE_PACKAGE, (uint8_t *)&rc, 1);
}

//OTA命令处理：
static void fusb_FUSB_OTA_FILE_FINISH_handler(fusb_obj_t *obj, uint8_t *data, uint16_t size)
{
	off_t firmware_size;
	unsigned short firmware_crc;
	int8_t rc;

	(void)data;
	if (size != 0) {
		rc = FNODE_OTA_ERROR_FILE_CMD_CRC;
		goto reply;
	}
	if (ota_info.status == FNODE_OTA_STATUS_FILE_FINISH) {
		/* The previous success response may have been lost. */
		rc = FNODE_OTA_ERROR_NO;
		goto reply;
	}
	if ((ota_info.status != FNODE_OTA_STATUS_FILE_START &&
	     ota_info.status != FNODE_OTA_STATUS_FILE_PACKAGE) ||
	    ota_info.downlaod_bin_fd < 0) {
		rc = FNODE_OTA_ERROR_FILE_OPEN;
		goto reply;
	}

	if (ota_info.download_bin_offset != ota_info.download_bin_size) {
		rc = FNODE_OTA_ERROR_FILE_RX_SIZE;
		goto reply;
	}

	if (fsync(ota_info.downlaod_bin_fd) != 0) {
		WLOGW("OTA FILE FINISH fsync failed: %s\r\n", strerror(errno));
		rc = FNODE_OTA_ERROR_FILE_SAVEDATA;
		goto reply;
	}
	fusb_ota_close_download();

	if (calculate_bin_crc16(ota_info.bin_path, &firmware_size,
				&firmware_crc) != 0) {
		rc = FNODE_OTA_ERROR_FILE_CRC_CALC;
		goto reply;
	}
	if (firmware_size != (off_t)ota_info.download_bin_size) {
		rc = FNODE_OTA_ERROR_FILE_RX_SIZE;
		goto reply;
	}
	ota_info.firmware_bin_size = (unsigned int)firmware_size;
	ota_info.firmware_bin_crc = firmware_crc;

	if ((uint16_t)ota_info.download_bin_crc !=
	    (uint16_t)ota_info.firmware_bin_crc) {
		WLOGW("OTA FILE FINISH CRC mismatch rx=0x%04x calc=0x%04x\r\n",
		      (uint16_t)ota_info.download_bin_crc,
		      (uint16_t)ota_info.firmware_bin_crc);
		rc = FNODE_OTA_ERROR_FILE_CRC;
		goto reply;
	}

	ota_info.status = FNODE_OTA_STATUS_FILE_FINISH;
	rc = FNODE_OTA_ERROR_NO;
	WLOGI("OTA FILE FINISH ok path=%s size=%u crc=0x%04x\r\n",
	      ota_info.bin_path, ota_info.download_bin_size,
	      (uint16_t)ota_info.download_bin_crc);

reply:
	if (rc != FNODE_OTA_ERROR_NO) {
		fusb_ota_close_download();
		ota_info.status = FNODE_OTA_STATUS_IDLE;
	}
	fusb_write(obj, FUSB_OTA_FILE_FINISH, (uint8_t *)&rc, 1);
}

//OTA命令处理：
static void fusb_FUSB_OTA_NODES_START_handler(fusb_obj_t *obj, uint8_t *data, uint16_t size)
{
	struct stat st;
	char path[sizeof(ota_info.bin_path)];
	uint32_t cmd_crc;
	uint16_t crc_calc;
	uint16_t bitmap;
	uint8_t filename_len;
	int8_t rc;

	if (!data || size < 1) {
		rc = FNODE_OTA_ERROR_FILE_NAME_SIZE;
		goto reply;
	}

	filename_len = data[0];
	if (filename_len == 0 || filename_len > OTA_FILE_DOWNLAOD_MAX_LEN) {
		rc = FNODE_OTA_ERROR_FILE_NAME_SIZE;
		goto reply;
	}
	if (size != (uint16_t)(3U + filename_len + 4U)) {
		rc = FNODE_OTA_ERROR_FILE_CMD_CRC;
		goto reply;
	}
	if (!fusb_ota_filename_valid(data + 3, filename_len) ||
	    fusb_ota_make_path(path, sizeof(path), data + 3, filename_len) != 0) {
		rc = FNODE_OTA_ERROR_FILE_NAME_SIZE;
		goto reply;
	}

	cmd_crc = fusb_ota_get_be32(data + 3 + filename_len);
	crc_calc = fusb_ota_crc16(data, 3U + filename_len);
	if ((uint16_t)cmd_crc != crc_calc) {
		WLOGW("OTA_NODES_START command CRC error: rx=0x%04x calc=0x%04x\r\n",
		      (uint16_t)cmd_crc, crc_calc);
		rc = FNODE_OTA_ERROR_FILE_CMD_CRC;
		goto reply;
	}

	bitmap = (uint16_t)data[1] | ((uint16_t)data[2] << 8);
	if (bitmap == 0 || (bitmap & ~((1U << FNODE_NID_MAX) - 1U)) != 0) {
		rc = FNODE_OTA_ERROR_MAX;
		goto reply;
	}

	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
		/* The requirements reserve result 1 specifically for file missing. */
		rc = 1;
		goto reply;
	}
	if ((ota_info.status == FNODE_OTA_STATUS_FILE_START ||
	     ota_info.status == FNODE_OTA_STATUS_FILE_PACKAGE) &&
	    strcmp(path, ota_info.bin_path) == 0) {
		rc = FNODE_OTA_ERROR_FILE_RX_SIZE;
		goto reply;
	}
	if (st.st_size <= 0 || (uint64_t)st.st_size > OTA_IMAGE_MAX_SIZE) {
		rc = FNODE_OTA_ERROR_FIRMWARE_TOOLARGE;
		goto reply;
	}

	fusb_ota_close_download();
	memset(ota_info.download_bin_filename, 0,
	       sizeof(ota_info.download_bin_filename));
	memcpy(ota_info.download_bin_filename, data + 3, filename_len);
	snprintf(ota_info.bin_path, sizeof(ota_info.bin_path), "%s", path);
	ota_info.node_ota_bitmap = bitmap;

	if (fNIRS_ota_start() != 0) {
		rc = FNODE_OTA_ERROR_MAX;
		goto reply;
	}

	rc = FNODE_OTA_ERROR_NO;
	WLOGI("OTA_NODES_START ok path=%s bitmap=0x%04x\r\n",
	      ota_info.bin_path, bitmap);

reply:
	fusb_write(obj, FUSB_OTA_NODES_START, (uint8_t *)&rc, 1);
}

//串口命令体处理总入口函数
static void fusb_handler(fusb_obj_t *obj, uint16_t addr, uint8_t *data, uint16_t size)
{
#if defined(FNIRS_EMBEDDED) && defined(FNIRS_EV_IO)
	(void)fNIRS_lazy_init();
#endif

	printf("PC -- cmd:0x%X,size: %d\r\n", addr, size);
	
    switch(addr)
	{
    case FUSB_RESET:
        fusb_handler_reset(obj, data, size);
        break;
	
    case FUSB_SCAN: //1
        fusb_handler_scan(obj, data, size);
        break;
	
    case FUSB_S_GAIN: //2 设置采样LED电流
        fusb_handler_s_gain(obj, data, size);
        break;
	
    case FUSB_SAMPLE_ON: //3 采样开始
        fdatalog_start(); //开始记录数据
        fusb_handler_sample_on(obj, data, size);
        break;
	
    case FUSB_SAMPLE_OFF: //4 采样结束
        fusb_handler_sample_off(obj, data, size);
        fdatalog_stop(); //结束记录数据
        break;
	
    case FUSB_NTPDATE:
        fusb_handler_ntpdate(obj, data, size);
        break;
    
    case FUSB_SET_SAMPLE_ARRAY: //设置发光序列
        fusb_handler_set_sample_array(obj, data, size);
        break;

	//PC上位机与控制器工厂测试相关命令
    case FUSB_FACTORY_MODE: // = 0x1001, //工厂测试模式进入/离开
    	fusb_FUSB_FACTORY_MODE_handler(obj, data, size);
    	break;
	
    case FUSB_MEASURE: // = 0x1002, //单通道3厘米发射与接收
    	fusb_FUSB_MEASURE_handler(obj, data, size);
    	break;
	
    case FUSB_CALI_PD_OFFSET: // = 0x1003, //校准PD OFFSET
    	fusb_FUSB_CALI_PD_OFFSET_handler(obj, data, size);
    	break;

    case FUSB_MEASURE_ALTERNATE: // = 0x1004, //单通道3厘米735和850交替发射与接收，huang
    	fusb_FUSB_MEASURE_ALTERNATE_handler(obj, data, size);
    	break;
	
    case FUSB_CHK_PD_UNIFORMITY: // = 0x1006, //单光源周边1厘米距离PD接收一致性???
    	fusb_FUSB_CHK_PD_UNIFORMITY_handler(obj, data, size);
    	break;

	case FUSB_LED_ON: //0x1007, //以互斥方式单独点亮某个LED
		fusb_FUSB_LED_ON_handler(obj, data, size);
		break;

	case FUSB_LED_ON_MEASURE: //0x1008, //LED常亮，单通道3厘米发射与接收
		fusb_FUSB_LED_ON_MEASURE_handler(obj, data, size);
		break;
	
	//PC上位机与控制器信息查询相关命令
    case FUSB_HUB_SN: // = 0x1010, //查询控制器SN号
    	fusb_FUSB_HUB_SN_handler(obj, data, size);
    	break;
	
    case FUSB_NODE_SN: // = 0x1011, //查询节点SN号
    	fusb_FUSB_NODE_SN_handler(obj, data, size);
    	break;
    
	//PC上位机与控制器OTA相关命令
    case FUSB_OTA_FILE_START: // = 0x2001, //OTA BIN文件信息
    	fusb_FUSB_OTA_FILE_START_handler(obj, data, size);
    	break;
		
    case FUSB_OTA_FILE_CANCEL: // = 0x2002, //取消OTA升级
    	fusb_FUSB_OTA_FILE_CANCEL_handler(obj, data, size);
    	break;
		
    case FUSB_OTA_FILE_PACKAGE: // = 0x2003, //OTA BIN文件包传输
    	fusb_FUSB_OTA_FILE_PACKAGE_handler(obj, data, size);
    	break;
		
    case FUSB_OTA_FILE_FINISH: // = 0x2004, //OTA BIN文件完成传输
    	fusb_FUSB_OTA_FILE_FINISH_handler(obj, data, size);
    	break;
		
    case FUSB_OTA_NODES_START: // = 0x2005, //通知控制器升级节点
    	fusb_FUSB_OTA_NODES_START_handler(obj, data, size);
    	break;
		
    case FUSB_OTA_PROGRESS: // [NOT USED] 0x2006, //升级节点进度通知
    	break;
		
    case FUSB_OTA_RESULT: // [NOT USED] 0x2007, //整体升级情况汇总
    	break;
		
    default:
        WLOGW("Unkown addr: %#x\r\n", addr);
        break;
    }
}
