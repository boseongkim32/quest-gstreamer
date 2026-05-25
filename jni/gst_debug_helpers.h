#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void GstUnityPlugin_ListDecoders(void);
void GstUnityPlugin_ListPlugins(void);
void GstUnityPlugin_InspectDecoder(const char* decoderName);

#ifdef __cplusplus
}
#endif
