#include "app/AppSettings.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

namespace mf {
namespace {

std::string ReadFile(const fs::path& path, std::string* error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        if (error) *error = "Could not open settings file.";
        return {};
    }
    std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (error) error->clear();
    return text;
}

bool WriteFileAtomically(const fs::path& path, const std::string& text, std::string* error) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        if (error) *error = "Could not create settings directory: " + ec.message();
        return false;
    }
    fs::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            if (error) *error = "Could not write settings file.";
            return false;
        }
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!stream) {
            if (error) *error = "Failed while writing settings file.";
            return false;
        }
    }
    fs::rename(temporary, path, ec);
    if (ec) {
        std::error_code removeError;
        fs::remove(path, removeError);
        ec.clear();
        fs::rename(temporary, path, ec);
    }
    if (ec) {
        fs::remove(temporary, ec);
        if (error) *error = "Could not finalize settings file.";
        return false;
    }
    if (error) error->clear();
    return true;
}

} // namespace

AppSettingsStore::AppSettingsStore(std::filesystem::path path)
    : m_Path(std::move(path)) {}

bool AppSettingsStore::Load(AppSettings& settings, std::string* error) const {
    std::error_code ec;
    if (!fs::is_regular_file(m_Path, ec)) {
        if (error) error->clear();
        return true;
    }
    std::string readError;
    std::string contents = ReadFile(m_Path, &readError);
    if (contents.empty() && !readError.empty()) {
        if (error) *error = readError;
        return false;
    }
    try {
        const nlohmann::json document = nlohmann::json::parse(contents);
        if (!document.is_object()) {
            if (error) *error = "Settings file root must be an object.";
            return false;
        }
        if (document.contains("language") && document["language"].is_string()) {
            settings.language = document["language"].get<std::string>();
        }
        if (document.contains("fileBarHeight") && document["fileBarHeight"].is_number()) {
            settings.fileBarHeight = document["fileBarHeight"].get<float>();
        }
        if (document.contains("theme") && document["theme"].is_string()) {
            settings.theme = document["theme"].get<std::string>();
        }
        if (document.contains("uiScale") && document["uiScale"].is_number()) {
            settings.uiScale = document["uiScale"].get<float>();
        }
        if (document.contains("renderApi") && document["renderApi"].is_string()) {
            settings.renderApi = document["renderApi"].get<std::string>();
        }
        if (document.contains("nodeLibraryCollapsed") &&
            document["nodeLibraryCollapsed"].is_boolean()) {
            settings.nodeLibraryCollapsed = document["nodeLibraryCollapsed"].get<bool>();
        }
        if (document.contains("consoleVisible") && document["consoleVisible"].is_boolean()) {
            settings.consoleVisible = document["consoleVisible"].get<bool>();
        }
        if (document.contains("consoleHeight") && document["consoleHeight"].is_number()) {
            settings.consoleHeight = document["consoleHeight"].get<float>();
        }
        if (document.contains("previewDetached") && document["previewDetached"].is_boolean()) {
            settings.previewDetached = document["previewDetached"].get<bool>();
        }
        if (document.contains("previewLive") && document["previewLive"].is_boolean()) {
            settings.previewLive = document["previewLive"].get<bool>();
        }
        if (document.contains("previewWindowX") && document["previewWindowX"].is_number()) {
            settings.previewWindowX = document["previewWindowX"].get<float>();
        }
        if (document.contains("previewWindowY") && document["previewWindowY"].is_number()) {
            settings.previewWindowY = document["previewWindowY"].get<float>();
        }
        if (document.contains("previewWindowWidth") && document["previewWindowWidth"].is_number()) {
            settings.previewWindowWidth = document["previewWindowWidth"].get<float>();
        }
        if (document.contains("previewWindowHeight") && document["previewWindowHeight"].is_number()) {
            settings.previewWindowHeight = document["previewWindowHeight"].get<float>();
        }
        if (document.contains("autosaveEnabled") && document["autosaveEnabled"].is_boolean()) {
            settings.autosaveEnabled = document["autosaveEnabled"].get<bool>();
        }
        if (document.contains("autosaveIntervalSeconds") &&
            document["autosaveIntervalSeconds"].is_number_integer()) {
            settings.autosaveIntervalSeconds = std::clamp(
                document["autosaveIntervalSeconds"].get<int>(), 2, 120);
        }
        if (document.contains("previewMeshMode") && document["previewMeshMode"].is_string()) {
            settings.previewMeshMode = document["previewMeshMode"].get<std::string>();
        }
        if (document.contains("previewMeshPath") && document["previewMeshPath"].is_string()) {
            settings.previewMeshPath = document["previewMeshPath"].get<std::string>();
        }
        if (document.contains("previewHdriPath") && document["previewHdriPath"].is_string()) {
            settings.previewHdriPath = document["previewHdriPath"].get<std::string>();
        }
        if (document.contains("previewGroundEnabled") &&
            document["previewGroundEnabled"].is_boolean()) {
            settings.previewGroundEnabled = document["previewGroundEnabled"].get<bool>();
        }
        if (document.contains("previewGroundHeight") &&
            document["previewGroundHeight"].is_number()) {
            settings.previewGroundHeight = std::clamp(
                document["previewGroundHeight"].get<float>(), -3.0f, 0.0f);
        }
        const auto readLight = [&](const char* key, float& value) {
            if (document.contains(key) && document[key].is_number()) {
                value = std::clamp(document[key].get<float>(), 0.0f, 4.0f);
            }
        };
        readLight("previewKeyIntensity", settings.previewKeyIntensity);
        readLight("previewFillIntensity", settings.previewFillIntensity);
        readLight("previewRimIntensity", settings.previewRimIntensity);
        readLight("previewAmbientIntensity", settings.previewAmbientIntensity);
        if (document.contains("previewExposure") &&
            document["previewExposure"].is_number()) {
            settings.previewExposure = std::clamp(
                document["previewExposure"].get<float>(), 0.1f, 4.0f);
        }
        if (document.contains("mcpEnabled") && document["mcpEnabled"].is_boolean()) {
            settings.mcpEnabled = document["mcpEnabled"].get<bool>();
        }
        if (document.contains("mcpPort") && document["mcpPort"].is_number_integer()) {
            settings.mcpPort = std::clamp(document["mcpPort"].get<int>(), 1024, 65535);
        }
    } catch (const std::exception& exception) {
        if (error) *error = std::string("Could not parse settings: ") + exception.what();
        return false;
    }
    if (error) error->clear();
    return true;
}

