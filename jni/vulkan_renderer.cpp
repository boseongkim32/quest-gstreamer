#include "vulkan_renderer.h"

#include <atomic>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <android/log.h>
#include <android/hardware_buffer.h>
#include <media/NdkImage.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>

#include "IUnityGraphics.h"
#include "IUnityGraphicsVulkan.h"
#include "ycbcr_blit_spv.h"

#define LOG_TAG "GstUnityPlugin"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static IUnityGraphicsVulkan* s_unityVulkan = nullptr;

// Unity texture — single RGBA8 output.
static void* s_textureHandle = nullptr;
static VkImageView s_unityImageView = VK_NULL_HANDLE;
static VkImage s_unityImage = VK_NULL_HANDLE;

// Vulkan extension function pointers.
static PFN_vkGetAndroidHardwareBufferPropertiesANDROID pfnGetAHBProps = nullptr;

// YCbCr conversion objects (created once on first AHB).
static VkSamplerYcbcrConversion s_ycbcrConversion = VK_NULL_HANDLE;
static VkSampler s_ycbcrSampler = VK_NULL_HANDLE;
static uint64_t s_externalFormat = 0;

static const int MAX_IN_FLIGHT = 3;

// Compute blit pipeline objects.
static VkDescriptorSetLayout s_descSetLayout = VK_NULL_HANDLE;
static VkPipelineLayout s_pipelineLayout = VK_NULL_HANDLE;
static VkPipeline s_blitPipeline = VK_NULL_HANDLE;
static VkDescriptorPool s_descPool = VK_NULL_HANDLE;
static VkDescriptorSet s_descSets[MAX_IN_FLIGHT] = {};
static VkShaderModule s_shaderModule = VK_NULL_HANDLE;

static std::atomic<bool> s_renderError(false);

static void FlagRenderError(const char* site) {
    LOGE("[ZeroCopy] Renderer entering failed state at %s", site);
    s_renderError.store(true);
}

struct InFlightFrame {
    AImage* image;
    unsigned long long frameNumber;
    bool inUse;
};

static InFlightFrame s_inFlight[MAX_IN_FLIGHT] = {};

// Per-AHardwareBuffer Vulkan import cache. AImageReader maxImages limits how
// many images this consumer may hold concurrently, but MediaCodec/BufferQueue
// can rotate through more backing AHB slots than that. Keep enough headroom for
// the producer pool so the hot path avoids per-frame Vulkan import work.
struct AhbCacheEntry {
    AHardwareBuffer* ahb;
    VkImage image;
    VkDeviceMemory mem;
    VkImageView view;
};

static const int AHB_CACHE_SIZE = 32;
static AhbCacheEntry s_ahbCache[AHB_CACHE_SIZE] = {};

static void VerifyVulkanExtensions(VkPhysicalDevice physDevice) {
    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(physDevice, nullptr, &extCount, nullptr);

    VkExtensionProperties* exts =
        (VkExtensionProperties*)malloc(extCount * sizeof(VkExtensionProperties));
    vkEnumerateDeviceExtensionProperties(physDevice, nullptr, &extCount, exts);

    const char* required[] = {
        "VK_ANDROID_external_memory_android_hardware_buffer",
        "VK_KHR_external_memory",
        "VK_KHR_external_memory_fd",
        "VK_EXT_queue_family_foreign",
        "VK_KHR_sampler_ycbcr_conversion",
        "VK_KHR_dedicated_allocation",
        "VK_KHR_get_memory_requirements2",
        "VK_KHR_bind_memory2",
        "VK_KHR_maintenance1",
        nullptr
    };

    LOGI("=== Zero-Copy Vulkan Extension Check ===");
    for (int r = 0; required[r] != nullptr; r++) {
        bool found = false;
        for (uint32_t e = 0; e < extCount; e++) {
            if (strcmp(required[r], exts[e].extensionName) == 0) {
                found = true;
                break;
            }
        }
        LOGI("  %s: %s", required[r], found ? "SUPPORTED" : "NOT FOUND");
    }

    free(exts);
}

