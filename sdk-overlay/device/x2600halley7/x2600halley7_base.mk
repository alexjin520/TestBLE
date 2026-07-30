#Tools and libraries necessary by program

#kernel & uboot
#PRODUCT_MODULES := $(KERNEL_TARGET_IMAGE) \
	$(UBOOT_TARGET_FILE)
PRODUCT_MODULES := 	uboot \
			kernel

ifneq ($(strip $(TARGET_EXT2_SUPPORT)),burn)
RUNTESTDEMO_UTILS := 	RuntestScript \
			StorageMediaTestScript \
			AwakeTestScript

# OTA support
ifeq ($(strip $(TARGET_EXT2_SUPPORT)),ota)
PRODUCT_MODULES += kernel_recovery	\
		   recovery		\
		   libupdater		\
		   libsysutils

ifeq ($(strip $(TARGET_STORAGE_MEDIUM)),msc)
PRODUCT_MODULES += getpackage
endif # end msc

endif

PRODUCT_MODULES += webcam_gadget \
		   getevent_test \
		   prn_example \
		   hid_gadget_test \
		   grab \
		   install_usb_composite_srcipts



ifneq ($(strip $(TARGET_STORAGE_MEDIUM)),nor)
DPU_UTILS := dpu
HASH_UTILS := hash
endif

IMPP_TEST_EXAMPLE +=						\
		impp-camera-example			\
		csc-example				\
		osd-example				\
		display-pic-example			\
		display-usrptr-example			\
		display-comp-extend-example		\
		display-pic-extend-example		\
		display-comp-local-alpha-example	\
		dpu-osd-example				\
		ai_test					\
		ao_test					\
		ai_aenc_test				\
		ao_adec_test				\
		resampler_test				\
		h264dec-example				\
		h264dec-example-thread			\
		h264dec-example-fb-memcpy		\
		jpegdec-example				\
		h264dec-example				\
		h264dec-example-thread			\
		h264dec-example-fb-memcpy		\
		jpegdec-x2600-example			\
		file-jpegenc-x2600-example		\
		impp-camera-example
		#camera-rtsp				\
		#camera-mp4-recoder

IHW_TEST_EXAMPLE +=		\
		pwm_test	\
		sadc_test	\
		iio_sadc_test	\
		watchdog_test	\
		i2c_test	\
		uart_test	\
		tcu_test	\
		gpio_test	\
		spi_test	\
		spi_slv_test

#the device applications & test demo
ifneq ($(strip $(TARGET_STORAGE_MEDIUM)),nor)

PRODUCT_MODULES += ingenic-mpp
PRODUCT_MODULES += $(IMPP_TEST_EXAMPLE)

ifneq ($(strip $(TARGET_EXT2_SUPPORT)),ota)
PRODUCT_MODULES += install-mount-userdata
endif
PRODUCT_MODULES += ingenic-hw
PRODUCT_MODULES += $(IHW_TEST_EXAMPLE)

PRODUCT_MODULES += $(RUNTESTDEMO_UTILS) \
		   $(DPU_UTILS)

else
PRODUCT_MODULES += $(RUNTESTDEMO_UTILS)

endif
BLUETOOTH_FIRMWARE := install_wifi_bt_aic8800
PRODUCT_MODULES += $(BLUETOOTH_FIRMWARE)

WIFI_FIRMWARE := install_wifi_bt_aic8800
PRODUCT_MODULES += $(WIFI_FIRMWARE)

PRODUCT_MODULES += ble-file-rx
PRODUCT_MODULES += my-fnirs
PRODUCT_MODULES += my-fnirs-uv
endif
