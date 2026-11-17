#include "app/AppSettings.h"

#include <filesystem>
#include <iostream>
#include <string>

namespace {

int g_Failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++g_Failures;
    }
}

} // namespace

int main() {
    namespace fs = std::filesystem;
    const fs::path directory =
        fs::current_path() / "build" / "test-scratch" / "settings";
    fs::remove_all(directory);
    fs::create_directories(directory);
    const fs::path path = directory / "settings.json";

    mf::AppSettings settings;
    settings.language = "zh-CN";
    settings.fileBarHeight = 310.0f;
    settings.theme = "midnight";
    settings.uiScale = 1.25f;
    settings.renderApi = "vulkan";
    settings.nodeLibraryCollapsed = true;
    settings.consoleVisible = true;
    settings.consoleHeight = 240.0f;
    settings.previewDetached = true;
    settings.previewLive = false;
    settings.previewWindowX = 120.0f;
    settings.previewWindowY = 80.0f;
    settings.previewWindowWidth = 640.0f;
    settings.previewWindowHeight = 480.0f;
    settings.autosaveEnabled = false;
    settings.autosaveIntervalSeconds = 12;
    settings.previewMeshMode = "Custom";
    settings.previewMeshPath = "assets/models/cube.obj";
    settings.previewHdriPath = "assets/hdri/studio.hdr";
    settings.previewGroundEnabled = true;
    settings.previewGroundHeight = -1.2f;
    settings.previewKeyIntensity = 1.4f;
    settings.previewFillIntensity = 0.7f;
    settings.previewRimIntensity = 1.2f;
    settings.previewAmbientIntensity = 0.8f;
    settings.previewExposure = 1.35f;
    settings.mcpEnabled = true;
    settings.mcpPort = 3210;

    mf::AppSettingsStore store(path);
    std::string error;
    Check(store.Save(settings, &error), "save settings: " + error);

    mf::AppSettings loaded;
    Check(store.Load(loaded, &error), "load settings: " + error);
    Check(loaded.language == "zh-CN", "language round-trip");
    Check(loaded.theme == "midnight", "theme round-trip");
    Check(loaded.renderApi == "vulkan", "renderApi round-trip");
    Check(loaded.nodeLibraryCollapsed, "nodeLibraryCollapsed round-trip");
    Check(loaded.consoleVisible, "consoleVisible round-trip");
    Check(loaded.previewDetached, "previewDetached round-trip");
    Check(!loaded.previewLive, "previewLive round-trip");
    Check(loaded.consoleHeight > 239.0f && loaded.consoleHeight < 241.0f,
          "consoleHeight round-trip");
    Check(loaded.previewWindowX > 119.0f && loaded.previewWindowX < 121.0f,
          "previewWindowX round-trip");
    Check(!loaded.autosaveEnabled, "autosaveEnabled round-trip");
    Check(loaded.autosaveIntervalSeconds == 12, "autosaveIntervalSeconds round-trip");
    Check(loaded.previewMeshMode == "Custom", "previewMeshMode round-trip");
    Check(loaded.previewMeshPath == "assets/models/cube.obj",
          "previewMeshPath round-trip");
    Check(loaded.previewHdriPath == "assets/hdri/studio.hdr",
          "previewHdriPath round-trip");
    Check(loaded.previewGroundEnabled, "previewGroundEnabled round-trip");
    Check(loaded.previewGroundHeight < -1.19f && loaded.previewGroundHeight > -1.21f,
          "previewGroundHeight round-trip");
    Check(loaded.previewKeyIntensity > 1.39f && loaded.previewKeyIntensity < 1.41f,
          "previewKeyIntensity round-trip");
    Check(loaded.previewExposure > 1.34f && loaded.previewExposure < 1.36f,
          "previewExposure round-trip");
    Check(loaded.mcpEnabled, "mcpEnabled round-trip");
    Check(loaded.mcpPort == 3210, "mcpPort round-trip");

    fs::remove_all(directory);
    if (g_Failures == 0) {
        std::cout << "App settings tests passed.\n";
        return 0;
    }
    std::cerr << g_Failures << " app settings test(s) failed.\n";
    return 1;
}
