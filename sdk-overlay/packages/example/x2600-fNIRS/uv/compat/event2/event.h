#ifndef FNIRS_UV_EVENT_COMPAT_H_
#define FNIRS_UV_EVENT_COMPAT_H_

#include <signal.h>
#include <stdint.h>
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int evutil_socket_t;
typedef void (*event_callback_fn)(evutil_socket_t, short, void *);

struct event_base;
struct event;

#define EV_TIMEOUT 0x01
#define EV_READ    0x02
#define EV_WRITE   0x04
#define EV_SIGNAL  0x08
#define EV_PERSIST 0x10

#define EVLOOP_NONBLOCK 0x02

struct event_base *event_base_new(void);
void event_base_free(struct event_base *base);
int event_base_dispatch(struct event_base *base);
int event_base_loop(struct event_base *base, int flags);
int event_base_loopbreak(struct event_base *base);

struct event *event_new(struct event_base *base, evutil_socket_t fd,
                        short events, event_callback_fn cb, void *arg);
struct event *evsignal_new(struct event_base *base, int sig,
                           event_callback_fn cb, void *arg);
struct event *evtimer_new(struct event_base *base, event_callback_fn cb,
                          void *arg);
int event_add(struct event *ev, const struct timeval *tv);
int evtimer_add(struct event *ev, const struct timeval *tv);
int event_del(struct event *ev);
void event_free(struct event *ev);

int event_base_once(struct event_base *base, evutil_socket_t fd, short events,
                    event_callback_fn cb, void *arg,
                    const struct timeval *tv);

#ifdef __cplusplus
}
#endif

#endif
