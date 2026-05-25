#pragma once

#include <stdint.h>

struct PluginDecoderConfig {
    char codecName[256];
    char mime[64];
    int32_t expectedFrameRate;
};

struct PluginStreamResolution {
    int32_t width;
    int32_t height;
};

void PluginStreamSession_Begin(const char* codecName,
                               const char* mime,
                               int32_t expectedFrameRate);
PluginDecoderConfig PluginStreamSession_GetDecoderConfig();

void PluginStreamSession_ResetResolution();
void PluginStreamSession_LockResolution(int32_t width, int32_t height);
bool PluginStreamSession_GetLockedResolution(PluginStreamResolution* outResolution);
bool PluginStreamSession_HasResolutionChange(int32_t width,
                                             int32_t height,
                                             PluginStreamResolution* outLockedResolution);
