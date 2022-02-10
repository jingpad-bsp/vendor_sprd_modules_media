LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
        SPRDAVCEncoder.cpp

LOCAL_C_INCLUDES := \
    frameworks/av/media/libstagefright/include                    \
    frameworks/native/include/media/hardware                      \
    frameworks/native/include                                     \
    frameworks/av/include                                     \
    $(LOCAL_PATH)/../../../../libstagefrighthw/include            \
    $(LOCAL_PATH)/../../../../libstagefrighthw/include/openmax    \
    $(LOCAL_PATH)/../../../../../libmemion                        \
    $(LOCAL_PATH)/../../../../../../external/kernel-headers

LOCAL_C_INCLUDES += $(TOP)/vendor/sprd/external/drivers/gpu
LOCAL_C_INCLUDES += $(TOP)/system/core/libion/kernel-headers

LOCAL_CFLAGS := -DOSCL_EXPORT_REF= -DOSCL_IMPORT_REF=

LOCAL_CFLAGS += -DTARGET_GPU_PLATFORM=$(TARGET_GPU_PLATFORM)

LOCAL_ARM_MODE := arm

LOCAL_MULTILIB := 32

LOCAL_SHARED_LIBRARIES := \
    libstagefright_omx         \
    libstagefright_omx_utils   \
    libstagefright_foundation  \
    libstagefrighthw           \
    libutils                   \
    libui                      \
    libmemion                  \
    libdl                      \
    liblog                     \
    libcutils

LOCAL_MODULE := libstagefright_sprd_h264enc
LOCAL_MODULE_TAGS := optional
LOCAL_PROPRIETARY_MODULE := true

ifeq ($(strip $(TARGET_BOARD_CAMERA_ANTI_SHAKE)),true)
LOCAL_CFLAGS += -DANTI_SHAKE
endif

ifneq ($(filter $(TARGET_BOARD_PLATFORM), sp9863a), )
LOCAL_CFLAGS += -DPLATFORM_SHARKL3
endif

ifneq ($(filter $(TARGET_BOARD_PLATFORM), ums312), )
LOCAL_CFLAGS += -DPLATFORM_SHARKL5
endif

ifneq ($(filter $(TARGET_BOARD_PLATFORM), ud710), )
LOCAL_CFLAGS += -DPLATFORM_ROC1
endif

ifeq ($(strip $(TARGET_BOARD_CAMERA_EIS)),true)
    LOCAL_CFLAGS += -DCONFIG_SPRD_RECORD_EIS
endif

ifeq ($(strip $(TARGET_BIA_SUPPORT)),true)
    LOCAL_CFLAGS += -DCONFIG_BIA_SUPPORT
endif

ifeq ($(strip $(SUPPORT_RGB_ENC)),true)
    LOCAL_CFLAGS += -DCONFIG_RGB_ENC_SUPPORT
endif
include $(BUILD_SHARED_LIBRARY)
