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
};

MF_PLUGIN_EXPORT int MaterialForgePluginInit(const MfPluginApi* api);
MF_PLUGIN_EXPORT void MaterialForgePluginShutdown(void);
