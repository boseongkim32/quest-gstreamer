#include "gstreamer_source.h"

#include <gst/app/gstappsink.h>
#include <atomic>
#include <pthread.h>
#include <stdio.h>
#include <android/log.h>

#define LOG_TAG "GstUnityPlugin"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static constexpr unsigned long long STREAM_INTERRUPT_TIMEOUT_NS = 1500ULL * 1000 * 1000;

static GstElement* s_pipeline = nullptr;
static GstElement* s_appsink = nullptr;
static GMainLoop* s_mainLoop = nullptr;
static pthread_t s_mainLoopThread;
static bool s_mainLoopThreadStarted = false;
static guint s_busWatchId = 0;
static std::atomic<bool> s_running(false);
static std::atomic<bool> s_stopping(false);
static std::atomic<bool> s_paused(false);
static std::atomic<bool> s_streamInterrupted(false);
static GStreamerSourceSampleCallback s_onSample = nullptr;
static GStreamerSourceEventCallback s_onEvent = nullptr;
static void* s_userData = nullptr;

static void NotifyStreamInterrupted(const char* reason) {
    if (s_stopping.load() || s_paused.load()) {
        return;
    }

    bool expected = false;
    if (!s_streamInterrupted.compare_exchange_strong(expected, true)) {
        return;
    }

    LOGE("Stream interrupted: %s", reason);
    if (s_onEvent) {
        s_onEvent(GSTREAMER_SOURCE_EVENT_STREAM_INTERRUPTED, s_userData);
    }
}

static GstFlowReturn OnNewSample(GstElement* sink, gpointer userData) {
    (void)userData;

    GstSample* sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
    if (!sample) {
        return GST_FLOW_ERROR;
    }

    if (s_stopping.load() || s_streamInterrupted.load()) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    GstFlowReturn result = GST_FLOW_OK;
    if (s_onSample) {
        result = s_onSample(sample, s_userData);
    }

    gst_sample_unref(sample);
    return result;
}

static void* GStreamerThreadFunc(void* arg) {
    (void)arg;
    LOGI("GStreamer thread started");
    g_main_loop_run(s_mainLoop);
    LOGI("GStreamer thread exiting");
    return nullptr;
}

static gboolean OnBusMessage(GstBus* bus, GstMessage* message, gpointer userData) {
    (void)bus;
    (void)userData;

    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ELEMENT) {
        const GstStructure* structure = gst_message_get_structure(message);
        if (structure && gst_structure_has_name(structure, "GstUDPSrcTimeout")) {
            guint64 timeoutNs = STREAM_INTERRUPT_TIMEOUT_NS;
            gst_structure_get_uint64(structure, "timeout", &timeoutNs);

            char reason[96];
            snprintf(reason, sizeof(reason),
                     "no UDP packets for %.1f ms",
                     (double)timeoutNs / 1000000.0);
            NotifyStreamInterrupted(reason);
        }
    } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
        GError* error = nullptr;
        gchar* debug = nullptr;
        gst_message_parse_error(message, &error, &debug);
        LOGE("GStreamer bus error: %s", error ? error->message : "(unknown)");
        if (debug) {
            LOGE("GStreamer bus debug: %s", debug);
        }
        g_clear_error(&error);
        g_free(debug);
    }

    return G_SOURCE_CONTINUE;
}

