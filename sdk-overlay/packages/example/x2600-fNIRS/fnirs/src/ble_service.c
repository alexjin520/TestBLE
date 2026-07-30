#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "unistd.h"
#include "signal.h"
#include "sys/wait.h"
#include "sys/stat.h"
#include "errno.h"

#include "ble_service.h"
#include "my_interface.h"

#define AIC_RADIO_SM        "/usr/bin/aic-radio-sm"
#define MY_SERVER_BIN       "/opt/golgi/my-server"
#define BLE_RX_DIR          "/app_data/ble_rx"

static pid_t g_ble_server_pid = -1;
static int32_t g_ble_enabled = 1;

static int32_t ble_env_disabled(void)
{
    const char *v = getenv("FNIRS_BLE_DISABLE");

    return (v && (v[0] == '1' || v[0] == 'y' || v[0] == 'Y'));
}

static int32_t run_shell(const char *cmd)
{
    int rc = system(cmd);

    if (rc == -1) {
        WLOGE("system() failed: %s (%s)\r\n", cmd, strerror(errno));
        return -1;
    }

    if (WIFEXITED(rc) && WEXITSTATUS(rc) != 0) {
        WLOGW("command exit %d: %s\r\n", WEXITSTATUS(rc), cmd);
        return -1;
    }

    return 0;
}

static int32_t radio_switch_ble(void)
{
    char cmd[160];

    if (access(AIC_RADIO_SM, X_OK) != 0) {
        WLOGE("missing %s\r\n", AIC_RADIO_SM);
        return -1;
    }

    snprintf(cmd, sizeof(cmd), "%s ble", AIC_RADIO_SM);
    return run_shell(cmd);
}

static void reap_ble_server_child(void)
{
    int status;

    if (g_ble_server_pid <= 0)
        return;

    if (waitpid(g_ble_server_pid, &status, WNOHANG) > 0)
        g_ble_server_pid = -1;
}

static int32_t pidof_my_server(void)
{
    int rc = system("pidof my-server >/dev/null 2>&1");

    return (rc != -1 && WIFEXITED(rc) && WEXITSTATUS(rc) == 0);
}

static int32_t my_server_running(void)
{
    reap_ble_server_child();

    if (pidof_my_server())
        return 1;

    if (g_ble_server_pid > 0)
        g_ble_server_pid = -1;

    return 0;
}

static int32_t spawn_my_server(void)
{
    pid_t pid;

    if (access(MY_SERVER_BIN, X_OK) != 0) {
        WLOGE("missing %s\r\n", MY_SERVER_BIN);
        return -1;
    }

    if (my_server_running()) {
        WLOGI("my-server already running\r\n");
        return 0;
    }

    mkdir(BLE_RX_DIR, 0755);

    pid = fork();
    if (pid < 0) {
        WLOGE("fork my-server failed: %s\r\n", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        setsid();
        execl(MY_SERVER_BIN, "my-server", "-U", (char *)NULL);
        _exit(127);
    }

    g_ble_server_pid = pid;
    sleep(1);
    reap_ble_server_child();
    if (!pidof_my_server()) {
        WLOGW("my-server exited soon after start (pid was %d)\r\n", (int)pid);
        return -1;
    }

    WLOGI("my-server started pid=%d (-U, files -> %s)\r\n",
          (int)pid, BLE_RX_DIR);
    return 0;
}

int32_t ble_service_init(void)
{
#if defined(FNIRS_EMBEDDED)
    g_ble_enabled = 0;
    WLOGI("BLE service disabled (GATT embedded in my-fnirs-ev)\r\n");
    return 0;
#else
    g_ble_enabled = ble_env_disabled() ? 0 : 1;
    if (!g_ble_enabled)
        WLOGW("BLE auto-start disabled (FNIRS_BLE_DISABLE)\r\n");
    return 0;
#endif
}

int32_t ble_service_start(void)
{
    if (!g_ble_enabled)
        return 0;

    /* S91 may already have switched radio; still try to spawn my-server. */
    if (radio_switch_ble() != 0)
        WLOGW("aic-radio-sm ble failed; try spawn my-server anyway\r\n");

    return spawn_my_server();
}

void ble_service_stop(void)
{
    if (g_ble_server_pid > 0) {
        kill(g_ble_server_pid, SIGTERM);
        waitpid(g_ble_server_pid, NULL, 0);
        g_ble_server_pid = -1;
    }

    run_shell("killall my-server 2>/dev/null");
}

void ble_service_tick(void)
{
    if (!g_ble_enabled)
        return;

    reap_ble_server_child();

    if (!my_server_running())
        spawn_my_server();
}
