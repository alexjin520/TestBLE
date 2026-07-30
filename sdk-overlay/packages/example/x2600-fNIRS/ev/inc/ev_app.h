#ifndef EV_APP_H_
#define EV_APP_H_

#include <stdint.h>

#include <event2/event.h>

#include "ev_module.h"

typedef struct ev_app {
    struct event_base *base;
    volatile int running;
    ev_module_t *modules;
    uint32_t module_count;
} ev_app_t;

ev_app_t *ev_app_create(void);
void ev_app_destroy(ev_app_t *app);

struct event_base *ev_app_base(ev_app_t *app);

/* Kick the main event loop from other threads (requires notifiable base). */
void ev_app_loop_nonblock(struct event_base *base);

int ev_app_register_modules(ev_app_t *app, ev_module_t *modules, uint32_t count);

int ev_app_init_modules(ev_app_t *app);
void ev_app_shutdown_modules(ev_app_t *app);

int ev_app_run(ev_app_t *app);
void ev_app_stop(ev_app_t *app);

#endif
