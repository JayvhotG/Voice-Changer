LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE     := voicefx
LOCAL_SRC_FILES  := voicefx.cpp rubberband-src/single/RubberBandSingle.cpp
LOCAL_C_INCLUDES := $(LOCAL_PATH)/rubberband-src
LOCAL_CPPFLAGS   := -std=c++17 -O3 -fvisibility=hidden -fvisibility-inlines-hidden -fexceptions
LOCAL_LDLIBS     := -llog -ldl
LOCAL_LDFLAGS    := -Wl,--exclude-libs,ALL -Wl,--no-undefined
include $(BUILD_SHARED_LIBRARY)
