#include "gst_debug_helpers.h"
#include <gst/gst.h>
#include <android/log.h>

#define LOG_TAG "GstUnityPlugin"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern "C" void GstUnityPlugin_ListDecoders() {
    GList *factories = gst_element_factory_list_get_elements(
        GST_ELEMENT_FACTORY_TYPE_DECODER | GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO,
        GST_RANK_MARGINAL
    );

    LOGI("=== Available Video Decoders ===");

    for (GList *iter = factories; iter != NULL; iter = iter->next) {
        GstElementFactory *factory = GST_ELEMENT_FACTORY(iter->data);
        const gchar *name = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory));
        const gchar *klass = gst_element_factory_get_metadata(factory, GST_ELEMENT_METADATA_KLASS);

        LOGI("  %s (%s)", name, klass);
    }

    gst_plugin_feature_list_free(factories);
}

extern "C" void GstUnityPlugin_ListPlugins() {
    GList *plugins = gst_registry_get_plugin_list(gst_registry_get());

    LOGI("=== Loaded Plugins ===");

    for (GList *iter = plugins; iter != NULL; iter = iter->next) {
        GstPlugin *plugin = GST_PLUGIN(iter->data);
        const gchar *name = gst_plugin_get_name(plugin);
        LOGI("  %s", name);
    }

    gst_plugin_list_free(plugins);
}

extern "C" void GstUnityPlugin_InspectDecoder(const char* decoderName) {
    GstElementFactory *factory = gst_element_factory_find(decoderName);
    if (!factory) {
        LOGE("Decoder not found: %s", decoderName);
        return;
    }
    LOGI("=== INSPECTING: %s ===", decoderName);
    const GList *templates = gst_element_factory_get_static_pad_templates(factory);
    while (templates) {
        GstStaticPadTemplate *templ = (GstStaticPadTemplate *)templates->data;
        LOGI("  PAD: %s (%s)", templ->name_template,
             (templ->direction == GST_PAD_SRC) ? "OUTPUT" : "INPUT");
        GstCaps *caps = gst_static_caps_get(&templ->static_caps);
        gchar *caps_str = gst_caps_to_string(caps);

        LOGI("    CAPS: %s", caps_str);

        g_free(caps_str);
        gst_caps_unref(caps);
        templates = g_list_next(templates);
    }
    gst_object_unref(factory);
}
