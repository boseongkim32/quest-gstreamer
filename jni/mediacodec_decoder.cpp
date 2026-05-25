#include "mediacodec_decoder.h"
#include "mediacodec_decoder_queue.h"

#include <gst/gst.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkImageReader.h>
#include <android/native_window.h>
#include <android/log.h>
#include <atomic>

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

static AMediaCodec* s_codec = nullptr;
static AMediaFormat* s_format = nullptr;

// AImageReader surface output (zero-copy path).
static AImageReader* s_imageReader = nullptr;
static ANativeWindow* s_surface = nullptr;

static std::atomic<int32_t> s_configuredWidth(0);
static std::atomic<int32_t> s_configuredHeight(0);
static std::atomic<bool> s_asyncError(false);
static std::atomic<bool> s_unsupportedFormatChange(false);
static std::atomic<bool> s_outputFormatLocked(false);
static std::atomic<int32_t> s_lockedOutputCodedWidth(0);
static std::atomic<int32_t> s_lockedOutputCodedHeight(0);
static std::atomic<int32_t> s_lockedOutputVisibleWidth(0);
static std::atomic<int32_t> s_lockedOutputVisibleHeight(0);

static void DestroyOutputSurface() {
    if (s_imageReader) {
        AImageReader_delete(s_imageReader);
        s_imageReader = nullptr;
    }
    s_surface = nullptr;  // owned by AImageReader
}

static void DestroyCodecObjects() {
    if (s_codec) {
        AMediaCodec_delete(s_codec);
        s_codec = nullptr;
    }
    if (s_format) {
        AMediaFormat_delete(s_format);
        s_format = nullptr;
    }
}

static void ResetOutputFormatTracking() {
    s_outputFormatLocked.store(false);
    s_lockedOutputCodedWidth.store(0);
    s_lockedOutputCodedHeight.store(0);
    s_lockedOutputVisibleWidth.store(0);
    s_lockedOutputVisibleHeight.store(0);
}

static bool CreateOutputSurface(int width, int height, int maxImages) {
    media_status_t status = AImageReader_newWithUsage(
        width > 0 ? width : 1280,
        height > 0 ? height : 720,
        AIMAGE_FORMAT_PRIVATE,
        AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
        maxImages,
        &s_imageReader);

    if (status != AMEDIA_OK || !s_imageReader) {
        LOGE("AImageReader_newWithUsage failed: %d", status);
        return false;
    }

    status = AImageReader_getWindow(s_imageReader, &s_surface);
    if (status != AMEDIA_OK || !s_surface) {
        LOGE("AImageReader_getWindow failed: %d", status);
        DestroyOutputSurface();
        return false;
    }

    return true;
}

static void LogAcceptedFormats() {
    AMediaFormat* inputFmt = AMediaCodec_getInputFormat(s_codec);
    if (inputFmt) {
        LOGI("Post-configure input format: %s", AMediaFormat_toString(inputFmt));
        AMediaFormat_delete(inputFmt);
    }

    AMediaFormat* outputFmt = AMediaCodec_getOutputFormat(s_codec);
    if (outputFmt) {
        LOGI("Post-configure output format: %s", AMediaFormat_toString(outputFmt));
        AMediaFormat_delete(outputFmt);
    }
}

struct OutputFormatResolution {
    int32_t codedWidth;
    int32_t codedHeight;
    int32_t visibleWidth;
    int32_t visibleHeight;
    bool hasCrop;
};

static bool ReadOutputFormatResolution(AMediaFormat* format, OutputFormatResolution* out) {
    if (!format || !out) {
        return false;
    }

    int32_t width = 0;
    int32_t height = 0;
    if (!AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_WIDTH, &width) ||
        !AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_HEIGHT, &height) ||
        width <= 0 || height <= 0) {
        return false;
    }

    int32_t cropLeft = 0;
    int32_t cropTop = 0;
    int32_t cropRight = width - 1;
    int32_t cropBottom = height - 1;
    const bool hasCrop =
        AMediaFormat_getInt32(format, "crop-left", &cropLeft) &&
        AMediaFormat_getInt32(format, "crop-top", &cropTop) &&
        AMediaFormat_getInt32(format, "crop-right", &cropRight) &&
        AMediaFormat_getInt32(format, "crop-bottom", &cropBottom);

    int32_t visibleWidth = width;
    int32_t visibleHeight = height;
    if (hasCrop && cropRight >= cropLeft && cropBottom >= cropTop) {
        visibleWidth = cropRight - cropLeft + 1;
        visibleHeight = cropBottom - cropTop + 1;
    }

    out->codedWidth = width;
    out->codedHeight = height;
    out->visibleWidth = visibleWidth;
    out->visibleHeight = visibleHeight;
    out->hasCrop = hasCrop;
    return true;
}

