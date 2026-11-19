#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace mf {

struct PluginCommandInfo {
    std::string pluginId;
    std::string name;
    std::string description;
    void* user = nullptr;
    void (*callback)(const char* args, void* user) = nullptr;
};

struct PluginInfo {
    std::string id;
    std::string name;
    std::string version;
    std::string author;
    std::string description;
    std::string entry;
    std::filesystem::path folder;
    bool enabled = true;
    bool loaded = false;
    std::string error;
    std::string warning;
    std::vector<std::string> commands;
};

// Discovers and loads MaterialForge DLL plugins from a plugin directory. The
// public ABI lives in plugin/MaterialForgePlugin.h. Plugins can register console
// commands during MaterialForgePluginInit.
class PluginManager {
public:
    using LogFn = std::function<void(int level, const std::string& message)>;

    PluginManager();
    ~PluginManager();

    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    void SetLogHandler(LogFn handler);
    const std::filesystem::path& RootPath() const { return m_RootPath; }
    void SetRootPath(std::filesystem::path path);

    void Refresh();
    void UnloadAll();
    std::vector<PluginInfo> Plugins() const;
    const std::vector<PluginCommandInfo>& Commands() const { return m_Commands; }

    bool Install(const std::filesystem::path& sourceDirectory, std::string* error = nullptr);
    bool Uninstall(const std::string& id, std::string* error = nullptr);
    bool SetEnabled(const std::string& id, bool enabled, std::string* error = nullptr);
    bool Reload(const std::string& id, std::string* error = nullptr);
    bool ExecuteCommand(const std::string& name, const std::string& args);

    static bool IsValidPluginId(const std::string& id, std::string* error = nullptr);

private:
    struct HostContext {
        PluginManager* manager = nullptr;
        std::string pluginId;
        std::string lastError;
    };

    struct LoadedPlugin {
        PluginInfo info;
        void* library = nullptr;
        void* hostContext = nullptr;
        void* apiBlock = nullptr;
        void (*shutdown)() = nullptr;
    };

    static void HostLog(void* host, int level, const char* message);
    static const char* HostRuntimeVersion(void* host);
    static int HostRegisterCommand(void* host, const char* name, const char* description,
                                   void (*callback)(const char*, void*), void* user);
    bool RegisterCommand(const std::string& pluginId, const std::string& name,
                         const std::string& description, void (*callback)(const char*, void*),
                         void* user);
    void LoadPlugin(LoadedPlugin& plugin);
    void UnloadPlugin(LoadedPlugin& plugin);
    bool ReadManifest(const std::filesystem::path& folder, PluginInfo* info,
                      std::string* error) const;
    bool WriteManifestEnabled(const PluginInfo& info, bool enabled, std::string* error);
    void MakeRoot();

    std::filesystem::path m_RootPath;
    std::vector<LoadedPlugin> m_Plugins;
    std::vector<PluginCommandInfo> m_Commands;
    LogFn m_LogHandler;
    std::string m_CurrentPluginId;
};

} // namespace mf