static bool InitYcbcrConversion(VkDevice device, VkPhysicalDevice physDevice, AHardwareBuffer* ahb) {
    (void)physDevice;

    VkAndroidHardwareBufferFormatPropertiesANDROID fmtProps = {};
    fmtProps.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID;

    VkAndroidHardwareBufferPropertiesANDROID ahbProps = {};
    ahbProps.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
    ahbProps.pNext = &fmtProps;

    VkResult res = pfnGetAHBProps(device, ahb, &ahbProps);
    if (res != VK_SUCCESS) {
        LOGE("[ZeroCopy] vkGetAndroidHardwareBufferPropertiesANDROID failed: %d", res);
        return false;
    }

    s_externalFormat = fmtProps.externalFormat;

    VkExternalFormatANDROID extFmt = {};
    extFmt.sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID;
    extFmt.externalFormat = s_externalFormat;

    VkSamplerYcbcrConversionCreateInfo convInfo = {};
    convInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO;
    convInfo.pNext = &extFmt;
    convInfo.format = VK_FORMAT_UNDEFINED;
    convInfo.ycbcrModel = fmtProps.suggestedYcbcrModel;
    convInfo.ycbcrRange = fmtProps.suggestedYcbcrRange;
    convInfo.components = fmtProps.samplerYcbcrConversionComponents;
    convInfo.xChromaOffset = fmtProps.suggestedXChromaOffset;
    convInfo.yChromaOffset = fmtProps.suggestedYChromaOffset;
    convInfo.chromaFilter = VK_FILTER_LINEAR;
    convInfo.forceExplicitReconstruction = VK_FALSE;

    res = vkCreateSamplerYcbcrConversion(device, &convInfo, nullptr, &s_ycbcrConversion);
    if (res != VK_SUCCESS) {
        LOGE("[ZeroCopy] vkCreateSamplerYcbcrConversion failed: %d", res);
        return false;
    }

    VkSamplerYcbcrConversionInfo convInfoRef = {};
    convInfoRef.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
    convInfoRef.conversion = s_ycbcrConversion;

    VkSamplerCreateInfo samplerInfo = {};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.pNext = &convInfoRef;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;

    res = vkCreateSampler(device, &samplerInfo, nullptr, &s_ycbcrSampler);
    if (res != VK_SUCCESS) {
        LOGE("[ZeroCopy] vkCreateSampler (YCbCr) failed: %d", res);
        return false;
    }

    LOGI("[ZeroCopy] YCbCr conversion and sampler created successfully");
    return true;
}

static bool ImportAHBToVkImage(VkDevice device, VkPhysicalDevice physDevice, AHardwareBuffer* ahb,
                               VkImage* outImage, VkDeviceMemory* outMem, VkImageView* outView) {
    (void)physDevice;

    VkAndroidHardwareBufferFormatPropertiesANDROID fmtProps = {};
    fmtProps.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID;

    VkAndroidHardwareBufferPropertiesANDROID ahbProps = {};
    ahbProps.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
    ahbProps.pNext = &fmtProps;

    VkResult ahbRes = pfnGetAHBProps(device, ahb, &ahbProps);
    if (ahbRes != VK_SUCCESS) {
        LOGE("[ZeroCopy] vkGetAndroidHardwareBufferPropertiesANDROID failed in ImportAHBToVkImage: %d",
             ahbRes);
        return false;
    }

    AHardwareBuffer_Desc desc;
    AHardwareBuffer_describe(ahb, &desc);

    VkExternalFormatANDROID extFmt = {};
    extFmt.sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID;
    extFmt.externalFormat = s_externalFormat;

    VkExternalMemoryImageCreateInfo extMemInfo = {};
    extMemInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    extMemInfo.pNext = &extFmt;
    extMemInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;

    VkImageCreateInfo imgInfo = {};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.pNext = &extMemInfo;
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.format = VK_FORMAT_UNDEFINED;
    imgInfo.extent = { desc.width, desc.height, 1 };
    imgInfo.mipLevels = 1;
    imgInfo.arrayLayers = 1;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkResult res = vkCreateImage(device, &imgInfo, nullptr, outImage);
    if (res != VK_SUCCESS) {
        LOGE("[ZeroCopy] vkCreateImage for AHB failed: %d", res);
        return false;
    }

    VkImportAndroidHardwareBufferInfoANDROID importInfo = {};
    importInfo.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
    importInfo.buffer = ahb;

    VkMemoryDedicatedAllocateInfo dedicatedInfo = {};
    dedicatedInfo.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicatedInfo.pNext = &importInfo;
    dedicatedInfo.image = *outImage;

    uint32_t memTypeIndex = 0;
    uint32_t typeBits = ahbProps.memoryTypeBits;
    while (typeBits) {
        if (typeBits & 1) break;
        memTypeIndex++;
        typeBits >>= 1;
    }

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.pNext = &dedicatedInfo;
    allocInfo.allocationSize = ahbProps.allocationSize;
    allocInfo.memoryTypeIndex = memTypeIndex;

    res = vkAllocateMemory(device, &allocInfo, nullptr, outMem);
    if (res != VK_SUCCESS) {
        LOGE("[ZeroCopy] vkAllocateMemory for AHB failed: %d", res);
        vkDestroyImage(device, *outImage, nullptr);
        *outImage = VK_NULL_HANDLE;
        return false;
    }

    res = vkBindImageMemory(device, *outImage, *outMem, 0);
    if (res != VK_SUCCESS) {
        LOGE("[ZeroCopy] vkBindImageMemory for AHB failed: %d", res);
        vkFreeMemory(device, *outMem, nullptr);
        vkDestroyImage(device, *outImage, nullptr);
        *outImage = VK_NULL_HANDLE;
        *outMem = VK_NULL_HANDLE;
        return false;
    }

    VkSamplerYcbcrConversionInfo convInfoRef = {};
    convInfoRef.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
    convInfoRef.conversion = s_ycbcrConversion;

    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.pNext = &convInfoRef;
    viewInfo.image = *outImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_UNDEFINED;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;

    res = vkCreateImageView(device, &viewInfo, nullptr, outView);
    if (res != VK_SUCCESS) {
        LOGE("[ZeroCopy] vkCreateImageView for AHB failed: %d", res);
        vkFreeMemory(device, *outMem, nullptr);
        vkDestroyImage(device, *outImage, nullptr);
        *outImage = VK_NULL_HANDLE;
        *outMem = VK_NULL_HANDLE;
        return false;
    }

    return true;
}

