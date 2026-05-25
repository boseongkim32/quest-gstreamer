#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkImageReader.h>
#include <android/hardware_buffer.h>

typedef struct _GstBuffer GstBuffer;

// Create the decoder with AImageReader surface output.
// width/height can be 0 if unknown (will be determined from stream).
// maxImages: number of AImage slots the AImageReader can hold (5 recommended).
// codecName: specific codec name (e.g. "c2.qti.avc.decoder.low_latency").
//            If null/empty, falls back to generic AMediaCodec_createDecoderByType.
// mime: MIME type (e.g. "video/avc", "video/hevc"). Defaults to "video/avc" if null.
// frameRate: expected incoming framerate. If > 0, sets AMEDIAFORMAT_KEY_FRAME_RATE.
//            Helps the decoder schedule resources appropriately.
bool MediaCodecDecoder_Create(int width, int height, int maxImages,
                              const char* codecName, const char* mime, int frameRate);

// Feed codec-specific data (SPS/PPS in Annex-B format).
// Must be called before Start if providing CSD explicitly.
bool MediaCodecDecoder_SetCSD(const uint8_t* csd, size_t size);

// Start the decoder. Decoded frames go to the AImageReader surface.
// Poll with AImageReader_acquireLatestImage on the render thread.
bool MediaCodecDecoder_Start();

// Returns true once if MediaCodec reports an async decoder error.
bool MediaCodecDecoder_ConsumeAsyncError();

// Sets the async-error flag from outside the OnError callback (e.g. when the
// queue layer detects an unrecoverable condition such as buffer-index overflow).
// Picked up on the next MediaCodecDecoder_ConsumeAsyncError call.
void MediaCodecDecoder_SignalAsyncError(const char* reason);

// Returns true once if MediaCodec reports an output resolution change after the
// first output format has locked this stream session's decoder format.
bool MediaCodecDecoder_ConsumeUnsupportedFormatChange();

// Queue an encoded H.264 access unit backed by a GstBuffer.
// The decoder takes its own ref to the buffer. Submission happens on an
// internal feeder thread as soon as a MediaCodec input slot is available.
// Returns false if the queue is full or the decoder is not running.
bool MediaCodecDecoder_QueueInput(GstBuffer* buffer, int64_t timestampUs);

// Drain decoded frames to the AImageReader surface.
// Call from the render thread before acquireLatestImage so frames
// don't sit waiting for the next QueueInput call.
void MediaCodecDecoder_DrainOutputs();

// Get the AImageReader for polling decoded frames on the render thread.
// Returns nullptr if the decoder hasn't been created yet.
AImageReader* MediaCodecDecoder_GetImageReader();

// Stop and destroy the decoder.
void MediaCodecDecoder_Destroy();
