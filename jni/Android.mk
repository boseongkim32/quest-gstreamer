LOCAL_PATH := $(call my-dir)

GSTREAMER_ROOT := $(GSTREAMER_ROOT_ANDROID)/arm64
GSTREAMER_NDK_BUILD_PATH := $(GSTREAMER_ROOT)/share/gst-android/ndk-build

include $(GSTREAMER_NDK_BUILD_PATH)/plugins.mk
GSTREAMER_PLUGINS := coreelements app udp rtp rtpmanager videoconvertscale videotestsrc autodetect playback videoparsersbad libav androidmedia
GSTREAMER_EXTRA_DEPS := gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0

include $(GSTREAMER_NDK_BUILD_PATH)/gstreamer-1.0.mk