static void RetireSlot(VkDevice device, InFlightFrame& slot) {
    (void)device;
    if (slot.image) {
        AImage_delete(slot.image);
    }
    slot = {};
}

static void RetireCompletedFrames(VkDevice device, unsigned long long safeFrameNumber) {
    for (int i = 0; i < MAX_IN_FLIGHT; i++) {
        if (s_inFlight[i].inUse && safeFrameNumber >= s_inFlight[i].frameNumber) {
            RetireSlot(device, s_inFlight[i]);
        }
    }
}

static int AllocateInFlightSlot() {
    for (int i = 0; i < MAX_IN_FLIGHT; i++) {
        if (!s_inFlight[i].inUse) return i;
    }

    static uint64_t dropCount = 0;
    if (dropCount++ % 60 == 0) {
        LOGI("[ZeroCopy] All in-flight slots still busy; dropped %llu newest frames",
             (unsigned long long)dropCount);
    }
    return -1;
}

static void RetireAllInFlightFrames(VkDevice device) {
    for (int i = 0; i < MAX_IN_FLIGHT; i++) {
        if (s_inFlight[i].inUse) {
            RetireSlot(device, s_inFlight[i]);
        }
    }
}

static const AhbCacheEntry* LookupAhbCache(AHardwareBuffer* ahb) {
    for (int i = 0; i < AHB_CACHE_SIZE; i++) {
        if (s_ahbCache[i].ahb == ahb) {
            return &s_ahbCache[i];
        }
    }
    return nullptr;
}

static const AhbCacheEntry* LookupOrImportAhb(VkDevice device,
                                              VkPhysicalDevice physDevice,
                                              AHardwareBuffer* ahb) {
    const AhbCacheEntry* hit = LookupAhbCache(ahb);
    if (hit) {
        return hit;
    }

    for (int i = 0; i < AHB_CACHE_SIZE; i++) {
        if (s_ahbCache[i].ahb != nullptr) continue;

        VkImage img = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        if (!ImportAHBToVkImage(device, physDevice, ahb, &img, &mem, &view)) {
            return nullptr;
        }

        s_ahbCache[i].ahb = ahb;
        s_ahbCache[i].image = img;
        s_ahbCache[i].mem = mem;
        s_ahbCache[i].view = view;
        LOGI("[ZeroCopy] AHB cache miss, imported to slot %d (ahb=%p)", i, ahb);
        return &s_ahbCache[i];
    }

    LOGE("[ZeroCopy] AHB cache full (size %d) — pool churn higher than expected",
         AHB_CACHE_SIZE);
    return nullptr;
}

