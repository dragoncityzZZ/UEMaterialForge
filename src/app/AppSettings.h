#pragma once

#include <filesystem>
#include <string>

namespace mf {

struct AppSettings {
    std::string language;      // "en" / "zh-CN"; empty means system default.
    float fileBarHeight = 292.0f;
    std::string theme = "graphite";  // "graphite" / "midnight"
    float uiScale = 0.0f;            // 0 = automatic from monitor DPI/resolution.
    std::string renderApi = "opengl";  // "opengl" / "vulkan" / "dx12".
    bool nodeLibraryCollapsed = false;
    bool consoleVisible = false;
    float consoleHeight = 190.0f;
    bool previewDetached = false;
    bool previewLive = true;
    float previewWindowX = 0.0f;
    float previewWindowY = 0.0f;
    float previewWindowWidth = 560.0f;
    float previewWindowHeight = 430.0f;
    bool autosaveEnabled = true;
    int autosaveIntervalSeconds = 4;
    std::string previewMeshMode = "Sphere";  // Sphere / Plane / Custom
    std::string previewMeshPath;
    std::string previewHdriPath;
    bool previewGroundEnabled = false;
    float previewGroundHeight = -1.05f;
    float previewKeyIntensity = 1.0f;
    float previewFillIntensity = 1.0f;
    float previewRimIntensity = 1.0f;
    float previewAmbientIntensity = 1.0f;
    float previewExposure = 1.0f;
    bool mcpEnabled = false;
    int mcpPort = 3011;
};

class AppSettingsStore {
public:
    explicit AppSettingsStore(std::filesystem::path path);

    bool Load(AppSettings& settings, std::string* error = nullptr) const;
    bool Save(const AppSettings& settings, std::string* error = nullptr) const;

private:
    std::filesystem::path m_Path;
};

} // namespace mf
