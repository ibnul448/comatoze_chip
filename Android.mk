LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE := comatoze_zygisk
LOCAL_SRC_FILES := module.cpp
LOCAL_LDLIBS := -llog
LOCAL_CPPFLAGS := -std=c++17 -fno-exceptions -fno-rtti
include $(BUILD_SHARED_LIBRARY)
