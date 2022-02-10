LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
    SPRDMP3Decoder.cpp

LOCAL_C_INCLUDES := \
    frameworks/av/media/libstagefright/include                    \
    frameworks/av/include                    \
    frameworks/av/include/media/stagefright                       \
    $(LOCAL_PATH)/../../../libstagefrighthw/include         \
    $(LOCAL_PATH)/../../../libstagefrighthw/include/openmax \
    $(LOCAL_PATH)/../../../../libmemion \
    frameworks/av/include

LOCAL_CFLAGS := -DOSCL_EXPORT_REF= -DOSCL_IMPORT_REF=

LOCAL_LDFLAGS += -Wl

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
    libcutils                  \
    liblog

LOCAL_MODULE := libstagefright_sprd_mp3dec
LOCAL_MODULE_TAGS := optional
LOCAL_PROPRIETARY_MODULE := true

include $(BUILD_SHARED_LIBRARY)
