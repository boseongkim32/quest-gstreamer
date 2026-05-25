#include "gst_unity_plugin.h"
#include "gst_debug_helpers.h"
#include "gstreamer_source.h"
#include "mediacodec_decoder.h"
#include "plugin_stream_session.h"
#include "plugin_status.h"
#include "vulkan_renderer.h"

#include <gst/gst.h>
#include <stdlib.h>
#include <android/log.h>

#include "IUnityInterface.h"

#define LOG_TAG "GstUnityPlugin"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static bool ReadCapsResolution(GstCaps* caps, int32_t* width, int32_t* height) {
    if (!caps || gst_caps_is_empty(caps) || gst_caps_get_size(caps) <= 0) {
        return false;
    }

    GstStructure* structure = gst_caps_get_structure(caps, 0);
    if (!structure) {
        return false;
    }

    gint capsWidth = 0;
    gint capsHeight = 0;
    if (!gst_structure_get_int(structure, "width", &capsWidth) ||
        !gst_structure_get_int(structure, "height", &capsHeight) ||
        capsWidth <= 0 || capsHeight <= 0) {
        return false;
    }

    *width = capsWidth;
    *height = capsHeight;
    return true;
}

static bool SampleResolutionChanged(GstSample* sample) {
    GstCaps* caps = gst_sample_get_caps(sample);
    int32_t width = 0;
    int32_t height = 0;
    if (!ReadCapsResolution(caps, &width, &height)) {
        return false;
    }

    PluginStreamResolution lockedResolution = {0, 0};
    if (!PluginStreamSession_HasResolutionChange(width, height, &lockedResolution)) {
        return false;
    }

    gchar* capsStr = caps ? gst_caps_to_string(caps) : nullptr;
    LOGE("Unsupported mid-stream resolution change: locked=%dx%d new=%dx%d caps=%s",
         lockedResolution.width,
         lockedResolution.height,
         width,
         height,
         capsStr ? capsStr : "(null)");
    g_free(capsStr);
    return true;
}

static void CleanupStreamResources(PluginStreamState finalState) {
    LOGI("Cleaning up stream resources (finalState=%d)", (int)finalState);
    if (finalState == PluginStreamState::Stopped) {
        PluginStatus_ClearLastError();
    }

    PluginStatus_TransitionTo(finalState);
    PluginStatus_MarkCleanupComplete(finalState);

    GStreamerSource_Stop();

    // Release any AImages still held by the renderer before tearing the decoder down.
    VulkanRenderer_RetireAll();
    MediaCodecDecoder_Destroy();
    VulkanRenderer_Cleanup();
    PluginStreamSession_ResetResolution();
}

static void TransitionToFailed(PluginErrorCode error, bool cleanupRequired) {
    PluginStatus_TransitionToFailed(error, cleanupRequired);
}

static void CheckPipelineFailures() {
    const PluginStreamState state = PluginStatus_GetStreamState();
    if (state != PluginStreamState::Starting && state != PluginStreamState::Streaming) {
        // Drop any stale renderer error that landed after we already left a
        // streaming state so it doesn't bleed into the next session.
        VulkanRenderer_ConsumeError();
        return;
    }

    if (MediaCodecDecoder_ConsumeUnsupportedFormatChange()) {
        TransitionToFailed(PluginErrorCode::UnsupportedFormatChange, true);
        return;
    }

    if (MediaCodecDecoder_ConsumeAsyncError()) {
        TransitionToFailed(PluginErrorCode::DecoderAsyncError, true);
        return;
    }

    if (VulkanRenderer_ConsumeError()) {
        TransitionToFailed(PluginErrorCode::RendererFailed, true);
    }
}

static void ProcessPendingCleanup() {
    PluginStreamState finalState = PluginStreamState::Stopped;
    if (PluginStatus_ConsumePendingCleanup(&finalState)) {
        LOGE("Finalizing terminal stream cleanup (finalState=%s)",
             PluginStatus_StreamStateName(finalState));
        CleanupStreamResources(finalState);
    }
}

static void FinalizeDeferredCleanup() {
    CheckPipelineFailures();
    ProcessPendingCleanup();
}

