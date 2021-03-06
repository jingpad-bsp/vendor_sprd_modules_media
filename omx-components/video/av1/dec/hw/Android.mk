LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
    SPRDAV1Decoder.cpp \

LOCAL_C_INCLUDES := \
    frameworks/av/media/libstagefright/include                    \
    frameworks/native/include/media/hardware                      \
    frameworks/native/include/ui                                  \
    frameworks/native/include/utils                               \
    frameworks/native/include/media/hardware                      \
    frameworks/av/include/media/stagefright/foundation/           \
    $(LOCAL_PATH)/../../../../../libstagefrighthw/include         \
    $(LOCAL_PATH)/../../../../../libstagefrighthw/include/openmax \
    $(LOCAL_PATH)/../../../../../../libmemion                     \
    $(LOCAL_PATH)/../../../../../../../external/kernel-headers    \

LOCAL_C_INCLUDES += $(TOP)/vendor/sprd/external/drivers/gpu

LOCAL_CFLAGS := -DOSCL_EXPORT_REF= -DOSCL_IMPORT_REF=

LOCAL_CFLAGS += -DTARGET_GPU_PLATFORM=$(TARGET_GPU_PLATFORM)

LOCAL_ARM_MODE := arm

LOCAL_SHARED_LIBRARIES := \
    libstagefright_omx         \
    libstagefright_foundation  \
    libstagefrighthw           \
    libutils                   \
    libui                      \
    libmemion                  \
    libdl                      \
    liblog                     \
    libcutils

LOCAL_MODULE := libstagefright_sprd_av1dec
LOCAL_MODULE_TAGS := optional
LOCAL_PROPRIETARY_MODULE := true

include $(BUILD_SHARED_LIBRARY)

