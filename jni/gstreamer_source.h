#pragma once

#include <gst/gst.h>

// Called for each sample pulled from the appsink. The callback may inspect the
// sample during the call; it must ref it if it needs to keep it afterwards.
typedef GstFlowReturn (*GStreamerSourceSampleCallback)(GstSample* sample, void* userData);

typedef enum {
    GSTREAMER_SOURCE_EVENT_STREAM_INTERRUPTED = 1,
} GStreamerSourceEvent;

typedef void (*GStreamerSourceEventCallback)(GStreamerSourceEvent event, void* userData);

bool GStreamerSource_Start(int port,
                           GStreamerSourceSampleCallback onSample,
                           GStreamerSourceEventCallback onEvent,
                           void* userData);
void GStreamerSource_Stop(void);
void GStreamerSource_Pause(void);
void GStreamerSource_Resume(void);
bool GStreamerSource_IsRunning(void);
