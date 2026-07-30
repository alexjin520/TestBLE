LOCAL_PATH := $(my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE := my-fnirs
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_PATH := $(TARGET_FS_BUILD)/opt/golgi/

BR_STAGING := $(TOP_DIR)/$(TARGET_SYSROOT)
BR_INTERMEDIATE := $(TOP_DIR)/$(OUT_DEVICE_OBJ_DIR)/buildroot-intermediate
BLE_HCI_DIR := ../ble_hci_test
BLUEZ_BUILD_DIR := $(TOP_DIR)/$(OUT_DEVICE_OBJ_DIR)/buildroot-intermediate/build
BLUEZ_SRC_ROOT := $(firstword $(wildcard $(BLUEZ_BUILD_DIR)/bluez5_utils-*))

LOCAL_SRC_FILES := ev/src/main_ev.c \
	ev/src/ev_app.c \
	ev/src/ev_signal.c \
	ev/src/ev_uart.c \
	ev/src/ev_gatt.c \
	ev/src/ev_timer.c \
	ev/src/ev_fnirs.c \
	ev/src/ev_can.c \
	ev/src/ev_ble_async.c \
	ev/src/embedded_gatt_server.c \
	fnirs/src/fnirs.c \
	fnirs/src/fhub.c \
	fnirs/src/fnode.c \
	fnirs/src/fusb.c \
	fnirs/src/fdatalog.c \
	fnirs/src/led.c \
	fnirs/src/msgq.c \
	fnirs/src/my_interface_canfd.c \
	fnirs/src/my_interface_uart.c \
	fnirs/src/my_interface_timer.c \
	fnirs/src/my_interface_gpio.c \
	fnirs/src/my_interface_rtc.c \
	fnirs/src/my_utils_crc.c \
	fnirs/src/my_utils_ota.c \
	fnirs/src/ble_service.c \
	fnirs/src/fnirs_stream.c \
	fnirs/src/fnirs_ble_ipc.c

LOCAL_C_INCLUDES := ev/inc \
	fnirs/inc \
	include \
	$(BLE_HCI_DIR)

ifneq ($(BLUEZ_SRC_ROOT),)
LOCAL_CFLAGS += -I$(BR_STAGING)/usr/include \
	-I$(BLUEZ_SRC_ROOT) -I$(BLUEZ_SRC_ROOT)/lib -I$(BLUEZ_SRC_ROOT)/src/shared
else
LOCAL_CFLAGS += -I$(BR_STAGING)/usr/include
endif

LOCAL_CFLAGS += -std=gnu99 -Wall -O2 -DFNIRS_EV_IO=1 -DFNIRS_EMBEDDED=1 -DMY_SERVER_EMBEDDED=1

LOCAL_LDFLAGS := -L$(BR_STAGING)/usr/lib

LOCAL_LDLIBS := -levent -levent_pthreads -lsocketcan -lpthread -lm
ifneq ($(BLUEZ_SRC_ROOT),)
LOCAL_LDLIBS += -L$(BLUEZ_SRC_ROOT)/src/.libs -L$(BLUEZ_SRC_ROOT)/lib/.libs \
	-lbluetooth-internal -lshared-mainloop
endif
LOCAL_LDLIBS += -lcrypto -lrt -ldl -lc

include $(BUILD_EXECUTABLE)

MY_FNIRS_BR_DEPS_STAMP := $(TOP_DIR)/$(OUT_DEVICE_OBJ_DIR)/my-fnirs-brdeps.stamp
$(MY_FNIRS_BR_DEPS_STAMP): $(BR_INTERMEDIATE)/.config
	@if ! grep -q '^BR2_PACKAGE_LIBEVENT=y' $(BR_INTERMEDIATE)/.config; then \
		sed -i 's/# BR2_PACKAGE_LIBEVENT is not set/BR2_PACKAGE_LIBEVENT=y/' \
			$(BR_INTERMEDIATE)/.config; \
	fi
	$(MAKE) buildroot-libevent
	touch $@

$(__local_stamp_config): | $(MY_FNIRS_BR_DEPS_STAMP)

include $(CLEAR_VARS)

LOCAL_MODULE := my-fnirs-uv
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_PATH := $(TARGET_FS_BUILD)/opt/golgi/

BR_STAGING := $(TOP_DIR)/$(TARGET_SYSROOT)
BR_INTERMEDIATE := $(TOP_DIR)/$(OUT_DEVICE_OBJ_DIR)/buildroot-intermediate
BLE_HCI_DIR := ../ble_hci_test
BLUEZ_BUILD_DIR := $(TOP_DIR)/$(OUT_DEVICE_OBJ_DIR)/buildroot-intermediate/build
BLUEZ_SRC_ROOT := $(firstword $(wildcard $(BLUEZ_BUILD_DIR)/bluez5_utils-*))

# Keep the business and protocol sources identical to my-fnirs.  The local
# event2 compatibility layer maps the small API subset used by the application
# onto a uv_loop_t, so this target measures the event-loop backend rather than
# a forked sampling implementation.
LOCAL_SRC_FILES := uv/src/uv_event_compat.c \
	uv/src/uv_stage.c \
	ev/src/main_ev.c \
	ev/src/ev_app.c \
	ev/src/ev_signal.c \
	ev/src/ev_uart.c \
	ev/src/ev_gatt.c \
	ev/src/ev_timer.c \
	ev/src/ev_fnirs.c \
	ev/src/ev_can.c \
	ev/src/ev_ble_async.c \
	ev/src/embedded_gatt_server.c \
	fnirs/src/fnirs.c \
	fnirs/src/fhub.c \
	fnirs/src/fnode.c \
	fnirs/src/fusb.c \
	fnirs/src/fdatalog.c \
	fnirs/src/led.c \
	fnirs/src/msgq.c \
	fnirs/src/my_interface_canfd.c \
	fnirs/src/my_interface_uart.c \
	fnirs/src/my_interface_timer.c \
	fnirs/src/my_interface_gpio.c \
	fnirs/src/my_interface_rtc.c \
	fnirs/src/my_utils_crc.c \
	fnirs/src/my_utils_ota.c \
	fnirs/src/ble_service.c \
	fnirs/src/fnirs_stream.c \
	fnirs/src/fnirs_ble_ipc.c

LOCAL_C_INCLUDES := uv/compat \
	ev/inc \
	fnirs/inc \
	include \
	$(BLE_HCI_DIR)

ifneq ($(BLUEZ_SRC_ROOT),)
LOCAL_CFLAGS += -I$(BR_STAGING)/usr/include \
	-I$(BLUEZ_SRC_ROOT) -I$(BLUEZ_SRC_ROOT)/lib -I$(BLUEZ_SRC_ROOT)/src/shared
else
LOCAL_CFLAGS += -I$(BR_STAGING)/usr/include
endif

LOCAL_CFLAGS += -std=gnu99 -Wall -O2 -fPIE -DFNIRS_EV_IO=1 -DFNIRS_EMBEDDED=1 \
	-DMY_SERVER_EMBEDDED=1 -DFNIRS_UV_BACKEND=1

LOCAL_PIE := true
LOCAL_LDFLAGS := -pie -L$(BR_STAGING)/usr/lib

LOCAL_LDLIBS := -luv -lsocketcan -lpthread -lm
ifneq ($(BLUEZ_SRC_ROOT),)
LOCAL_LDLIBS += -L$(BLUEZ_SRC_ROOT)/src/.libs -L$(BLUEZ_SRC_ROOT)/lib/.libs \
	-lbluetooth-internal -lshared-mainloop
endif
LOCAL_LDLIBS += -lcrypto -lrt -ldl -lc

include $(BUILD_EXECUTABLE)

MY_FNIRS_UV_BR_DEPS_STAMP := $(TOP_DIR)/$(OUT_DEVICE_OBJ_DIR)/my-fnirs-uv-brdeps.stamp
$(MY_FNIRS_UV_BR_DEPS_STAMP): $(BR_INTERMEDIATE)/.config
	@if ! grep -q '^BR2_PACKAGE_LIBUV=y' $(BR_INTERMEDIATE)/.config; then \
		sed -i 's/# BR2_PACKAGE_LIBUV is not set/BR2_PACKAGE_LIBUV=y/' \
			$(BR_INTERMEDIATE)/.config; \
	fi
	$(MAKE) buildroot-libuv TAR_OPTIONS=--no-same-owner
	touch $@

$(__local_stamp_config): | $(MY_FNIRS_UV_BR_DEPS_STAMP)
