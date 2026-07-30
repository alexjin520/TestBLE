//#include "wcan.h"
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <libsocketcan.h>

#include "my_interface.h"

#define NLMSG_TAIL(nmsg) \
	((struct rtattr *)(((void *)(nmsg)) + NLMSG_ALIGN((nmsg)->nlmsg_len)))

struct wcan_set_req {
	struct nlmsghdr n;
	struct ifinfomsg i;
	char buf[1024];
};

static int wcan_addattr_l(struct nlmsghdr *n, size_t maxlen, int type,
			  const void *data, int alen)
{
	int len = RTA_LENGTH(alen);
	struct rtattr *rta;

	if (NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(len) > maxlen)
		return -1;

	rta = NLMSG_TAIL(n);
	rta->rta_type = type;
	rta->rta_len = len;
	memcpy(RTA_DATA(rta), data, alen);
	n->nlmsg_len = NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(len);
	return 0;
}

static int wcan_addattr32(struct nlmsghdr *n, size_t maxlen, int type, __u32 data)
{
	return wcan_addattr_l(n, maxlen, type, &data, sizeof(data));
}

static int wcan_send_mod_request(int fd, struct nlmsghdr *n)
{
	struct sockaddr_nl nladdr = { .nl_family = AF_NETLINK };
	struct iovec iov = { .iov_base = n, .iov_len = n->nlmsg_len };
	struct msghdr msg = {
		.msg_name = &nladdr,
		.msg_namelen = sizeof(nladdr),
		.msg_iov = &iov,
		.msg_iovlen = 1,
	};
	char buf[16384];
	int status;

	n->nlmsg_seq = 0;
	n->nlmsg_flags |= NLM_F_ACK;
	if (sendmsg(fd, &msg, 0) < 0)
		return -1;

	iov.iov_base = buf;
	while (1) {
		status = recvmsg(fd, &msg, 0);
		if (status < 0)
			return -1;
		if (status == 0)
			return -1;

		for (struct nlmsghdr *h = (struct nlmsghdr *)buf;
		     NLMSG_OK(h, status); h = NLMSG_NEXT(h, status)) {
			if (h->nlmsg_type == NLMSG_ERROR) {
				struct nlmsgerr *err = NLMSG_DATA(h);
				return err->error ? -1 : 0;
			}
		}
	}
}

/* Kernel requires arb + data bittiming + FD ctrlmode in one netlink message. */
static int wcan_set_fd_link(const char *name, const struct can_bittiming *bt,
			    const struct can_bittiming *dbt,
			    const struct can_ctrlmode *cm, __u32 restart_ms)
{
	struct wcan_set_req req;
	struct rtattr *linkinfo, *data;
	const char *type = "can";
	int fd, ret;

	memset(&req, 0, sizeof(req));
	req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
	req.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	req.n.nlmsg_type = RTM_NEWLINK;
	req.i.ifi_family = AF_UNSPEC;
	req.i.ifi_index = if_nametoindex(name);
	if (!req.i.ifi_index)
		return -ENODEV;

	linkinfo = NLMSG_TAIL(&req.n);
	wcan_addattr_l(&req.n, sizeof(req), IFLA_LINKINFO, NULL, 0);
	wcan_addattr_l(&req.n, sizeof(req), IFLA_INFO_KIND, type, strlen(type));

	data = NLMSG_TAIL(&req.n);
	wcan_addattr_l(&req.n, sizeof(req), IFLA_INFO_DATA, NULL, 0);
	wcan_addattr_l(&req.n, sizeof(req), IFLA_CAN_BITTIMING, bt,
		       sizeof(*bt));
	wcan_addattr_l(&req.n, sizeof(req), IFLA_CAN_DATA_BITTIMING, dbt,
		       sizeof(*dbt));
	wcan_addattr_l(&req.n, sizeof(req), IFLA_CAN_CTRLMODE, cm,
		       sizeof(*cm));
	if (restart_ms)
		wcan_addattr32(&req.n, sizeof(req), IFLA_CAN_RESTART_MS,
			       restart_ms);
	data->rta_len = (void *)NLMSG_TAIL(&req.n) - (void *)data;
	linkinfo->rta_len = (void *)NLMSG_TAIL(&req.n) - (void *)linkinfo;

	fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (fd < 0)
		return -errno;

	ret = wcan_send_mod_request(fd, &req.n);
	close(fd);
	return ret;
}