static void TransitionToStalled() {
    PluginStatus_TransitionToStalled();
}

static void OnSourceEvent(GStreamerSourceEvent event, void* userData) {
    (void)userData;

    if (event == GSTREAMER_SOURCE_EVENT_STREAM_INTERRUPTED) {
        LOGE("Received stream interruption event from GStreamer source");
        TransitionToStalled();
    }
}

static void UNITY_INTERFACE_API OnRenderEvent(int eventId) {
    (void)eventId;

    CheckPipelineFailures();

    if (PluginStatus_GetStreamState() != PluginStreamState::Streaming) {
        return;
    }

    MediaCodecDecoder_DrainOutputs();
    VulkanRenderer_Render(MediaCodecDecoder_GetImageReader());
}

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API
UnityPluginLoad(IUnityInterfaces* unityInterfaces) {
    LOGI("UnityPluginLoad called");
    VulkanRenderer_Load(unityInterfaces);
}

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API
UnityPluginUnload() {
    LOGI("UnityPluginUnload called");
    VulkanRenderer_Unload();
}

extern "C" UnityRenderingEvent UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API
GstUnityPlugin_GetRenderEventFunc() {
    return OnRenderEvent;
}

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API
GstUnityPlugin_SetTexture(void* rgbaTexture, int width, int height) {
    VulkanRenderer_SetTexture(rgbaTexture, width, height);
}

static GstFlowReturn OnStreamSample(GstSample* sample, void* userData) {
    (void)userData;

    CheckPipelineFailures();

    const PluginStreamState currentState = PluginStatus_GetStreamState();
    if (currentState == PluginStreamState::Stopped ||
        currentState == PluginStreamState::Stalled ||
        currentState == PluginStreamState::Failed) {
        return GST_FLOW_OK;
    }

    GstBuffer* buffer = gst_sample_get_buffer(sample);
    if (!buffer) {
        TransitionToFailed(PluginErrorCode::SampleMissingBuffer, true);
        return GST_FLOW_ERROR;
    }

    if (currentState != PluginStreamState::Streaming) {
        GstCaps* caps = gst_sample_get_caps(sample);
        if (!caps) {
            LOGE("First sample is missing caps");
            TransitionToFailed(PluginErrorCode::SampleMissingCaps, true);
            return GST_FLOW_ERROR;
        }

        GstStructure* s = gst_caps_get_structure(caps, 0);
        int32_t width = 0;
        int32_t height = 0;

        gchar* capsStr = gst_caps_to_string(caps);
        LOGI("First H.264 NAL: caps=%s", capsStr ? capsStr : "(null)");

        if (!ReadCapsResolution(caps, &width, &height)) {
            LOGE("First sample caps do not include a fixed resolution: caps=%s",
                 capsStr ? capsStr : "(null)");
            g_free(capsStr);
            TransitionToFailed(PluginErrorCode::SampleMissingResolution, true);
            return GST_FLOW_ERROR;
        }

        g_free(capsStr);
        PluginStreamSession_LockResolution(width, height);
        LOGI("Locked stream session resolution: %dx%d", width, height);

        const PluginDecoderConfig decoderConfig = PluginStreamSession_GetDecoderConfig();
        const char* codecName = decoderConfig.codecName[0] ? decoderConfig.codecName : nullptr;
        const char* mime = decoderConfig.mime[0] ? decoderConfig.mime : nullptr;
        if (!MediaCodecDecoder_Create(width, height, 5, codecName, mime, decoderConfig.expectedFrameRate)) {
            LOGE("Failed to create MediaCodec decoder");
            TransitionToFailed(PluginErrorCode::DecoderCreateFailed, true);
            return GST_FLOW_ERROR;
        }

        const GValue* cdVal = gst_structure_get_value(s, "codec_data");
        if (cdVal) {
            GstBuffer* cdBuf = gst_value_get_buffer(cdVal);
            GstMapInfo cdMap;
            if (gst_buffer_map(cdBuf, &cdMap, GST_MAP_READ)) {
                LOGI("Setting CSD from codec_data: %zu bytes", cdMap.size);
                MediaCodecDecoder_SetCSD(cdMap.data, cdMap.size);
                gst_buffer_unmap(cdBuf, &cdMap);
            }
        }

        if (!MediaCodecDecoder_Start()) {
            LOGE("Failed to start MediaCodec decoder");
            MediaCodecDecoder_Destroy();
            TransitionToFailed(PluginErrorCode::DecoderStartFailed, true);
            return GST_FLOW_ERROR;
        }

        PluginStatus_ClearLastError();
        PluginStatus_TransitionTo(PluginStreamState::Streaming);
    } else if (SampleResolutionChanged(sample)) {
        TransitionToFailed(PluginErrorCode::UnsupportedFormatChange, true);
        return GST_FLOW_ERROR;
    }

    int64_t pts = GST_BUFFER_PTS_IS_VALID(buffer)
        ? (int64_t)(GST_BUFFER_PTS(buffer) / 1000)  // ns -> us
        : 0;

    if (!MediaCodecDecoder_QueueInput(buffer, pts)) {
        static uint64_t dropCount = 0;
        if (dropCount++ % 100 == 0) {
            LOGE("MediaCodec input full, dropped %lu NALs", (unsigned long)dropCount);
        }
    }

    return GST_FLOW_OK;
}

