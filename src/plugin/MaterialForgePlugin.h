#pragma once

// Public MaterialForge plugin ABI. A plugin is a shared library that exports
// MaterialForgePluginInit and optionally MaterialForgePluginShutdown.
//
// Build the DLL, create a plugin.json next to it, and use
// Edit -> Plugin Manager -> Add Plugin... to install the folder.

#include <stdint.h>

#define MF_PLUGIN_API_VERSION 1u

#ifdef _WIN32
#define MF_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#define MF_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

typedef struct MfPluginApi MfPluginApi;
typedef void (*MfPluginCommandFn)(const char* args, void* user);

struct MfPluginApi {
    uint32_t api_version;
    void* host;
    void (*log)(void* host, int level, const char* message);
    const char* (*runtime_version)(void* host);
    int (*register_command)(void* host, const char* name, const char* description,
                            MfPluginCommandFn callback, void* user);
    // Optional visual customization API (fields are appended for backward
    // compatibility with plugins built against the original ABI).
    // Colors use 0xAABBGGRR (ImU32-format, little-endian RGBA).
    // style keys: text, text_disabled, window_bg, child_bg, popup_bg, border,
    // frame_bg, button, button_hovered, button_active, header, header_hovered,
    // tab, tab_selected, accent.
    // Node color key may be a node type or a category; type wins.
    int (*set_theme)(void* host, const char* name);
    int (*set_style_color)(void* host, const char* key, uint32_t rgba);
    int (*clear_style_color)(void* host, const char* key);
    int (*set_node_color)(void* host, const char* key, uint32_t rgba);
    int (*clear_node_color)(void* host, const char* key);
    // shape: 0 = curved, 1 = straight. Thickness is clamped to [0.5, 12].
    int (*set_link_style)(void* host, uint32_t rgba, float thickness, int shape);
    int (*clear_link_style)(void* host);
    int (*set_ui_scale)(void* host, float scale);
};

MF_PLUGIN_EXPORT int MaterialForgePluginInit(const MfPluginApi* api);
MF_PLUGIN_EXPORT void MaterialForgePluginShutdown(void);
