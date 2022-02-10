LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:=                     \
        sprd_omx_core.cpp              \

LOCAL_C_INCLUDES += \
        vendor/sprd/modules/libmemion                     \
        $(LOCAL_PATH)/../libstagefrighthw/include         \
        $(LOCAL_PATH)/../libstagefrighthw/include/openmax

LOCAL_SHARED_LIBRARIES :=               \
        libstagefright_foundation       \
        libstagefrighthw                \
        libcutils                       \
        libutils                        \
        libdl                           \
        liblog

ifneq ($(filter $(TARGET_BOARD_PLATFORM), sp9850ka), )
LOCAL_CFLAGS += -DPLATFORM_SHARKL2
endif

ifneq ($(filter $(TARGET_BOARD_PLATFORM), ums510), )
LOCAL_CFLAGS += -DPLATFORM_SHARKL5
endif

ifneq ($(filter ums512 ums518 ums518-zebu, $(strip $(TARGET_BOARD_PLATFORM))), )
LOCAL_CFLAGS += -DPLATFORM_SHARKL5PRO
endif

ifneq ($(filter $(TARGET_BOARD_PLATFORM), sp9863a), )
LOCAL_CFLAGS += -DPLATFORM_SHARKL3
endif

ifneq ($(filter $(TARGET_BOARD_PLATFORM), sp9853i), )
LOCAL_CFLAGS += -DPLATFORM_ISHARKL2
endif

ifneq ($(filter $(TARGET_BOARD_PLATFORM), ud710), )
LOCAL_CFLAGS += -DPLATFORM_ROC1
endif

LOCAL_MODULE:= libsprd_omx_core

LOCAL_PROPRIETARY_MODULE := true
include $(BUILD_SHARED_LIBRARY)

################################################################################

include $(call all-makefiles-under,$(LOCAL_PATH))