extern "C" {

int32_t GstUnityPlugin_Init() {
    LOGI("Initializing GStreamer");
    setenv("GST_AMC_IGNORE_UNKNOWN_COLOR_FORMATS", "yes", 1);
    gst_init(nullptr, nullptr);
    LOGI("GStreamer version: %s", gst_version_string());
    return 0;
}

int32_t GstUnityPlugin_StartStream(int port,
                                   const char* codecName,
                                   const char* mime,
                                   int32_t expectedFrameRate) {
    LOGI("Starting zero-copy stream on port %d", port);

    const PluginStreamState state = PluginStatus_GetStreamState();
    if (state != PluginStreamState::Stopped) {
        LOGE("Cannot start stream from state %s; call StopStream first",
             PluginStatus_StreamStateName(state));
        return -1;
    }

    PluginStatus_ResetForStart();

    if (!PluginStatus_TransitionTo(PluginStreamState::Starting)) {
        return -1;
    }

    PluginStreamSession_Begin(codecName, mime, expectedFrameRate);

    if (!GStreamerSource_Start(port, OnStreamSample, OnSourceEvent, nullptr)) {
        TransitionToFailed(PluginErrorCode::SourceStartFailed, false);
        return -1;
    }

    LOGI("Zero-copy stream started");
    return 0;
}

void GstUnityPlugin_StopStream() {
    LOGI("Stopping stream");
    CleanupStreamResources(PluginStreamState::Stopped);
    LOGI("Stream stopped");
}

int32_t GstUnityPlugin_IsStreaming() {
    FinalizeDeferredCleanup();
    return PluginStatus_GetStreamState() == PluginStreamState::Streaming ? 1 : 0;
}

int32_t GstUnityPlugin_GetStreamState() {
    FinalizeDeferredCleanup();
    return (int32_t)PluginStatus_GetStreamState();
}

int32_t GstUnityPlugin_GetLastErrorCode() {
    return (int32_t)PluginStatus_GetLastError();
}

int32_t GstUnityPlugin_GetStreamResolution(int32_t* width, int32_t* height) {
    if (!width || !height) {
        return 0;
    }

    PluginStreamResolution resolution = {0, 0};
    if (!PluginStreamSession_GetLockedResolution(&resolution)) {
        return 0;
    }

    *width = resolution.width;
    *height = resolution.height;
    return 1;
}

void GstUnityPlugin_PauseStream() {
    if (GStreamerSource_IsRunning()) {
        LOGI("Pausing stream");
        GStreamerSource_Pause();
    }
}

void GstUnityPlugin_ResumeStream() {
    if (GStreamerSource_IsRunning()) {
        LOGI("Resuming stream");
        GStreamerSource_Resume();
    }
}

void GstUnityPlugin_Shutdown() {
    LOGI("Shutting down GStreamer");
    GstUnityPlugin_StopStream();
    gst_deinit();
}

}
