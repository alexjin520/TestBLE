
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>  // 必须包含：提供 PRIu64 宏

#include "my_interface.h"

/* 内部：运行 shell 并判成功 */
static int shell_ok(const char *cmd)
{
    return system(cmd) == 0 ? 0 : -EIO;
}

/* 1. 系统时间 → RTC */
int32_t wos_hwclock_write(void)
{
    return shell_ok("hwclock -w -f /dev/rtc0");
}

/* 2. RTC → 系统时间 */
int32_t wos_hwclock_read(void)
{
    return shell_ok("hwclock -s -f /dev/rtc0");
}



/* 3. 设置系统 UTC（秒）+ 同步 RTC —— 支持 64 位时间戳 */
int32_t wos_rtc_set(uint64_t utc)
{
    char cmd[64];
    // 使用 PRIu64 确保跨平台兼容性（Linux、嵌入式等）
    int len = snprintf(cmd, sizeof(cmd), "date -u @%" PRIu64, utc);
    
    // 防御性检查：防止缓冲区溢出（虽然 64 位最大 20 位数字，64 字节足够）
    if (len < 0 || (size_t)len >= sizeof(cmd)) {
        return -2; // 命令构造失败
    }

    if (shell_ok(cmd) != 0) {
        return -1; // date 命令执行失败
    }

    /* 立即写回 RTC */
    return wos_hwclock_write();
}




/* 4. gettimeofday 毫秒时间戳 */
uint64_t wos_system_timestamp_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)(tv.tv_usec / 1000);
}

/* 5. CLOCK_MONOTONIC 毫秒 */
uint64_t wos_system_clock_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000);
}

/* 5.1 CLOCK_MONOTONIC 微秒 */
uint64_t wos_system_clock_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
}

/* 6. CLOCK_MONOTONIC 秒 */
uint64_t wos_system_clock(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec;
}
