LOCAL_PATH := $(call my-dir)
ifndef GSTREAMER_ROOT_ANDROID
$(error GSTREAMER_ROOT_ANDROID is not defined!)
endif
GSTREAMER_ROOT := $(GSTREAMER_ROOT_ANDROID)/arm64

# Declare the prebuilt gstreamer_android library
include $(CLEAR_VARS)
LOCAL_MODULE := gstreamer_android
LOCAL_SRC_FILES := ../obj/local/arm64-v8a/libgstreamer_android.so
include $(PREBUILT_SHARED_LIBRARY)

# Build the Unity plugin (.so)
include $(CLEAR_VARS)
LOCAL_MODULE := gst_unity_plugin
LOCAL_SRC_FILES := gst_unity_plugin.cpp mediacodec_decoder.cpp mediacodec_decoder_queue.cpp gst_debug_helpers.cpp vulkan_renderer.cpp gstreamer_source.cpp plugin_status.cpp plugin_stream_session.cpp
LOCAL_C_INCLUDES := $(GSTREAMER_ROOT)/include/gstreamer-1.0 \
                    $(GSTREAMER_ROOT)/include/glib-2.0 \
                    $(GSTREAMER_ROOT)/lib/glib-2.0/include \
                    $(LOCAL_PATH)/../include/Unity
LOCAL_SHARED_LIBRARIES := gstreamer_android
LOCAL_LDLIBS := -llog -lvulkan -lmediandk -landroid
include $(BUILD_SHARED_LIBRARY)
