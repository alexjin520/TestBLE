/* _gpio.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include "my_interface.h"

/* ========== 板级映射（根据原理图修改） ========== */
typedef struct {
    uint32_t  logic;     /* wos_logic_gpio_t */
    uint32_t  phy;       /* 物理 GPIO 编号 = bank*32 + pin */
    uint8_t   active_low;/* 1-低电平有效 */
} logic_map_t;

static const logic_map_t g_map[] = {
    /* input */
    {WOS_GPIO_FIRE,   WOS_GPIOE(7),  1}, /* PE7  低=火警 */
    {WOS_GPIO_PUSH,   WOS_GPIOE(8),  1}, /* PE8  低=按键 */
    {WOS_GPIO_DOOR,   WOS_GPIOE(9),  0}, /* PE9  高=门开 */
    {WOS_GPIO_DISA,   WOS_GPIOE(10),1}, /* PE10 低=防拆 */
    {WOS_GPIO_RESET,  WOS_GPIOE(11),1}, /* PE11 低=复位 */
    {WOS_GPIO_ALARM0, WOS_GPIOE(12),0}, /* PE12 高=告警0 */
    {WOS_GPIO_ALARM1, WOS_GPIOE(13),0}, /* PE13 高=告警1 */

    /* output */
    {WOS_GPIO_RELAY,  WOS_GPIOD(15), 0}, /* PD15 高=继电器闭合 */
    {WOS_GPIO_SWITCH, WOS_GPIOD(16), 0}, /* PD16 高=板载开关开 */
    {WOS_GPIO_WLED,   WOS_GPIOD(17), 0}, /* PD17 高=白光 LED 亮 */
    {WOS_GPIO_IRLED,  WOS_GPIOD(18), 0}, /* PD18 高=红外 LED 亮 */
    {WOS_GPIO_LCD,    WOS_GPIOD(19), 0}, /* PD19 高=LCD 亮屏 */
    {WOS_GPIO_WIFI,   WOS_GPIOD(20), 0}, /* PD20 高=WIFI 上电 */
    {WOS_GPIO_LTE,    WOS_GPIOD(21), 0}, /* PD21 高=4G 上电 */
    {WOS_GPIO_BLE,    WOS_GPIOD(22), 0}, /* PD22 高=BLE 上电 */
    {WOS_GPIO_MCU,    WOS_GPIOD(23), 0}, /* PD23 高=MCU 上电 */
};

#define SYSFS_GPIO_DIR "/sys/class/gpio"

static int gpio_export(uint32_t phy)
{
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "echo %u > " SYSFS_GPIO_DIR "/export", phy);
    return system(cmd); /* 已导出会返回 0，忽略错误 */
}

static int gpio_set_dir(uint32_t phy, int out)
{
    char path[128];
    snprintf(path, sizeof(path), SYSFS_GPIO_DIR "/gpio%d/direction", phy);
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    const char *dir = out ? "out" : "in";
    write(fd, dir, strlen(dir));
    close(fd);
    return 0;
}

static int gpio_set_value(uint32_t phy, int val)
{
    char path[128];
    snprintf(path, sizeof(path), SYSFS_GPIO_DIR "/gpio%d/value", phy);
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    char v = val ? '1' : '0';
    write(fd, &v, 1);
    close(fd);
    return 0;
}

static int gpio_get_value(uint32_t phy)
{
    char path[128];
    snprintf(path, sizeof(path), SYSFS_GPIO_DIR "/gpio%d/value", phy);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    char v;
    read(fd, &v, 1);
    close(fd);
    return (v == '1') ? WOS_GPIO_HIGH : WOS_GPIO_LOW;
}

static const logic_map_t *find_logic(uint32_t logic)
{
    for (size_t i = 0; i < sizeof(g_map)/sizeof(g_map[0]); i++)
        if (g_map[i].logic == logic) return &g_map[i];
    return NULL;
}

int32_t wos_logic_gpio_init(uint32_t gpio, uint32_t dir)
{
    const logic_map_t *m = find_logic(gpio);
    if (!m) return -EINVAL;
    gpio_export(m->phy);
    return gpio_set_dir(m->phy, dir == WOS_GPIO_OUTPUT);
}

int32_t wos_logic_gpio_deinit(uint32_t gpio)
{
    const logic_map_t *m = find_logic(gpio);
    if (!m) return -EINVAL;
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "echo %u > " SYSFS_GPIO_DIR "/unexport", m->phy);
    system(cmd);
    return 0;
}

int32_t wos_logic_gpio_get_input(uint32_t gpio)
{
    const logic_map_t *m = find_logic(gpio);
    if (!m) return -1;
    int v = gpio_get_value(m->phy);
    if (v < 0) return -1;
    /* 根据 active_low 翻转逻辑意义 */
    if (m->active_low) v = !v;
    return v;
}

int32_t wos_logic_gpio_set_output(uint32_t gpio, uint32_t state)
{
    const logic_map_t *m = find_logic(gpio);
    if (!m) return -EINVAL;
    int v = (state == WOS_GPIO_HIGH);
    if (m->active_low) v = !v;
    return gpio_set_value(m->phy, v) ? -1 : 0;
}

int32_t wos_phy_gpio_init(uint32_t gpio, uint32_t dir)
{
    gpio_export(gpio);
    return gpio_set_dir(gpio, dir == WOS_GPIO_OUTPUT);
}

int32_t wos_phy_gpio_deinit(uint32_t gpio)
{
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "echo %u > " SYSFS_GPIO_DIR "/unexport", gpio);
    system(cmd);
    return 0;
}

int32_t wos_phy_gpio_get_input(uint32_t gpio)
{
    return gpio_get_value(gpio);
}

int32_t wos_phy_gpio_set_output(uint32_t gpio, uint32_t state)
{
    return gpio_set_value(gpio, state == WOS_GPIO_HIGH) ? -1 : 0;
}


