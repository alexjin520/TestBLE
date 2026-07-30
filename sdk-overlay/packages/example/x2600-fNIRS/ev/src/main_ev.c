#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "ev_app.h"
#include "ev_timer.h"
#include "my_interface.h"
#ifdef FNIRS_UV_BACKEND
#include "fnirs_uv_stage.h"
#endif

extern ev_module_t ev_signal_module;
extern ev_module_t ev_fnirs_module;
extern ev_module_t ev_can_module;
extern ev_module_t ev_uart_module;
extern ev_module_t ev_gatt_module;
extern ev_module_t ev_ble_async_module;
extern ev_module_t ev_timer_module;

static void custom_tzset(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();
}

static void ev_fatal_signal_handler(int sig, siginfo_t *info, void *ucontext)
{
    char note[80];

    (void)info;
    (void)ucontext;

    snprintf(note, sizeof(note), "fatal signal %d gettid=%ld pthread=%lu",
             sig, (long)syscall(SYS_gettid),
             (unsigned long)pthread_self());
    ev_timer_note_unsafe(note);
    signal(sig, SIG_DFL);
    raise(sig);
}

static void ev_on_exit(void)
{
    ev_timer_note("main exit");
}

static void ev_install_crash_hooks(void)
{
    struct sigaction sa;

    atexit(ev_on_exit);
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = ev_fatal_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
}

int32_t main(int32_t argc, const char *argv[])
{
    ev_app_t *app;
    ev_module_t modules[] = {
        ev_signal_module,
        ev_fnirs_module,
        ev_can_module,
        ev_uart_module,
        ev_ble_async_module,
        ev_gatt_module,
        ev_timer_module,
    };
    ev_module_t uv_safe_modules[1];
    ev_module_t uv_business_modules[] = {
        ev_signal_module,
        ev_fnirs_module,
    };
    ev_module_t uv_core_modules[] = {
        ev_signal_module,
        ev_fnirs_module,
        ev_can_module,
        ev_uart_module,
        ev_timer_module,
    };
    ev_module_t *selected_modules = modules;
    uint32_t selected_module_count =
        (uint32_t)(sizeof(modules) / sizeof(modules[0]));
#ifdef FNIRS_UV_BACKEND
    const char *uv_profile;
#endif
    int rc = 0;

    (void)argc;
    (void)argv;

#ifdef FNIRS_UV_BACKEND
    fnirs_uv_stage("main");
#endif
    custom_tzset();
#ifdef FNIRS_UV_BACKEND
    fnirs_uv_stage("tz ready");
#endif
    ev_install_crash_hooks();
#ifdef FNIRS_UV_BACKEND
    fnirs_uv_stage("hooks ready");
#endif

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
#ifdef FNIRS_UV_BACKEND
    fnirs_uv_stage("stdio ready");
#endif

#ifdef FNIRS_UV_BACKEND
    WLOGI("my-fnirs-uv unified (libuv + GATT) %s\r\n", SW_VERSION);
    uv_profile = getenv("FNIRS_UV_PROFILE");
    if (getenv("FNIRS_UV_SAFE_MODE") &&
        strcmp(getenv("FNIRS_UV_SAFE_MODE"), "0") != 0) {
        uv_profile = "safe";
    }
    if (!uv_profile || strcmp(uv_profile, "full") == 0) {
        uv_profile = "full";
        fnirs_uv_stage("profile full selected");
    } else if (strcmp(uv_profile, "safe") == 0) {
        uv_safe_modules[0] = ev_signal_module;
        selected_modules = uv_safe_modules;
        selected_module_count = 1;
        WLOGW("UV safe mode: hardware/GATT modules disabled\r\n");
        fnirs_uv_stage("profile safe selected");
    } else if (strcmp(uv_profile, "business") == 0) {
        selected_modules = uv_business_modules;
        selected_module_count =
            (uint32_t)(sizeof(uv_business_modules) /
                       sizeof(uv_business_modules[0]));
        fnirs_uv_stage("profile business selected");
    } else if (strcmp(uv_profile, "core") == 0) {
        selected_modules = uv_core_modules;
        selected_module_count =
            (uint32_t)(sizeof(uv_core_modules) /
                       sizeof(uv_core_modules[0]));
        fnirs_uv_stage("profile core selected");
    } else {
        WLOGE("invalid FNIRS_UV_PROFILE=%s\r\n", uv_profile);
        return 2;
    }
    WLOGI("UV profile=%s modules=%u\r\n",
          uv_profile, selected_module_count);
#else
    WLOGI("my-fnirs unified (libevent + GATT) %s\r\n", SW_VERSION);
#endif

#ifdef FNIRS_UV_BACKEND
    fnirs_uv_stage("app create enter");
#endif
    app = ev_app_create();
    if (!app) {
        WLOGE("ev_app_create failed\r\n");
        return 1;
    }
#ifdef FNIRS_UV_BACKEND
    fnirs_uv_stage("app create ready");
#endif

    if (ev_app_register_modules(app, selected_modules,
                                selected_module_count) != 0) {
        WLOGE("ev_app_register_modules failed\r\n");
        rc = 1;
        goto out;
    }
#ifdef FNIRS_UV_BACKEND
    fnirs_uv_stage("modules registered");
#endif

    int dispatch_rc;

    if (ev_app_init_modules(app) != 0) {
        WLOGE("ev_app_init_modules failed\r\n");
        rc = 1;
        goto out;
    }
#ifdef FNIRS_UV_BACKEND
    fnirs_uv_stage("modules ready");
    fnirs_uv_stage("dispatch enter");
#endif

    dispatch_rc = ev_app_run(app);
#ifdef FNIRS_UV_BACKEND
    fnirs_uv_stage("dispatch returned");
#endif
    if (dispatch_rc != 0)
        WLOGE("event loop exited abnormally, rc=%d\r\n", dispatch_rc);
    ev_app_shutdown_modules(app);

out:
    ev_app_destroy(app);
    return rc;
}
