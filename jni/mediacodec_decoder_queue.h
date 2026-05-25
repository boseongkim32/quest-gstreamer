#pragma once

#include <stdint.h>
#include <media/NdkMediaCodec.h>

typedef struct _GstBuffer GstBuffer;

// Internal queue/feeder subsystem for the single MediaCodec decoder instance.
// Owns:
// - pending GstBuffer input queue
// - available MediaCodec input indices
// - pending surface-release output indices
// - feeder thread that copies queued GstBuffers into codec input buffers
void MediaCodecDecoderQueue_Reset(AMediaCodec* codec);
bool MediaCodecDecoderQueue_StartFeeder(void);
void MediaCodecDecoderQueue_StopFeeder(void);
void MediaCodecDecoderQueue_ClearPending(void);

bool MediaCodecDecoderQueue_QueueInput(GstBuffer* buffer, int64_t timestampUs);
void MediaCodecDecoderQueue_DrainOutputs(void);

void MediaCodecDecoderQueue_OnInputAvailable(int32_t index);
void MediaCodecDecoderQueue_OnOutputAvailable(int32_t index, int32_t size);

int MediaCodecDecoderQueue_GetPendingNALCount(void);
int MediaCodecDecoderQueue_GetAvailableInputCount(void);