static void DestroyAhbCache(VkDevice device) {
    for (int i = 0; i < AHB_CACHE_SIZE; i++) {
        AhbCacheEntry& e = s_ahbCache[i];
        if (e.view != VK_NULL_HANDLE) {
            vkDestroyImageView(device, e.view, nullptr);
        }
        if (e.mem != VK_NULL_HANDLE) {
            vkFreeMemory(device, e.mem, nullptr);
        }
        if (e.image != VK_NULL_HANDLE) {
            vkDestroyImage(device, e.image, nullptr);
        }
        e = {};
    }
}

static bool CreateBlitPipeline(VkDevice device) {
    VkDescriptorSetLayoutBinding bindings[2] = {};

    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[0].pImmutableSamplers = &s_ycbcrSampler;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;

    VkResult res = vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &s_descSetLayout);
    if (res != VK_SUCCESS) {
        LOGE("[ZeroCopy] vkCreateDescriptorSetLayout failed: %d", res);
        return false;
    }

    VkPushConstantRange pushRange = {};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = 8;

    VkPipelineLayoutCreateInfo plInfo = {};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &s_descSetLayout;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges = &pushRange;

    res = vkCreatePipelineLayout(device, &plInfo, nullptr, &s_pipelineLayout);
    if (res != VK_SUCCESS) {
        LOGE("[ZeroCopy] vkCreatePipelineLayout failed: %d", res);
        return false;
    }

    VkShaderModuleCreateInfo smInfo = {};
    smInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = ycbcr_blit_spv_len;
    smInfo.pCode = (const uint32_t*)ycbcr_blit_spv;

    res = vkCreateShaderModule(device, &smInfo, nullptr, &s_shaderModule);
    if (res != VK_SUCCESS) {
        LOGE("[ZeroCopy] vkCreateShaderModule failed: %d", res);
        return false;
    }

    VkComputePipelineCreateInfo cpInfo = {};
    cpInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpInfo.stage.module = s_shaderModule;
    cpInfo.stage.pName = "main";
    cpInfo.layout = s_pipelineLayout;

    res = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpInfo, nullptr, &s_blitPipeline);
    if (res != VK_SUCCESS) {
        LOGE("[ZeroCopy] vkCreateComputePipelines failed: %d", res);
        return false;
    }

    VkDescriptorPoolSize poolSizes[2] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = MAX_IN_FLIGHT;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = MAX_IN_FLIGHT;

    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = MAX_IN_FLIGHT;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;

    res = vkCreateDescriptorPool(device, &poolInfo, nullptr, &s_descPool);
    if (res != VK_SUCCESS) {
        LOGE("[ZeroCopy] vkCreateDescriptorPool failed: %d", res);
        return false;
    }

    VkDescriptorSetLayout layouts[MAX_IN_FLIGHT] = {};
    for (int i = 0; i < MAX_IN_FLIGHT; i++) {
        layouts[i] = s_descSetLayout;
    }

    VkDescriptorSetAllocateInfo dsAllocInfo = {};
    dsAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAllocInfo.descriptorPool = s_descPool;
    dsAllocInfo.descriptorSetCount = MAX_IN_FLIGHT;
    dsAllocInfo.pSetLayouts = layouts;

    res = vkAllocateDescriptorSets(device, &dsAllocInfo, s_descSets);
    if (res != VK_SUCCESS) {
        LOGE("[ZeroCopy] vkAllocateDescriptorSets failed: %d", res);
        return false;
    }

    LOGI("[ZeroCopy] Compute blit pipeline created successfully");
    return true;
}