//#define USING_SAVE_OPT

#define DEFAULT_BITRATE  1000000
/*
 * Deployed V7.0.1+ nodes use a 4 Mbps data phase.  The extension
 * specification describes 2 Mbps, so keep that selectable for matching
 * node firmware without breaking discovery of the installed nodes.
 */
#define DEFAULT_DBITRATE  4000000

static int wcan_data_bitrate(void)
{
	const char *value = getenv("FNIRS_CAN_DBITRATE");
	char *end = NULL;
	long bitrate;

	if (!value || value[0] == '\0')
		return DEFAULT_DBITRATE;

	errno = 0;
	bitrate = strtol(value, &end, 10);
	if (errno != 0 || !end || *end != '\0' ||
	    (bitrate != 2000000 && bitrate != 4000000)) {
		WLOGW("invalid FNIRS_CAN_DBITRATE=%s; using %d\r\n",
		      value, DEFAULT_DBITRATE);
		return DEFAULT_DBITRATE;
	}

	return (int)bitrate;
}

typedef struct {
    int            fd;          // socket fd
#ifdef USING_SAVE_OPT
    pthread_mutex_t lock;       // 线程安全
#endif
    int            fd_on;       // 是否已启用 FD
} wcan_priv_t;

/* 内部：接口 up + 设置比特率 */
static int wcan_up_OLD(const char *name)
{
    int ret;
    int dbitrate = wcan_data_bitrate();
    char cmd[128];
	
    /* 关闭 -> 设置 -> 启用 */
    ret = snprintf(cmd, sizeof(cmd),
                   "ip link set %s down && "
                   "ip link set %s type can bitrate %d dbitrate %d fd on && "
                   "ip link set %s up",
                   name, name, DEFAULT_BITRATE, dbitrate, name);
	
    if (ret >= (int)sizeof(cmd))
		return -ENOMEM;
	
    ret = system(cmd);
    return (ret == 0) ? 0 : -EIO;
}

/* Busybox ip 不支持 CAN-FD；libsocketcan 0.0.12 也无 data bittiming。 */
static int wcan_up(const char *name)
{
    int ret;
    int dbitrate = wcan_data_bitrate();
    struct can_bittiming bt = {
        .bitrate = DEFAULT_BITRATE,
        .sample_point = 800,
    };
    struct can_bittiming dbt = {
        .bitrate = dbitrate,
        .sample_point = 800,
    };
    struct can_ctrlmode cm = {
        .mask = CAN_CTRLMODE_FD,
        .flags = CAN_CTRLMODE_FD,
    };

    can_do_stop(name);

    ret = wcan_set_fd_link(name, &bt, &dbt, &cm, 100);
    if (ret < 0) {
        WLOGW("wcan_set_fd_link %s failed: %d\r\n", name, ret);
        return -EIO;
    }

    ret = can_do_start(name);
    if (ret < 0) {
        WLOGW("can_do_start %s failed: %d\r\n", name, ret);
        return -EIO;
    }

    WLOGI("wcan_up() -- %s bitrate %d dbitrate %d fd on\r\n",
          name, DEFAULT_BITRATE, dbitrate);
    return 0;
}





/* 
 * 内部：通用读，可接收经典/FD 帧 
 *
 * 返回值：成功返回0;
 */
static int wcan_read_common(int fd, void *frame, size_t sz, uint32_t to_ms)
{
    fd_set rfds;
    struct timeval tv = {.tv_sec  = to_ms / 1000,
                         .tv_usec = (to_ms % 1000) * 1000};
                         
	
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);

	// 始终传递&tv（而非根据tout_ms判断传NULL），实现超时控制
    int ret = select(fd + 1, &rfds, NULL, NULL, &tv);
	int save = errno;              // 立即保存
	if(ret <= 0)
    {
    	// rc=0 → 超时（包括tout_ms=0时无数据的情况）；rc<0 → 错误（如信号中断）
        return (ret == 0) ? -ETIMEDOUT : -save;
    }

    ssize_t n = read(fd, frame, sz);

	//WLOGI("wcan_read_common() -- n:0x%x\r\n", n);
	
    return (n == (ssize_t)sz) ? 0 : -errno;
}






