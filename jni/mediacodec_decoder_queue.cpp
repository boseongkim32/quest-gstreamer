#include "mediacodec_decoder_queue.h"
#include "mediacodec_decoder.h"

#include <gst/gst.h>
#include <android/log.h>
#include <pthread.h>
#include <string.h>

#define LOG_TAG "MediaCodecDecoder"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Set to 1 to enable verbose per-frame debug logging.
#define DEBUG_LOG_ENABLED 0
#if DEBUG_LOG_ENABLED
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
#define LOGD(...) ((void)0)
#endif

// Sized for a comfortable codec input/output buffer count plus
// the AImageReader pool. If we ever overflow this, the codec has lost a buffer
// index and is wedged — surface that as a fatal async error rather than
// silently dropping the index.
static const int MAX_PENDING = 32;

struct PendingRelease {
    int32_t index;
};

struct PendingNAL {
    GstBuffer* buffer;
    int64_t timestampUs;
};

static AMediaCodec* s_codec = nullptr;

static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cond = PTHREAD_COND_INITIALIZER;
static pthread_t s_feederThread;
static bool s_feederThreadRunning = false;
static bool s_feederStopRequested = false;

// Available input buffer indices (from OnInputAvailable).
static int32_t s_availableInputs[MAX_PENDING];
static int s_availableInputCount = 0;

// NALs waiting for an input buffer slot.
static PendingNAL s_pendingNALs[MAX_PENDING];
static int s_pendingNALHead = 0;
static int s_pendingNALTail = 0;
static int s_pendingNALCount = 0;

// Output buffer indices waiting to be released-to-surface (from OnOutputAvailable).
static PendingRelease s_pendingReleases[MAX_PENDING];
static int s_pendingReleaseCount = 0;

// Submit a NAL to a known-available input buffer index.
// MUST be called WITHOUT s_mutex held.
static bool SubmitToInputBuffer(int32_t idx, const uint8_t* data, size_t size, int64_t timestampUs) {
    if (!s_codec) {
        LOGE("SubmitToInputBuffer called without an active codec");
        return false;
    }

    size_t bufSize = 0;
    uint8_t* buf = AMediaCodec_getInputBuffer(s_codec, idx, &bufSize);
    if (!buf) {
        LOGE("getInputBuffer returned null for index %d", idx);
        return false;
    }

    if (size > bufSize) {
        LOGE("Oversized NAL: %zu bytes exceeds input buffer capacity (%zu) at pts=%lld; dropping",
             size, bufSize, (long long)timestampUs);
        // Recycle the input index with an empty submission so the codec's
        // buffer pool isn't reduced. Truncating + submitting would corrupt
        // the H.264 bitstream until the next IDR.
        media_status_t recycle = AMediaCodec_queueInputBuffer(
            s_codec, idx, 0, 0, timestampUs, 0);
        if (recycle != AMEDIA_OK) {
            LOGE("queueInputBuffer (recycle on oversized) failed: %d", recycle);
        }
        return false;
    }

    memcpy(buf, data, size);

    media_status_t status = AMediaCodec_queueInputBuffer(
        s_codec, idx, 0, size, timestampUs, 0);

    if (status != AMEDIA_OK) {
        LOGE("queueInputBuffer failed: %d", status);
        return false;
    }

    return true;
}

static void ClearPendingNAL(PendingNAL* nal) {
    if (nal->buffer) {
        gst_buffer_unref(nal->buffer);
        nal->buffer = nullptr;
    }
    nal->timestampUs = 0;
}

static void ClearPendingNALQueueLocked() {
    while (s_pendingNALCount > 0) {
        PendingNAL* nal = &s_pendingNALs[s_pendingNALHead];
        ClearPendingNAL(nal);
        s_pendingNALHead = (s_pendingNALHead + 1) % MAX_PENDING;
        s_pendingNALCount--;
    }
}

static bool IsReadyToSubmitLocked() {
    return s_pendingNALCount > 0 && s_availableInputCount > 0;
}

static bool SubmitPendingNALToInputBuffer(int32_t idx, const PendingNAL& nal) {
    if (!nal.buffer) {
        LOGE("SubmitPendingNALToInputBuffer called with null GstBuffer");
        return false;
    }

    GstMapInfo map;
    if (!gst_buffer_map(nal.buffer, &map, GST_MAP_READ)) {
        LOGE("gst_buffer_map failed for pending NAL");
        return false;
    }

    bool ok = SubmitToInputBuffer(idx, map.data, map.size, nal.timestampUs);
    gst_buffer_unmap(nal.buffer, &map);
    return ok;
}

static void* InputFeederThreadFunc(void* userdata) {
    (void)userdata;
    LOGI("MediaCodec input feeder thread started");

    while (true) {
        int32_t idx = -1;
        PendingNAL nal = {};

        pthread_mutex_lock(&s_mutex);
        while (!s_feederStopRequested && !IsReadyToSubmitLocked()) {
            pthread_cond_wait(&s_cond, &s_mutex);
        }

        if (s_feederStopRequested) {
            pthread_mutex_unlock(&s_mutex);
            break;
        }

        idx = s_availableInputs[--s_availableInputCount];

        PendingNAL* queuedNal = &s_pendingNALs[s_pendingNALHead];
        nal = *queuedNal;
        queuedNal->buffer = nullptr;
        queuedNal->timestampUs = 0;
        s_pendingNALHead = (s_pendingNALHead + 1) % MAX_PENDING;
        s_pendingNALCount--;
        pthread_mutex_unlock(&s_mutex);

        if (!SubmitPendingNALToInputBuffer(idx, nal)) {
            LOGE("Failed to submit queued NAL to MediaCodec input %d", idx);
        }

        ClearPendingNAL(&nal);
    }

    LOGI("MediaCodec input feeder thread exiting");
    return nullptr;
}