static void CleanupPipelineResources(VkDevice device) {
    if (device == VK_NULL_HANDLE) return;

    // Wait for any in-flight GPU work that may still reference cached AHB
    // imports, retained AImages, descriptor sets, or the Unity texture view
    // before tearing them down. One-off cost on stream stop.
    vkDeviceWaitIdle(device);

    RetireAllInFlightFrames(device);
    DestroyAhbCache(device);

    if (s_unityImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(device, s_unityImageView, nullptr);
        s_unityImageView = VK_NULL_HANDLE;
    }
    s_unityImage = VK_NULL_HANDLE;

    if (s_descPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, s_descPool, nullptr);
        s_descPool = VK_NULL_HANDLE;
        memset(s_descSets, 0, sizeof(s_descSets));
    }
    if (s_blitPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, s_blitPipeline, nullptr);
        s_blitPipeline = VK_NULL_HANDLE;
    }
    if (s_shaderModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, s_shaderModule, nullptr);
        s_shaderModule = VK_NULL_HANDLE;
    }
    if (s_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, s_pipelineLayout, nullptr);
        s_pipelineLayout = VK_NULL_HANDLE;
    }
    if (s_descSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, s_descSetLayout, nullptr);
        s_descSetLayout = VK_NULL_HANDLE;
    }
    if (s_ycbcrSampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, s_ycbcrSampler, nullptr);
        s_ycbcrSampler = VK_NULL_HANDLE;
    }
    if (s_ycbcrConversion != VK_NULL_HANDLE) {
        vkDestroySamplerYcbcrConversion(device, s_ycbcrConversion, nullptr);
        s_ycbcrConversion = VK_NULL_HANDLE;
    }
}

void VulkanRenderer_Load(IUnityInterfaces* unityInterfaces) {
    IUnityGraphics* graphics = unityInterfaces ? unityInterfaces->Get<IUnityGraphics>() : nullptr;
    s_unityVulkan = unityInterfaces ? unityInterfaces->Get<IUnityGraphicsVulkan>() : nullptr;

    if (graphics) {
        LOGI("Graphics API: %d (Vulkan=%d)", graphics->GetRenderer(), kUnityGfxRendererVulkan);
    }

    if (s_unityVulkan) {
        LOGI("Vulkan interface available");

        UnityVulkanInstance vulkanInstance = s_unityVulkan->Instance();
        VerifyVulkanExtensions(vulkanInstance.physicalDevice);

        pfnGetAHBProps = (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)
            vkGetDeviceProcAddr(vulkanInstance.device, "vkGetAndroidHardwareBufferPropertiesANDROID");
        if (pfnGetAHBProps) {
            LOGI("[ZeroCopy] vkGetAndroidHardwareBufferPropertiesANDROID loaded");
        } else {
            LOGE("[ZeroCopy] FAILED to load vkGetAndroidHardwareBufferPropertiesANDROID — extension not enabled on device?");
        }
    } else {
        LOGE("Vulkan interface NOT available");
    }
}

void VulkanRenderer_Unload(void) {
    VulkanRenderer_Cleanup();
    s_textureHandle = nullptr;
    s_unityVulkan = nullptr;
    pfnGetAHBProps = nullptr;
    s_unityImage = VK_NULL_HANDLE;
}

void VulkanRenderer_SetTexture(void* rgbaTexture, int width, int height) {
    s_textureHandle = rgbaTexture;
    s_unityImage = VK_NULL_HANDLE;
    LOGI("RGBA texture set: %p (%dx%d)", rgbaTexture, width, height);
}