/*-------------------- 对外 API 实现 --------------------*/

int32_t wcan_init(const char *canx, wcan_t *wcan)
{
    wcan_priv_t *priv;
    struct ifreq ifr = {0};
    struct sockaddr_can addr = {.can_family = AF_CAN};

    if (!canx || !wcan)
		return -EINVAL;

    if (wcan_up(canx) < 0)
		return -EIO;

    int fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0)
		return -errno;


    /*
    // >>> 新增：设置为非阻塞模式 调试CANFD发送停顿问题<<<
    int flags = fcntl(fd, F_GETFL, 0);
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        WLOGE("Failed to set CAN socket non-blocking\n");
        close(fd);
        return -errno;
    }*/




    strncpy(ifr.ifr_name, canx, IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0)
		goto err_sock;
	
    addr.can_ifindex = ifr.ifr_ifindex;



    /* 默认启用 FD 帧 */
    int fd_on = 1;
    if (setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &fd_on, sizeof(fd_on)) < 0) 
		goto err_sock;

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
		goto err_sock;



	/* 设置接收过滤器：只收 0x200-0x2FF 的 FD 帧 */
	struct can_filter rfilter[1] = 
	{
		{ .can_id = 0x200, .can_mask = 0xF00 }
	};
	if (setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FILTER, rfilter, sizeof(rfilter)) < 0)
		goto err_sock;

    priv = calloc(1, sizeof(wcan_priv_t));
    if (!priv)
		goto err_sock;
	
    priv->fd     = fd;
    priv->fd_on  = 1;
#ifdef USING_SAVE_OPT
    pthread_mutex_init(&priv->lock, NULL);
#endif
    *wcan = priv;
	
    return 0;

err_sock:
    close(fd);
	
    return -errno;
}





int32_t wcan_exit(wcan_t wcan)
{
    wcan_priv_t *priv = (wcan_priv_t *)wcan;
    if (!priv)
		return -EINVAL;
#ifdef USING_SAVE_OPT
    pthread_mutex_destroy(&priv->lock);
#endif
    close(priv->fd);
    free(priv);
	
    return 0;
}

int32_t wcan_get_fd(wcan_t wcan)
{
    wcan_priv_t *priv = (wcan_priv_t *)wcan;

    if (!priv)
        return -EINVAL;

    return priv->fd;
}

int32_t wcan_write(wcan_t wcan, struct can_frame *frame)
{
    wcan_priv_t *priv = (wcan_priv_t *)wcan;
    if (!priv || !frame)
		return -EINVAL;
#ifdef USING_SAVE_OPT
    pthread_mutex_lock(&priv->lock);
#endif
    ssize_t n = write(priv->fd, frame, sizeof(*frame));

	WLOGI("wcan_write() -- n:%d\r\n", n);

#ifdef USING_SAVE_OPT
    pthread_mutex_unlock(&priv->lock);
#endif
    return (n == sizeof(*frame)) ? 0 : -errno;
}




int32_t wcan_read(wcan_t wcan, struct can_frame *frame, uint32_t to_ms)
{
    wcan_priv_t *priv = (wcan_priv_t *)wcan;
    if (!priv || !frame)
		return -EINVAL;
#ifdef USING_SAVE_OPT	
    pthread_mutex_lock(&priv->lock);
#endif
    int rc = wcan_read_common(priv->fd, frame, sizeof(*frame), to_ms);

	WLOGI("wcan_read() -- rc:%d\r\n", rc);

#ifdef USING_SAVE_OPT
    pthread_mutex_unlock(&priv->lock);
#endif
	
    return rc;
}