bool GStreamerSource_Start(int port,
                           GStreamerSourceSampleCallback onSample,
                           GStreamerSourceEventCallback onEvent,
                           void* userData) {
    if (s_pipeline || s_running.load()) {
        LOGE("GStreamer source already running");
        return false;
    }

    s_onSample = onSample;
    s_onEvent = onEvent;
    s_userData = userData;
    s_stopping.store(false);
    s_paused.store(false);
    s_streamInterrupted.store(false);

    char pipelineStr[512];
    snprintf(pipelineStr, sizeof(pipelineStr),
        "udpsrc port=%d timeout=%llu caps=\"application/x-rtp,media=video,encoding-name=H264,payload=96,clock-rate=90000\" ! "
        "rtph264depay ! "
        "h264parse config-interval=-1 ! "
        "video/x-h264,stream-format=byte-stream,alignment=au ! "
        "appsink name=sink emit-signals=true sync=false drop=false max-buffers=0",
        port,
        STREAM_INTERRUPT_TIMEOUT_NS);

    LOGI("Pipeline: %s", pipelineStr);

    GError* error = nullptr;
    s_pipeline = gst_parse_launch(pipelineStr, &error);
    if (error) {
        LOGE("Pipeline error: %s", error->message);
        g_error_free(error);
        s_onSample = nullptr;
        s_onEvent = nullptr;
        s_userData = nullptr;
        return false;
    }

    s_appsink = gst_bin_get_by_name(GST_BIN(s_pipeline), "sink");
    g_signal_connect(s_appsink, "new-sample", G_CALLBACK(OnNewSample), nullptr);

    s_mainLoop = g_main_loop_new(nullptr, FALSE);

    GstBus* bus = gst_element_get_bus(s_pipeline);
    s_busWatchId = gst_bus_add_watch(bus, OnBusMessage, nullptr);
    gst_object_unref(bus);
    if (s_busWatchId == 0) {
        LOGE("Failed to attach GStreamer bus watch");
        GStreamerSource_Stop();
        return false;
    }

    gst_element_set_state(s_pipeline, GST_STATE_PLAYING);

    GstState state;
    GstState pending;
    GstStateChangeReturn ret = gst_element_get_state(s_pipeline, &state, &pending, GST_CLOCK_TIME_NONE);
    LOGI("Pipeline state change return: %d, current state: %d", ret, state);

    if (ret == GST_STATE_CHANGE_FAILURE) {
        LOGE("Failed to set pipeline to PLAYING");

        GstBus* bus = gst_element_get_bus(s_pipeline);
        GstMessage* msg = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
        if (msg) {
            GError* err;
            gchar* debug;
            gst_message_parse_error(msg, &err, &debug);
            LOGE("Error: %s", err->message);
            LOGE("Debug: %s", debug);
            g_error_free(err);
            g_free(debug);
            gst_message_unref(msg);
        }
        gst_object_unref(bus);
        GStreamerSource_Stop();
        return false;
    }

    s_running.store(true);

    if (pthread_create(&s_mainLoopThread, nullptr, GStreamerThreadFunc, nullptr) != 0) {
        LOGE("Failed to create GStreamer thread");
        s_running.store(false);
        GStreamerSource_Stop();
        return false;
    }
    s_mainLoopThreadStarted = true;

    return true;
}

void GStreamerSource_Stop(void) {
    s_stopping.store(true);
    s_paused.store(false);
    s_running.store(false);

    if (s_appsink) {
        g_signal_handlers_disconnect_by_func(s_appsink, (gpointer)OnNewSample, nullptr);
    }

    if (s_pipeline) {
        gst_element_set_state(s_pipeline, GST_STATE_NULL);
    }

    if (s_busWatchId != 0) {
        g_source_remove(s_busWatchId);
        s_busWatchId = 0;
    }

    if (s_mainLoop) {
        g_main_loop_quit(s_mainLoop);
    }

    if (s_mainLoopThreadStarted) {
        pthread_join(s_mainLoopThread, nullptr);
        s_mainLoopThreadStarted = false;
    }

    if (s_pipeline) {
        gst_object_unref(s_pipeline);
        s_pipeline = nullptr;
    }

    if (s_appsink) {
        gst_object_unref(s_appsink);
        s_appsink = nullptr;
    }

    if (s_mainLoop) {
        g_main_loop_unref(s_mainLoop);
        s_mainLoop = nullptr;
    }

    s_onSample = nullptr;
    s_onEvent = nullptr;
    s_userData = nullptr;
    s_streamInterrupted.store(false);
}

void GStreamerSource_Pause(void) {
    if (s_pipeline) {
        s_paused.store(true);
        gst_element_set_state(s_pipeline, GST_STATE_PAUSED);
    }
}

void GStreamerSource_Resume(void) {
    if (s_pipeline) {
        s_paused.store(false);
        gst_element_set_state(s_pipeline, GST_STATE_PLAYING);
    }
}

bool GStreamerSource_IsRunning(void) {
    return s_running.load();
}