bool AppSettingsStore::Save(const AppSettings& settings, std::string* error) const {
    nlohmann::json document;
    document["language"] = settings.language;
    document["fileBarHeight"] = std::clamp(settings.fileBarHeight, 180.0f, 420.0f);
    document["theme"] = settings.theme;
    document["uiScale"] = settings.uiScale;
    document["renderApi"] = settings.renderApi;
    document["nodeLibraryCollapsed"] = settings.nodeLibraryCollapsed;
    document["consoleVisible"] = settings.consoleVisible;
    document["consoleHeight"] = std::clamp(settings.consoleHeight, 110.0f, 520.0f);
    document["previewDetached"] = settings.previewDetached;
    document["previewLive"] = settings.previewLive;
    document["previewWindowX"] = settings.previewWindowX;
    document["previewWindowY"] = settings.previewWindowY;
    document["previewWindowWidth"] = std::clamp(settings.previewWindowWidth, 320.0f, 1600.0f);
    document["previewWindowHeight"] = std::clamp(settings.previewWindowHeight, 260.0f, 1200.0f);
    document["autosaveEnabled"] = settings.autosaveEnabled;
    document["autosaveIntervalSeconds"] = std::clamp(settings.autosaveIntervalSeconds, 2, 120);
    document["previewMeshMode"] = settings.previewMeshMode;
    document["previewMeshPath"] = settings.previewMeshPath;
    document["previewHdriPath"] = settings.previewHdriPath;
    document["previewGroundEnabled"] = settings.previewGroundEnabled;
    document["previewGroundHeight"] = std::clamp(settings.previewGroundHeight, -3.0f, 0.0f);
    document["previewKeyIntensity"] =
        std::clamp(settings.previewKeyIntensity, 0.0f, 4.0f);
    document["previewFillIntensity"] =
        std::clamp(settings.previewFillIntensity, 0.0f, 4.0f);
    document["previewRimIntensity"] =
        std::clamp(settings.previewRimIntensity, 0.0f, 4.0f);
    document["previewAmbientIntensity"] =
        std::clamp(settings.previewAmbientIntensity, 0.0f, 4.0f);
    document["previewExposure"] = std::clamp(settings.previewExposure, 0.1f, 4.0f);
    document["mcpEnabled"] = settings.mcpEnabled;
    document["mcpPort"] = std::clamp(settings.mcpPort, 1024, 65535);
    if (error) error->clear();
    return WriteFileAtomically(m_Path, document.dump(2) + "\n", error);
}

} // namespace mf