int32_t wcanfd_write(wcan_t wcan, struct canfd_frame *frame)
{
    wcan_priv_t *priv = (wcan_priv_t *)wcan;
    if (!priv || !frame)
		return -EINVAL;

#ifdef USING_SAVE_OPT
    pthread_mutex_lock(&priv->lock);
#endif
    ssize_t n = write(priv->fd, frame, sizeof(*frame));
	int saved_errno = errno;

	//WLOGI("wcanfd_write() -- n:%d\r\n", n);

#ifdef USING_SAVE_OPT
    pthread_mutex_unlock(&priv->lock);
#endif
	
	if (n == (ssize_t)sizeof(*frame))
		return 0;
	return (n < 0) ? -saved_errno : -EIO;
}



int32_t wcanfd_read(wcan_t wcan, struct canfd_frame *frame, uint32_t to_ms)
{
    //led_can_nodes(1);

    wcan_priv_t *priv = (wcan_priv_t *)wcan;
    if (!priv || !frame)
		return -EINVAL;

#ifdef USING_SAVE_OPT
    pthread_mutex_lock(&priv->lock);
#endif
    int rc = wcan_read_common(priv->fd, frame, sizeof(*frame), to_ms);

    //led_can_nodes(0);

	//WLOGI("wcanfd_read() -- rc:%d\r\n", rc);

#ifdef USING_SAVE_OPT
    pthread_mutex_unlock(&priv->lock);
#endif

    return rc;
}



/**
 * @brief 带超时的CAN FD接收函数：在指定时间内等待接收一帧CAN FD数据
 * @param sockfd    已初始化的CAN RAW套接字描述符（需启用CAN FD模式并绑定接口）
 * @param frame     输出参数：存储接收到的CAN FD帧（调用者需提前分配内存）
 * @param timeout_ms 超时时间（毫秒，>0：超时等待；=0：非阻塞；<0：默认阻塞）
 * @return          0：接收成功；-1：接收失败（非超时错误）；-2：超时未收到数据
 * @note            1. 超时时间通过设置套接字SO_RCVTIMEO选项实现
 *                  2. 超时后需重新调用函数才能继续接收（超时设置是一次性的）
 */
int wcanfd_read_new(wcan_t wcan, struct canfd_frame *frame, uint32_t timeout_ms)
{
    wcan_priv_t *priv = (wcan_priv_t *)wcan;
    if (!priv || !frame)
		return -EINVAL;

	int sockfd = priv->fd;

    // 1. 参数合法性检查
    if (sockfd < 0) {
        WLOGE("sockfd = %d\n", sockfd);
        return -EINVAL;
    }

    // 2. 设置接收超时（仅当timeout_ms >=0时）
    struct timeval timeout;
    if (timeout_ms >= 0)
	{
        timeout.tv_sec = timeout_ms / 1000;          // 秒（1秒=1000毫秒）
        timeout.tv_usec = (timeout_ms % 1000) * 1000;  // 微秒（1毫秒=1000微秒）

        // 设置套接字接收超时选项（SO_RCVTIMEO）
        if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0)
		{
            WLOGE("setsockopt(SO_RCVTIMEO) FAIL");
            return -1;
        }
    }

    // 3. 接收CAN FD帧
    ssize_t recv_len = read(sockfd, frame, sizeof(struct canfd_frame));

    // 4. 处理接收结果
    if (recv_len < 0) 
	{
        // 区分超时错误和其他错误
        if (errno == EAGAIN || errno == EWOULDBLOCK)
		{
            // EAGAIN/EWOULDBLOCK：超时未收到数据（非阻塞或超时模式下）
            //WLOGE("RX timeout (%d ms)\n", timeout_ms);
            return -2;  // 超时状态码
        }
		else
		{
            // 其他错误（如接口断开、被信号中断等）
            WLOGE("RX Fail, other");
            return -1;
        }
    }
	else if (recv_len != sizeof(struct canfd_frame))
	{
        // 数据长度异常（未接收到完整帧）
        WLOGE("RX Fail, %d %d\n", recv_len, sizeof(struct canfd_frame));
        errno = EIO;
        return -1;
    }

    // 5. 接收成功
    return 0;
}
