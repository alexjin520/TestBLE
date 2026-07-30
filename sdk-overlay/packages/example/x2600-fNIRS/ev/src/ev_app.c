#include "ev_app.h"

#include <stdlib.h>
#include <string.h>

#include <event2/event.h>
#include <event2/thread.h>

#include "ev_timer.h"
#include "my_interface.h"
#ifdef FNIRS_UV_BACKEND
#include "fnirs_uv_stage.h"
#endif

ev_app_t *ev_app_create(void)
{
    ev_app_t *app;

#ifdef FNIRS_UV_BACKEND
    fnirs_uv_stage("ev_app calloc enter");
#endif
    app = calloc(1, sizeof(*app));
    if (!app)
        return NULL;

#ifdef FNIRS_UV_BACKEND
    fnirs_uv_stage("ev_app calloc ready");
    fnirs_uv_stage("evthread enter");
#endif
    if (evthread_use_pthreads() != 0)
        WLOGW("evthread_use_pthreads failed (continuing)\r\n");
#ifdef FNIRS_UV_BACKEND
    fnirs_uv_stage("evthread ready");
    fnirs_uv_stage("event base enter");
#endif

    app->base = event_base_new();
    if (!app->base) {
        free(app);
        return NULL;
    }
#ifdef FNIRS_UV_BACKEND
    fnirs_uv_stage("event base ready");
#endif

    if (evthread_make_base_notifiable(app->base) != 0)
        WLOGW("evthread_make_base_notifiable failed\r\n");

    app->running = 1;
    return app;
}

void ev_app_destroy(ev_app_t *app)
{
    if (!app)
        return;

    if (app->base) {
        event_base_free(app->base);
        app->base = NULL;
    }

    free(app);
}

struct event_base *ev_app_base(ev_app_t *app)
{
    return app ? app->base : NULL;
}

void ev_app_loop_nonblock(struct event_base *base)
{
    if (!base)
        return;

    event_base_loop(base, EVLOOP_NONBLOCK);
}

int ev_app_register_modules(ev_app_t *app, ev_module_t *modules, uint32_t count)
{
    if (!app || !modules || count == 0)
        return -1;

    app->modules = modules;
    app->module_count = count;
    return 0;
}

int ev_app_init_modules(ev_app_t *app)
{
    uint32_t i;

    if (!app || !app->modules)
        return -1;

    for (i = 0; i < app->module_count; i++) {
        ev_module_t *mod = &app->modules[i];
#ifdef FNIRS_UV_BACKEND
        char stage[96];
#endif

        if (!mod->init)
            continue;

#ifdef FNIRS_UV_BACKEND
        snprintf(stage, sizeof(stage), "module %s init enter",
                 mod->name ? mod->name : "(null)");
        fnirs_uv_stage(stage);
#endif
        if (mod->init(app, mod) != 0) {
#ifdef FNIRS_UV_BACKEND
            snprintf(stage, sizeof(stage), "module %s init failed",
                     mod->name ? mod->name : "(null)");
            fnirs_uv_stage(stage);
#endif
            WLOGE("module init failed: %s\r\n", mod->name ? mod->name : "(null)");
            app->module_count = i;
            ev_app_shutdown_modules(app);
            return -1;
        }

#ifdef FNIRS_UV_BACKEND
        snprintf(stage, sizeof(stage), "module %s init ready",
                 mod->name ? mod->name : "(null)");
        fnirs_uv_stage(stage);
#endif
        WLOGI("module ready: %s\r\n", mod->name ? mod->name : "(null)");
    }

    return 0;
}

void ev_app_shutdown_modules(ev_app_t *app)
{
    int32_t i;

    if (!app || !app->modules)
        return;

    for (i = (int32_t)app->module_count - 1; i >= 0; i--) {
        ev_module_t *mod = &app->modules[i];

        if (mod->shutdown) {
#ifdef FNIRS_UV_BACKEND
            char stage[96];

            snprintf(stage, sizeof(stage), "module %s shutdown enter",
                     mod->name ? mod->name : "(null)");
            fnirs_uv_stage(stage);
#endif
            mod->shutdown(app, mod);
#ifdef FNIRS_UV_BACKEND
            snprintf(stage, sizeof(stage), "module %s shutdown ready",
                     mod->name ? mod->name : "(null)");
            fnirs_uv_stage(stage);
#endif
        }
    }
}

int ev_app_run(ev_app_t *app)
{
    int rc;

    if (!app || !app->base)
        return -1;

    WLOGI("event loop start\r\n");
    ev_timer_note("event loop start");
    ev_timer_arm();
    rc = event_base_dispatch(app->base);
    {
        char note[64];

        snprintf(note, sizeof(note), "event loop exit rc=%d", rc);
        ev_timer_note(note);
    }
    WLOGI("event loop exit, rc=%d\r\n", rc);

    return rc;
}

void ev_app_stop(ev_app_t *app)
{
    if (!app || !app->base)
        return;

    app->running = 0;
    event_base_loopbreak(app->base);
}
