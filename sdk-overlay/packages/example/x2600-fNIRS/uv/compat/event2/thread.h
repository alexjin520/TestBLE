#ifndef FNIRS_UV_EVENT_THREAD_COMPAT_H_
#define FNIRS_UV_EVENT_THREAD_COMPAT_H_

struct event_base;

static inline int evthread_use_pthreads(void)
{
    return 0;
}

static inline int evthread_make_base_notifiable(struct event_base *base)
{
    (void)base;
    return 0;
}

#endif