void VulkanRenderer_Render(AImageReader* reader) {
    // Transient: Unity hasn't finished wiring the texture yet, or the decoder's
    // surface isn't up. Both resolve themselves; keep returning silently.
    if (!reader || !s_textureHandle) return;

    // Persistent: Unity didn't hand us a Vulkan interface, or the AHB import
    // entry point never loaded. Neither will become available later, so the
    // stream can never produce frames — surface this as a renderer failure.
    if (!s_unityVulkan || !pfnGetAHBProps) {
        FlagRenderError("missing Vulkan/AHB infrastructure");
        return;
    }

    UnityVulkanInstance vi = s_unityVulkan->Instance();
    VkDevice device = vi.device;

    UnityVulkanRecordingState rs;
    if (s_unityVulkan->CommandRecordingState(&rs, kUnityVulkanGraphicsQueueAccess_DontCare)) {
        RetireCompletedFrames(device, rs.safeFrameNumber);
    }

    AImage* image = nullptr;
    media_status_t sts = AImageReader_acquireLatestImage(reader, &image);
    if (sts != AMEDIA_OK || !image) {
        return;
    }

    AHardwareBuffer* ahb = nullptr;
    AImage_getHardwareBuffer(image, &ahb);
    if (!ahb) {
        AImage_delete(image);
        return;
    }

    s_unityVulkan->EnsureOutsideRenderPass();

    if (s_ycbcrConversion == VK_NULL_HANDLE) {
        if (!InitYcbcrConversion(device, vi.physicalDevice, ahb)) {
            FlagRenderError("InitYcbcrConversion");
            AImage_delete(image);
            return;
        }
        if (!CreateBlitPipeline(device)) {
            FlagRenderError("CreateBlitPipeline");
            AImage_delete(image);
            return;
        }
    }

    int slot = AllocateInFlightSlot();
    if (slot < 0) {
        AImage_delete(image);
        return;
    }

    const AhbCacheEntry* cached = LookupOrImportAhb(device, vi.physicalDevice, ahb);
    if (!cached) {
        FlagRenderError("LookupOrImportAhb");
        AImage_delete(image);
        return;
    }

    UnityVulkanImage unityImg;
    if (!s_unityVulkan->AccessTexture(
            s_textureHandle,
            UnityVulkanWholeImage,
            VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_WRITE_BIT,
            kUnityVulkanResourceAccess_PipelineBarrier,
            &unityImg)) {
        FlagRenderError("AccessTexture");
        AImage_delete(image);
        return;
    }

    if (s_unityImageView == VK_NULL_HANDLE || s_unityImage != unityImg.image) {
        if (s_unityImageView != VK_NULL_HANDLE) {
            vkDestroyImageView(device, s_unityImageView, nullptr);
            s_unityImageView = VK_NULL_HANDLE;
        }
        s_unityImage = VK_NULL_HANDLE;

        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = unityImg.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = unityImg.format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        VkResult viewRes = vkCreateImageView(device, &viewInfo, nullptr, &s_unityImageView);
        if (viewRes != VK_SUCCESS) {
            LOGE("[ZeroCopy] vkCreateImageView for Unity texture failed: %d", viewRes);
            FlagRenderError("vkCreateImageView (Unity texture)");
            s_unityImageView = VK_NULL_HANDLE;
            AImage_delete(image);
            return;
        }
        s_unityImage = unityImg.image;
    }

    VkDescriptorImageInfo srcInfo = {};
    srcInfo.sampler = s_ycbcrSampler;
    srcInfo.imageView = cached->view;
    srcInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo dstInfo = {};
    dstInfo.imageView = s_unityImageView;
    dstInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet writes[2] = {};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = s_descSets[slot];
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &srcInfo;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = s_descSets[slot];
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo = &dstInfo;

    vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

    if (!s_unityVulkan->CommandRecordingState(&rs, kUnityVulkanGraphicsQueueAccess_DontCare)) {
        FlagRenderError("CommandRecordingState");
        AImage_delete(image);
        return;
    }

    VkCommandBuffer cmd = rs.commandBuffer;

    VkImageMemoryBarrier ahbBarrier = {};
    ahbBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    ahbBarrier.srcAccessMask = 0;
    ahbBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    ahbBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ahbBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ahbBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    ahbBarrier.dstQueueFamilyIndex = vi.queueFamilyIndex;
    ahbBarrier.image = cached->image;
    ahbBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ahbBarrier.subresourceRange.levelCount = 1;
    ahbBarrier.subresourceRange.layerCount = 1;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &ahbBarrier);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_blitPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        s_pipelineLayout, 0, 1, &s_descSets[slot], 0, nullptr);

    int32_t size[2] = { (int32_t)unityImg.extent.width, (int32_t)unityImg.extent.height };
    vkCmdPushConstants(cmd, s_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, size);

    uint32_t groupsX = (unityImg.extent.width + 15) / 16;
    uint32_t groupsY = (unityImg.extent.height + 15) / 16;
    vkCmdDispatch(cmd, groupsX, groupsY, 1);

    s_inFlight[slot].image = image;
    s_inFlight[slot].frameNumber = rs.currentFrameNumber;
    s_inFlight[slot].inUse = true;
}

void VulkanRenderer_RetireAll(void) {
    if (!s_unityVulkan) return;

    UnityVulkanInstance vi = s_unityVulkan->Instance();
    if (vi.device == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(vi.device);
    RetireAllInFlightFrames(vi.device);
}

void VulkanRenderer_Cleanup(void) {
    s_renderError.store(false);

    if (!s_unityVulkan) {
        s_unityImage = VK_NULL_HANDLE;
        return;
    }

    UnityVulkanInstance vi = s_unityVulkan->Instance();
    CleanupPipelineResources(vi.device);
}

bool VulkanRenderer_ConsumeError(void) {
    return s_renderError.exchange(false);
}
