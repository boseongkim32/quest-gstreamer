#include "plugin_stream_session.h"

#include <android/log.h>
#include <atomic>
#include <stdio.h>

#define LOG_TAG "GstUnityPlugin"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

// Captured once during StartStream before source callbacks are installed.
static PluginDecoderConfig s_decoderConfig = {{0}, {0}, 0};

static std::atomic<int32_t> s_streamWidth(0);
static std::atomic<int32_t> s_streamHeight(0);

void PluginStreamSession_Begin(const char* codecName,
                               const char* mime,
                               int32_t expectedFrameRate) {
    PluginDecoderConfig nextConfig = {{0}, {0}, expectedFrameRate};

    if (codecName && codecName[0]) {
        snprintf(nextConfig.codecName, sizeof(nextConfig.codecName), "%s", codecName);
    }

    if (mime && mime[0]) {
        snprintf(nextConfig.mime, sizeof(nextConfig.mime), "%s", mime);
    }

    s_decoderConfig = nextConfig;
    PluginStreamSession_ResetResolution();

    LOGI("Decoder config: codec='%s' mime='%s' expectedFrameRate=%d",
         nextConfig.codecName,
         nextConfig.mime,
         nextConfig.expectedFrameRate);
}

PluginDecoderConfig PluginStreamSession_GetDecoderConfig() {
    return s_decoderConfig;
}

void PluginStreamSession_ResetResolution() {
    s_streamWidth.store(0);
    s_streamHeight.store(0);
}

void PluginStreamSession_LockResolution(int32_t width, int32_t height) {
    s_streamWidth.store(width);
    s_streamHeight.store(height);
}

bool PluginStreamSession_GetLockedResolution(PluginStreamResolution* outResolution) {
    const int32_t lockedWidth = s_streamWidth.load();
    const int32_t lockedHeight = s_streamHeight.load();

    if (outResolution) {
        outResolution->width = lockedWidth;
        outResolution->height = lockedHeight;
    }

    return lockedWidth > 0 && lockedHeight > 0;
}

bool PluginStreamSession_HasResolutionChange(int32_t width,
                                             int32_t height,
                                             PluginStreamResolution* outLockedResolution) {
    PluginStreamResolution lockedResolution = {0, 0};
    if (!PluginStreamSession_GetLockedResolution(&lockedResolution)) {
        return false;
    }

    if (outLockedResolution) {
        *outLockedResolution = lockedResolution;
    }

    return width != lockedResolution.width || height != lockedResolution.height;
}
