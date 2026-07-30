LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE := my-server
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_PATH := $(TARGET_FS_BUILD)/opt/golgi/

# 1. 定义 BlueZ 源码路径（跟随当前 SDK 输出目录，避免机器相关绝对路径）
BLUEZ_BUILD_DIR := $(TOP_DIR)/$(OUT_DEVICE_OBJ_DIR)/buildroot-intermediate/build
BLUEZ_SRC_ROOT := $(firstword $(wildcard $(BLUEZ_BUILD_DIR)/bluez5_utils-*))

# 2. 你的主程序源文件（只需要包含你自己的 .c 文件！）
LOCAL_SRC_FILES := mybtgatt-server.c

# 3. 添加 BlueZ 的头文件路径
# 这样你在 mybtgatt-server.c 里就可以直接 #include "lib/bluetooth.h" 或 #include "src/shared/mainloop.h"
LOCAL_CFLAGS += -I$(BLUEZ_SRC_ROOT)
LOCAL_CFLAGS += -I$(BLUEZ_SRC_ROOT)/lib
LOCAL_CFLAGS += -I$(BLUEZ_SRC_ROOT)/src/shared
LOCAL_CFLAGS += -I$(SYSROOT_HOST)/mips-linux-gnu/sysroot/usr/include

# 4. 链接 BlueZ 的预编译库（关键！）
# 既然我们不编译 BlueZ 的 .c 文件了，就需要链接 BlueZ 编译好的库。
# 如果你系统里已经编译过 BlueZ，通常会生成 libbluetooth-internal.a 或 libshared-mainloop.a
# 假设 BlueZ 编译后的库在它的 .libs 目录下：
LOCAL_LDLIBS += -L$(BLUEZ_SRC_ROOT)/src/.libs
LOCAL_LDLIBS += -L$(BLUEZ_SRC_ROOT)/lib/.libs
# 链接 BlueZ 的核心内部库（名字根据实际生成的 .a 文件调整）
LOCAL_LDLIBS += -lbluetooth-internal -lshared-mainloop
# 链接系统基础库
LOCAL_LDLIBS += -lrt -ldl -lpthread -lm -lc -lcrypto

include $(BUILD_EXECUTABLE)