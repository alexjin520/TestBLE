/*
 *  wos_uart.c
 *  Linux POSIX 实现，适配君正 X2600 串口列表
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/ioctl.h>

#include "my_interface.h"

/* 波特率转换表 */
static speed_t baud2speed(uint32_t baud)
{
    switch (baud)
    {
    case 9600:
        return B9600;
    case 19200:
        return B19200;
    case 38400:
        return B38400;
    case 57600:
        return B57600;
    case 115200:
        return B115200;
    case 230400:
        return B230400;
    case 460800:
        return B460800;
    case 500000:
        return B500000;
    case 576000:
        return B576000;
    case 921600:
        return B921600;
    case 1000000:
        return B1000000;
    case 1152000:
        return B1152000;
    case 1500000:
        return B1500000;
	case 2000000:  // 新增2MBPS分支
        return B2000000;
    default:
        return B0;
    }
}

/* 打开并配置串口 */
static int uart_open_raw(const char *dev, uint32_t baud,
                         uint8_t bits, uint8_t parity, uint8_t stop)
{
    int fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
        return -1;

    struct termios tio;
    if (tcgetattr(fd, &tio) != 0)
        goto err;

    /* 波特率 */
    speed_t spd = baud2speed(baud);
    if (spd == B0)
        goto err;
    cfsetispeed(&tio, spd);
    cfsetospeed(&tio, spd);

    /* 数据位 */
    tio.c_cflag &= ~CSIZE;
    switch (bits)
    {
    case 5:
        tio.c_cflag |= CS5;
        break;
    case 6:
        tio.c_cflag |= CS6;
        break;
    case 7:
        tio.c_cflag |= CS7;
        break;
    case 8:
        tio.c_cflag |= CS8;
        break;
    default:
        goto err;
    }

    /* 校验位 */
    tio.c_cflag &= ~(PARENB | PARODD);
    if (parity == 'E')
        tio.c_cflag |= PARENB;
    else if (parity == 'O')
        tio.c_cflag |= PARENB | PARODD;

    /* 停止位 */
    if (stop == 2)
        tio.c_cflag |= CSTOPB;
    else
        tio.c_cflag &= ~CSTOPB;

    /* 无流控，原始模式 */
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tio.c_iflag &= ~(IXON | IXOFF | IXANY | ICRNL);
    tio.c_oflag &= ~OPOST;

    tcflush(fd, TCIFLUSH);
    if (tcsetattr(fd, TCSANOW, &tio) != 0)
        goto err;

    /* 清空 NONBLOCK，后续用 select 做超时 */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    return fd;

err:
    close(fd);
    return -1;
}

/* 设备名映射表（X2600） */
static const char *uart_map[] = {
    "/dev/ttyS0", "/dev/ttyS1", "/dev/ttyS2", "/dev/ttyS3",
    "/dev/ttyS4", "/dev/ttyS5", "/dev/ttyS6", "/dev/ttyS7"};

/*-------------------- 对外实现 --------------------*/

int32_t wos_uart_fopen(const char *dev, uint32_t baud,
                       uint8_t bits, uint8_t parity, uint8_t stop)
{
    return uart_open_raw(dev, baud, bits, parity, stop);
}

int32_t wos_uart_open(uint8_t uartx, uint32_t baud,
                      uint8_t bits, uint8_t parity, uint8_t stop)
{
    if (uartx >= sizeof(uart_map) / sizeof(uart_map[0]))
        return -EINVAL;
    return uart_open_raw(uart_map[uartx], baud, bits, parity, stop);
}

int32_t wos_uart_close(int32_t fd)
{
    return (close(fd) == 0) ? 0 : -errno;
}

int32_t wos_uart_write(int32_t fd, const uint8_t *buf, uint32_t len)
{
    ssize_t nw = write(fd, buf, len);
    return (nw == (ssize_t)len) ? (int32_t)len : -errno;
}

int32_t wos_uart_read_OLD(int32_t fd, uint8_t *buf, uint32_t len, uint32_t tout_ms)
{
    fd_set rfds;
    struct timeval tv = {.tv_sec = tout_ms / 1000,
                         .tv_usec = (tout_ms % 1000) * 1000};
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    int rc = select(fd + 1, &rfds, NULL, NULL, tout_ms ? &tv : NULL);
    if (rc <= 0)
        return (rc == 0) ? -ETIMEDOUT : -errno;

    ssize_t nr = read(fd, buf, len);
    return (nr >= 0) ? (int32_t)nr : -errno;
}

int32_t wos_uart_read(int32_t fd, uint8_t *buf, uint32_t len, uint32_t tout_ms)
{
    fd_set rfds;
    // 1. 无论tout_ms是否为0，都初始化timeval（tout_ms=0时，tv_sec和tv_usec自动为0）
    struct timeval tv = {
        .tv_sec = tout_ms / 1000,          // 秒数（如tout_ms=1500 → 1秒）
        .tv_usec = (tout_ms % 1000) * 1000 // 微秒数（如tout_ms=1500 → 500000微秒）
    };

    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);

    // 2. 始终传递&tv（而非根据tout_ms判断传NULL），实现超时控制
    int rc = select(fd + 1, &rfds, NULL, NULL, &tv);
    if (rc <= 0) {
        // rc=0 → 超时（包括tout_ms=0时无数据的情况）；rc<0 → 错误（如信号中断）
        return (rc == 0) ? -ETIMEDOUT : -errno;
    }

    // 3. 有数据可读，执行读取（原逻辑不变）
    ssize_t nr = read(fd, buf, len);
    return (nr >= 0) ? (int32_t)nr : -errno;
}