static void TrackOutputFormatResolution(const OutputFormatResolution& resolution) {
    if (!s_outputFormatLocked.load()) {
        s_lockedOutputCodedWidth.store(resolution.codedWidth);
        s_lockedOutputCodedHeight.store(resolution.codedHeight);
        s_lockedOutputVisibleWidth.store(resolution.visibleWidth);
        s_lockedOutputVisibleHeight.store(resolution.visibleHeight);
        s_outputFormatLocked.store(true);
        LOGI("Locked decoder output format for stream session: coded=%dx%d visible=%dx%d%s",
             resolution.codedWidth,
             resolution.codedHeight,
             resolution.visibleWidth,
             resolution.visibleHeight,
             resolution.hasCrop ? " crop-adjusted" : "");
        return;
    }

    const int32_t lockedCodedWidth = s_lockedOutputCodedWidth.load();
    const int32_t lockedCodedHeight = s_lockedOutputCodedHeight.load();
    const int32_t lockedVisibleWidth = s_lockedOutputVisibleWidth.load();
    const int32_t lockedVisibleHeight = s_lockedOutputVisibleHeight.load();
    if (resolution.codedWidth != lockedCodedWidth ||
        resolution.codedHeight != lockedCodedHeight ||
        resolution.visibleWidth != lockedVisibleWidth ||
        resolution.visibleHeight != lockedVisibleHeight) {
        LOGE("Unsupported decoder output format change: locked coded=%dx%d visible=%dx%d, new coded=%dx%d visible=%dx%d",
             lockedCodedWidth,
             lockedCodedHeight,
             lockedVisibleWidth,
             lockedVisibleHeight,
             resolution.codedWidth,
             resolution.codedHeight,
             resolution.visibleWidth,
             resolution.visibleHeight);
        s_unsupportedFormatChange.store(true);
    }
}

//------------------------------------------------------------------------------
// Async callbacks (called on MediaCodec's internal looper thread)
// RULE: These must NEVER call AMediaCodec_* APIs directly.
//------------------------------------------------------------------------------

static void OnInputAvailable(AMediaCodec* codec, void* userdata, int32_t index) {
    (void)codec;
    (void)userdata;
    MediaCodecDecoderQueue_OnInputAvailable(index);
}

static void OnOutputAvailable(AMediaCodec* codec, void* userdata,
                              int32_t index, AMediaCodecBufferInfo* info) {
    (void)codec;
    (void)userdata;
    MediaCodecDecoderQueue_OnOutputAvailable(index, info ? info->size : 0);
}

static void OnFormatChanged(AMediaCodec* codec, void* userdata, AMediaFormat* format) {
    (void)codec;
    (void)userdata;
    if (!format) return;

    const char* fmtStr = AMediaFormat_toString(format);
    LOGI("Output format changed: %s", fmtStr);

    int32_t w = 0;
    int32_t h = 0;
    int32_t stride = 0;
    int32_t sliceHeight = 0;
    int32_t colorFmt = 0;
    AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_WIDTH, &w);
    AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_HEIGHT, &h);
    AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_STRIDE, &stride);
    AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, &colorFmt);
    AMediaFormat_getInt32(format, "slice-height", &sliceHeight);
    LOGI("Decoded: %dx%d stride=%d sliceHeight=%d colorFormat=0x%x",
         w, h, stride, sliceHeight, colorFmt);

    OutputFormatResolution resolution = {};
    if (ReadOutputFormatResolution(format, &resolution)) {
        TrackOutputFormatResolution(resolution);
        s_configuredWidth.store(resolution.codedWidth);
        s_configuredHeight.store(resolution.codedHeight);
    } else {
        LOGE("Output format change did not include a valid resolution");
    }
}

static void OnError(AMediaCodec* codec, void* userdata,
                    media_status_t error, int32_t actionCode, const char* detail) {
    (void)codec;
    (void)userdata;
    LOGE("MediaCodec async error: %d, actionCode=%d, detail=%s",
         error, actionCode, detail ? detail : "(null)");
    s_asyncError.store(true);
}

//------------------------------------------------------------------------------
// Public API
//------------------------------------------------------------------------------

bool MediaCodecDecoder_Create(int width, int height, int maxImages,
                              const char* codecName, const char* mime, int frameRate) {
    if (s_codec) {
        LOGE("Decoder already created");
        return false;
    }
    s_asyncError.store(false);
    s_unsupportedFormatChange.store(false);
    ResetOutputFormatTracking();

    const char* effectiveMime = (mime && mime[0]) ? mime : "video/avc";

    if (codecName && codecName[0]) {
        s_codec = AMediaCodec_createCodecByName(codecName);
        if (s_codec) {
            LOGI("Using requested decoder: %s", codecName);
        } else {
            LOGE("Requested decoder '%s' not available, falling back to generic %s", codecName, effectiveMime);
            s_codec = AMediaCodec_createDecoderByType(effectiveMime);
        }
    } else {
        LOGI("No decoder specified, using generic %s", effectiveMime);
        s_codec = AMediaCodec_createDecoderByType(effectiveMime);
    }

    if (!s_codec) {
        LOGE("Failed to create AMediaCodec for %s", effectiveMime);
        return false;
    }

    s_format = AMediaFormat_new();
    AMediaFormat_setString(s_format, AMEDIAFORMAT_KEY_MIME, effectiveMime);
    if (width > 0 && height > 0) {
        AMediaFormat_setInt32(s_format, AMEDIAFORMAT_KEY_WIDTH, width);
        AMediaFormat_setInt32(s_format, AMEDIAFORMAT_KEY_HEIGHT, height);
    }
    if (frameRate > 0) {
        AMediaFormat_setInt32(s_format, AMEDIAFORMAT_KEY_FRAME_RATE, frameRate);
        LOGI("Frame rate set: %d", frameRate);
    }

    if (!CreateOutputSurface(width, height, maxImages)) {
        DestroyCodecObjects();
        return false;
    }

    s_configuredWidth.store(width);
    s_configuredHeight.store(height);

    LOGI("MediaCodec created: %dx%d (AImageReader maxImages=%d)", width, height, maxImages);
    return true;
}

