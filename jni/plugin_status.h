#pragma once

#include "gst_unity_plugin.h"

#include <stdint.h>

// Owns native stream state and terminal cleanup requests.
// gst_unity_plugin.cpp should keep orchestration details only.
// Unity reads state/error via the polling APIs in gst_unity_plugin.h.

enum class PluginStreamState : int32_t {
    Stopped = GST_UNITY_PLUGIN_STREAM_STATE_STOPPED,
    Starting = GST_UNITY_PLUGIN_STREAM_STATE_STARTING,
    Streaming = GST_UNITY_PLUGIN_STREAM_STATE_STREAMING,
    Stalled = GST_UNITY_PLUGIN_STREAM_STATE_STALLED,
    Failed = GST_UNITY_PLUGIN_STREAM_STATE_FAILED,
};

enum class PluginErrorCode : int32_t {
    None = GST_UNITY_PLUGIN_ERROR_NONE,
    SourceStartFailed = GST_UNITY_PLUGIN_ERROR_SOURCE_START_FAILED,
    SampleMissingBuffer = GST_UNITY_PLUGIN_ERROR_SAMPLE_MISSING_BUFFER,
    SampleMissingCaps = GST_UNITY_PLUGIN_ERROR_SAMPLE_MISSING_CAPS,
    DecoderCreateFailed = GST_UNITY_PLUGIN_ERROR_DECODER_CREATE_FAILED,
    DecoderStartFailed = GST_UNITY_PLUGIN_ERROR_DECODER_START_FAILED,
    DecoderAsyncError = GST_UNITY_PLUGIN_ERROR_DECODER_ASYNC_ERROR,
    SampleMissingResolution = GST_UNITY_PLUGIN_ERROR_SAMPLE_MISSING_RESOLUTION,
    UnsupportedFormatChange = GST_UNITY_PLUGIN_ERROR_UNSUPPORTED_FORMAT_CHANGE,
    RendererFailed = GST_UNITY_PLUGIN_ERROR_RENDERER_FAILED,
};

const char* PluginStatus_StreamStateName(PluginStreamState state);

PluginStreamState PluginStatus_GetStreamState();
PluginErrorCode PluginStatus_GetLastError();

void PluginStatus_ResetForStart();
void PluginStatus_ClearLastError();
void PluginStatus_MarkCleanupComplete(PluginStreamState finalState);

bool PluginStatus_TransitionTo(PluginStreamState targetState);
bool PluginStatus_TransitionToFailed(PluginErrorCode error, bool cleanupRequired);
void PluginStatus_TransitionToStalled();

bool PluginStatus_ConsumePendingCleanup(PluginStreamState* outFinalState);