void MediaCodecDecoderQueue_Reset(AMediaCodec* codec) {
    pthread_mutex_lock(&s_mutex);
    s_codec = codec;
    s_availableInputCount = 0;
    s_pendingNALHead = 0;
    s_pendingNALTail = 0;
    ClearPendingNALQueueLocked();
    s_pendingReleaseCount = 0;
    s_feederStopRequested = false;
    pthread_mutex_unlock(&s_mutex);
}

bool MediaCodecDecoderQueue_StartFeeder(void) {
    if (!s_codec) {
        LOGE("Cannot start feeder thread without an active codec");
        return false;
    }
    if (s_feederThreadRunning) {
        LOGE("MediaCodec input feeder thread already running");
        return false;
    }

    if (pthread_create(&s_feederThread, nullptr, InputFeederThreadFunc, nullptr) != 0) {
        LOGE("Failed to create MediaCodec input feeder thread");
        return false;
    }

    s_feederThreadRunning = true;
    return true;
}

void MediaCodecDecoderQueue_StopFeeder(void) {
    pthread_mutex_lock(&s_mutex);
    s_feederStopRequested = true;
    pthread_cond_broadcast(&s_cond);
    pthread_mutex_unlock(&s_mutex);

    if (s_feederThreadRunning) {
        pthread_join(s_feederThread, nullptr);
        s_feederThreadRunning = false;
    }
}

void MediaCodecDecoderQueue_ClearPending(void) {
    pthread_mutex_lock(&s_mutex);
    ClearPendingNALQueueLocked();
    s_availableInputCount = 0;
    s_pendingNALHead = 0;
    s_pendingNALTail = 0;
    s_pendingReleaseCount = 0;
    pthread_mutex_unlock(&s_mutex);
}

bool MediaCodecDecoderQueue_QueueInput(GstBuffer* buffer, int64_t timestampUs) {
    if (!buffer) return false;

    pthread_mutex_lock(&s_mutex);

    if (s_feederStopRequested) {
        pthread_mutex_unlock(&s_mutex);
        return false;
    }

    if (s_pendingNALCount >= MAX_PENDING) {
        LOGE("Pending NAL queue full, dropping NAL (%zu bytes)", gst_buffer_get_size(buffer));
        pthread_mutex_unlock(&s_mutex);
        return false;
    }

    // Always push to the pending queue first to guarantee FIFO order.
    PendingNAL* nal = &s_pendingNALs[s_pendingNALTail];
    nal->buffer = gst_buffer_ref(buffer);
    nal->timestampUs = timestampUs;
    s_pendingNALTail = (s_pendingNALTail + 1) % MAX_PENDING;
    s_pendingNALCount++;
    pthread_cond_signal(&s_cond);

    pthread_mutex_unlock(&s_mutex);
    return true;
}

void MediaCodecDecoderQueue_DrainOutputs(void) {
    PendingRelease toRelease[MAX_PENDING];
    int count = 0;

    pthread_mutex_lock(&s_mutex);
    count = s_pendingReleaseCount;
    if (count > 0) {
        memcpy(toRelease, s_pendingReleases, count * sizeof(PendingRelease));
        s_pendingReleaseCount = 0;
    }
    pthread_mutex_unlock(&s_mutex);

    if (!s_codec) {
        return;
    }

    for (int i = 0; i < count; i++) {
        media_status_t status = AMediaCodec_releaseOutputBuffer(s_codec, toRelease[i].index, true);
        if (status != AMEDIA_OK) {
            LOGE("releaseOutputBuffer failed: %d (index=%d)", status, toRelease[i].index);
        }
    }
}

void MediaCodecDecoderQueue_OnInputAvailable(int32_t index) {
    LOGD("OnInputAvailable: index=%d (pending=%d, avail=%d)", index, s_pendingNALCount, s_availableInputCount);

    pthread_mutex_lock(&s_mutex);

    if (s_availableInputCount < MAX_PENDING) {
        s_availableInputs[s_availableInputCount++] = index;
        pthread_cond_signal(&s_cond);
        pthread_mutex_unlock(&s_mutex);
    } else {
        LOGE("Available input index overflow, dropping index %d", index);
        pthread_mutex_unlock(&s_mutex);
        MediaCodecDecoder_SignalAsyncError("input index queue overflow");
    }
}

void MediaCodecDecoderQueue_OnOutputAvailable(int32_t index, int32_t size) {
    LOGD("OnOutputAvailable: index=%d size=%d", index, size);

    pthread_mutex_lock(&s_mutex);
    if (s_pendingReleaseCount < MAX_PENDING) {
        s_pendingReleases[s_pendingReleaseCount].index = index;
        s_pendingReleaseCount++;
        pthread_mutex_unlock(&s_mutex);
    } else {
        LOGE("Pending release overflow, leaking output buffer index %d", index);
        pthread_mutex_unlock(&s_mutex);
        MediaCodecDecoder_SignalAsyncError("output index queue overflow");
    }
}

int MediaCodecDecoderQueue_GetPendingNALCount(void) {
    pthread_mutex_lock(&s_mutex);
    int count = s_pendingNALCount;
    pthread_mutex_unlock(&s_mutex);
    return count;
}

int MediaCodecDecoderQueue_GetAvailableInputCount(void) {
    pthread_mutex_lock(&s_mutex);
    int count = s_availableInputCount;
    pthread_mutex_unlock(&s_mutex);
    return count;
}
