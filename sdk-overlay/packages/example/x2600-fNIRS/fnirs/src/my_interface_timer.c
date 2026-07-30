
#include <time.h>

#include "my_interface.h"

/* 获取单调毫秒时间戳 */
uint64_t get_monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
	
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}


void wos_timer_init(wos_timer_t *timer)
{
    timer->time = 0;   /* 标记为超时/未启动 */
}



void wos_timer_start(wos_timer_t *timer)
{
    timer->time = get_monotonic_ms();
}



void wos_timer_countdown_ms(wos_timer_t *timer, uint64_t millisecond)
{
    timer->time = get_monotonic_ms() + millisecond;
}



void wos_timer_expire(wos_timer_t *timer)
{
    timer->time = 1; /* 任意非 0 且小于当前时间即可 */
}

uint32_t wos_timer_is_expired(wos_timer_t *timer)
{
    if (timer->time == 0)
		return 1; /* 强制过期 */
	
    return (get_monotonic_ms() >= timer->time) ? 1 : 0;
}

uint64_t wos_timer_left(wos_timer_t *timer)
{
    if (timer->time == 0)
		return 0;
	
    uint64_t now = get_monotonic_ms();
	
    return (now >= timer->time) ? 0 : (timer->time - now);
}

