include $(DEVICE_PATH)/uboot.mk

include $(DEVICE_PATH)/kernel.mk

ifeq ($(strip $(TARGET_EXT2_SUPPORT)),ota)
include $(DEVICE_PATH)/kernel-recovery.mk
endif

include $(DEVICE_PATH)/testsuites-overlay/testsuite.mk
