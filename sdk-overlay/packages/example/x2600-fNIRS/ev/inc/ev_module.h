#ifndef EV_MODULE_H_
#define EV_MODULE_H_

#include <stdint.h>

struct ev_app;

typedef struct ev_module {
    const char *name;
    int (*init)(struct ev_app *app, struct ev_module *mod);
    void (*shutdown)(struct ev_app *app, struct ev_module *mod);
    void *ctx;
} ev_module_t;

#define EV_MODULE_REGISTER(name_, init_, shutdown_) \
    { .name = (name_), .init = (init_), .shutdown = (shutdown_), .ctx = NULL }

#endif
