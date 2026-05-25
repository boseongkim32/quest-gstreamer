#include "plugin_status.h"

#include <android/log.h>
#include <atomic>

#define LOG_TAG "GstUnityPlugin"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static std::atomic<PluginStreamState> s_streamState(PluginStreamState::Stopped);
static std::atomic<bool> s_cleanupPending(false);
static std::atomic<PluginStreamState> s_cleanupFinalState(PluginStreamState::Stopped);
static std::atomic<PluginErrorCode> s_lastError(PluginErrorCode::None);

const char* PluginStatus_StreamStateName(PluginStreamState state) {
    switch (state) {
        case PluginStreamState::Stopped: return "Stopped";
        case PluginStreamState::Starting: return "Starting";
        case PluginStreamState::Streaming: return "Streaming";
        case PluginStreamState::Stalled: return "Stalled";
        case PluginStreamState::Failed: return "Failed";
    }
    return "Unknown";
}

PluginStreamState PluginStatus_GetStreamState() {
    return s_streamState.load();
}

PluginErrorCode PluginStatus_GetLastError() {
    return s_lastError.load();
}

void PluginStatus_ResetForStart() {
    s_cleanupPending.store(false);
    s_cleanupFinalState.store(PluginStreamState::Stopped);
    s_lastError.store(PluginErrorCode::None);
}

void PluginStatus_ClearLastError() {
    s_lastError.store(PluginErrorCode::None);
}

void PluginStatus_MarkCleanupComplete(PluginStreamState finalState) {
    s_streamState.store(finalState);
    s_cleanupPending.store(false);
    s_cleanupFinalState.store(finalState);
}

static bool IsAllowedTransition(PluginStreamState from, PluginStreamState to) {
    if (from == to) {
        return true;
    }

    switch (from) {
        case PluginStreamState::Stopped:
            return to == PluginStreamState::Starting;
        case PluginStreamState::Starting:
            return to == PluginStreamState::Streaming ||
                   to == PluginStreamState::Stalled ||
                   to == PluginStreamState::Failed ||
                   to == PluginStreamState::Stopped;
        case PluginStreamState::Streaming:
            return to == PluginStreamState::Stalled ||
                   to == PluginStreamState::Failed ||
                   to == PluginStreamState::Stopped;
        case PluginStreamState::Stalled:
            return to == PluginStreamState::Failed ||
                   to == PluginStreamState::Stopped;
        case PluginStreamState::Failed:
            return to == PluginStreamState::Stopped;
    }

    return false;
}

bool PluginStatus_TransitionTo(PluginStreamState targetState) {
    PluginStreamState currentState = s_streamState.load();
    while (true) {
        if (currentState == targetState) {
            return true;
        }

        if (!IsAllowedTransition(currentState, targetState)) {
            LOGE("Invalid stream state transition: %s -> %s",
                 PluginStatus_StreamStateName(currentState),
                 PluginStatus_StreamStateName(targetState));
            return false;
        }

        if (s_streamState.compare_exchange_weak(currentState, targetState)) {
            LOGI("Stream state transition: %s -> %s",
                 PluginStatus_StreamStateName(currentState),
                 PluginStatus_StreamStateName(targetState));
            return true;
        }
    }
}

static void RequestTerminalCleanup(PluginStreamState finalState) {
    s_cleanupFinalState.store(finalState);
    s_cleanupPending.store(true);
}

bool PluginStatus_TransitionToFailed(PluginErrorCode error, bool cleanupRequired) {
    s_lastError.store(error);
    if (!PluginStatus_TransitionTo(PluginStreamState::Failed)) {
        LOGE("Transitioning to failed state skipped (error=%d)", (int)error);
        return false;
    }

    if (cleanupRequired) {
        RequestTerminalCleanup(PluginStreamState::Failed);
    }
    LOGE("Transitioning to failed state (error=%d)", (int)error);
    return true;
}

void PluginStatus_TransitionToStalled() {
    const PluginStreamState state = s_streamState.load();
    if (state != PluginStreamState::Starting && state != PluginStreamState::Streaming) {
        return;
    }

    if (PluginStatus_TransitionTo(PluginStreamState::Stalled)) {
        LOGE("Stream stalled: source stopped delivering samples");
    }
}

bool PluginStatus_ConsumePendingCleanup(PluginStreamState* outFinalState) {
    if (!s_cleanupPending.exchange(false)) {
        return false;
    }

    if (outFinalState) {
        *outFinalState = s_cleanupFinalState.load();
    }
    return true;
}

