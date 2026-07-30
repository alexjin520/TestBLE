#include "ev_module.h"
#include "ev_app.h"

#include <signal.h>
#include <stdlib.h>

#include <event2/event.h>

#include "my_interface.h"

typedef struct {
    struct event *sigint_ev;
    struct event *sigterm_ev;
} ev_signal_ctx_t;

static void ev_signal_cb(evutil_socket_t sig, short events, void *arg)
{
    ev_app_t *app = arg;

    (void)events;

    WLOGW("signal %d, stopping event loop\r\n", (int)sig);
    ev_app_stop(app);
}

static int ev_signal_init(ev_app_t *app, ev_module_t *mod)
{
    ev_signal_ctx_t *ctx = calloc(1, sizeof(*ctx));

    if (!ctx)
        return -1;

    mod->ctx = ctx;

    ctx->sigint_ev = evsignal_new(app->base, SIGINT, ev_signal_cb, app);
    ctx->sigterm_ev = evsignal_new(app->base, SIGTERM, ev_signal_cb, app);
    if (!ctx->sigint_ev || !ctx->sigterm_ev)
        return -1;

    signal(SIGPIPE, SIG_IGN);

    if (event_add(ctx->sigint_ev, NULL) != 0)
        return -1;
    if (event_add(ctx->sigterm_ev, NULL) != 0)
        return -1;

    return 0;
}

static void ev_signal_shutdown(ev_app_t *app, ev_module_t *mod)
{
    ev_signal_ctx_t *ctx = mod->ctx;

    (void)app;

    if (!ctx)
        return;

    if (ctx->sigint_ev) {
        event_free(ctx->sigint_ev);
        ctx->sigint_ev = NULL;
    }
    if (ctx->sigterm_ev) {
        event_free(ctx->sigterm_ev);
        ctx->sigterm_ev = NULL;
    }

    free(ctx);
    mod->ctx = NULL;
}

ev_module_t ev_signal_module = EV_MODULE_REGISTER("signal", ev_signal_init, ev_signal_shutdown);