bool MediaCodecDecoder_SetCSD(const uint8_t* csd, size_t size) {
    if (!s_format || !csd || size == 0) return false;

    AMediaFormat_setBuffer(s_format, "csd-0", csd, size);
    LOGI("CSD set: %zu bytes", size);
    return true;
}

bool MediaCodecDecoder_Start() {
    if (!s_codec || !s_format || !s_surface) {
        LOGE("Decoder not created or surface not available");
        return false;
    }
    s_asyncError.store(false);
    s_unsupportedFormatChange.store(false);
    ResetOutputFormatTracking();

    AMediaFormat_setInt32(s_format, AMEDIAFORMAT_KEY_PRIORITY, 0); // 0 = realtime priority
    LOGI("MediaCodec format: %s", AMediaFormat_toString(s_format));

    media_status_t status = AMediaCodec_configure(s_codec, s_format, s_surface, nullptr, 0);
    if (status != AMEDIA_OK) {
        LOGE("AMediaCodec_configure failed: %d", status);
        return false;
    }

    LogAcceptedFormats();

    AMediaCodecOnAsyncNotifyCallback asyncCb = {};
    asyncCb.onAsyncInputAvailable = OnInputAvailable;
    asyncCb.onAsyncOutputAvailable = OnOutputAvailable;
    asyncCb.onAsyncFormatChanged = OnFormatChanged;
    asyncCb.onAsyncError = OnError;

    status = AMediaCodec_setAsyncNotifyCallback(s_codec, asyncCb, nullptr);
    if (status != AMEDIA_OK) {
        LOGE("AMediaCodec_setAsyncNotifyCallback failed: %d", status);
        return false;
    }

    MediaCodecDecoderQueue_Reset(s_codec);

    status = AMediaCodec_start(s_codec);
    if (status != AMEDIA_OK) {
        LOGE("AMediaCodec_start failed: %d", status);
        MediaCodecDecoderQueue_Reset(nullptr);
        return false;
    }

    if (!MediaCodecDecoderQueue_StartFeeder()) {
        AMediaCodec_stop(s_codec);
        MediaCodecDecoderQueue_Reset(nullptr);
        return false;
    }

    LOGI("MediaCodec started (async mode, surface output)");
    return true;
}

bool MediaCodecDecoder_ConsumeAsyncError() {
    return s_asyncError.exchange(false);
}

void MediaCodecDecoder_SignalAsyncError(const char* reason) {
    LOGE("MediaCodec async error signalled: %s", reason ? reason : "(unspecified)");
    s_asyncError.store(true);
}

bool MediaCodecDecoder_ConsumeUnsupportedFormatChange() {
    return s_unsupportedFormatChange.exchange(false);
}

bool MediaCodecDecoder_QueueInput(GstBuffer* buffer, int64_t timestampUs) {
    if (!s_codec || !buffer) return false;

    LOGD("QueueInput: size=%zu pts=%lld (avail=%d, pending=%d)",
         gst_buffer_get_size(buffer),
         (long long)timestampUs,
         MediaCodecDecoderQueue_GetAvailableInputCount(),
         MediaCodecDecoderQueue_GetPendingNALCount());

    return MediaCodecDecoderQueue_QueueInput(buffer, timestampUs);
}

void MediaCodecDecoder_DrainOutputs() {
    MediaCodecDecoderQueue_DrainOutputs();
}

AImageReader* MediaCodecDecoder_GetImageReader() {
    return s_imageReader;
}

void MediaCodecDecoder_Destroy() {
    LOGI("Destroying MediaCodec decoder");

    MediaCodecDecoderQueue_StopFeeder();

    if (s_codec) {
        AMediaCodec_stop(s_codec);
    }

    DestroyCodecObjects();
    DestroyOutputSurface();
    MediaCodecDecoderQueue_ClearPending();
    MediaCodecDecoderQueue_Reset(nullptr);

    s_configuredWidth.store(0);
    s_configuredHeight.store(0);
    s_asyncError.store(false);
    s_unsupportedFormatChange.store(false);
    ResetOutputFormatTracking();

    LOGI("MediaCodec decoder destroyed");
}
