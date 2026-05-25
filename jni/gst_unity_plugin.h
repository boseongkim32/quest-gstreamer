#ifndef GST_UNITY_PLUGIN_H
#define GST_UNITY_PLUGIN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GST_UNITY_PLUGIN_STREAM_STATE_STOPPED = 0,
    GST_UNITY_PLUGIN_STREAM_STATE_STARTING = 1,
    GST_UNITY_PLUGIN_STREAM_STATE_STREAMING = 2,
    GST_UNITY_PLUGIN_STREAM_STATE_STALLED = 3,
    GST_UNITY_PLUGIN_STREAM_STATE_STREAM_LOST = 3, // Backward-compatible alias.
    GST_UNITY_PLUGIN_STREAM_STATE_FAILED = 4,
} GstUnityPluginStreamState;

// Stream state transition contract:
// Stopped -> Starting
// Starting -> Streaming | Stalled | Failed | Stopped
// Streaming -> Stalled | Failed | Stopped
// Stalled -> Failed | Stopped
// Failed -> Stopped
//
// Stalled means the source stopped delivering samples after a valid stream had
// been observed. Failed means the current session hit a fatal native error, such
// as decoder failure or an unsupported mid-stream format change. Restart paths
// should call StopStream before StartStream.

typedef enum {
    GST_UNITY_PLUGIN_ERROR_NONE = 0,
    GST_UNITY_PLUGIN_ERROR_SOURCE_START_FAILED = 1,
    GST_UNITY_PLUGIN_ERROR_SAMPLE_MISSING_BUFFER = 2,
    GST_UNITY_PLUGIN_ERROR_SAMPLE_MISSING_CAPS = 3,
    GST_UNITY_PLUGIN_ERROR_DECODER_CREATE_FAILED = 4,
    GST_UNITY_PLUGIN_ERROR_DECODER_START_FAILED = 5,
    GST_UNITY_PLUGIN_ERROR_DECODER_ASYNC_ERROR = 6,
    GST_UNITY_PLUGIN_ERROR_SAMPLE_MISSING_RESOLUTION = 7,
    GST_UNITY_PLUGIN_ERROR_UNSUPPORTED_FORMAT_CHANGE = 8,
    GST_UNITY_PLUGIN_ERROR_RENDERER_FAILED = 9,
} GstUnityPluginErrorCode;

// Typical usage order from Unity:
// 1. GstUnityPlugin_Init()
// 2. Create the Unity RenderTexture and call GstUnityPlugin_SetTexture()
// 3. GstUnityPlugin_StartStream()
// 4. Each frame: GL.IssuePluginEvent(GstUnityPlugin_GetRenderEventFunc(), 0)
// 5. GstUnityPlugin_StopStream()
// 6. GstUnityPlugin_Shutdown()

// Initialization
int32_t GstUnityPlugin_Init(void);
void GstUnityPlugin_Shutdown(void);

// Decoder configuration is captured as part of StartStream.
// codecName: specific MediaCodec name, or NULL/"" for generic fallback.
// mime: "video/avc" or "video/hevc". NULL/empty defaults to "video/avc".
// expectedFrameRate: if > 0, sets AMEDIAFORMAT_KEY_FRAME_RATE on the decoder format.

// Stream control
int32_t GstUnityPlugin_StartStream(int port,
                                   const char* codecName,
                                   const char* mime,
                                   int32_t expectedFrameRate);
void GstUnityPlugin_StopStream(void);
void GstUnityPlugin_PauseStream(void);
void GstUnityPlugin_ResumeStream(void);

// Frame info
// Returns 1 only while the decoder/render path is actively streaming.
// Returns 0 during startup, after stop, or after an interruption transitions
// the plugin into the stalled or failed state.
int32_t GstUnityPlugin_IsStreaming(void);
// Returns GstUnityPluginStreamState.
int32_t GstUnityPlugin_GetStreamState(void);
// Returns GstUnityPluginErrorCode. Meaningful when stream state is failed.
int32_t GstUnityPlugin_GetLastErrorCode(void);
// Returns 1 when the current stream session has locked its first-frame
// resolution and writes it to width/height. Returns 0 before first sample.
int32_t GstUnityPlugin_GetStreamResolution(int32_t* width, int32_t* height);

// Texture setup (single RGBA8 texture, zero-copy YCbCr → RGBA blit)
void GstUnityPlugin_SetTexture(void* rgbaTexture, int width, int height);

// Unity render callback
typedef void (*UnityRenderingEvent)(int eventId);
UnityRenderingEvent GstUnityPlugin_GetRenderEventFunc(void);

// Optional debug helpers. These are not required for normal playback.
void GstUnityPlugin_ListDecoders(void);
void GstUnityPlugin_ListPlugins(void);
void GstUnityPlugin_InspectDecoder(const char* decoderName);

#ifdef __cplusplus
}
#endif

#endif // GST_UNITY_PLUGIN_H
