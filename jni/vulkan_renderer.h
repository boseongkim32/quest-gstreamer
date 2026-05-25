#pragma once

#include <media/NdkImageReader.h>
#include "IUnityInterface.h"

// Owns the Unity/Vulkan-side rendering path:
// AImageReader acquire -> AHardwareBuffer import -> YCbCr compute blit.
void VulkanRenderer_Load(IUnityInterfaces* unityInterfaces);
void VulkanRenderer_Unload(void);
void VulkanRenderer_SetTexture(void* rgbaTexture, int width, int height);
void VulkanRenderer_Render(AImageReader* reader);
void VulkanRenderer_RetireAll(void);
void VulkanRenderer_Cleanup(void);

// Returns true once if Render hit a fatal Vulkan error since the last call.
// Mirrors MediaCodecDecoder_ConsumeAsyncError so the orchestrator can drive
// stream-state transitions from a single place.
bool VulkanRenderer_ConsumeError(void);
