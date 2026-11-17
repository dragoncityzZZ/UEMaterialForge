#include "app/PluginManager.h"

#include "plugin/MaterialForgePlugin.h"
#include "util/PathUtil.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace mf {
namespace {

constexpr const char* kPluginSchema = "materialforge.plugin";
constexpr const char* kRuntimeVersion = "2.5.0";

std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string ManifestPathName() {
    return "plugin.json";
}

} // namespace

PluginManager::PluginManager() {
    SetRootPath(PathUtil::UserConfigDirectory() / "plugins");
}

PluginManager::~PluginManager() {
    UnloadAll();
}

void PluginManager::SetLogHandler(LogFn handler) {
    m_LogHandler = std::move(handler);
}

std::vector<PluginInfo> PluginManager::Plugins() const {
    std::vector<PluginInfo> infos;
    infos.reserve(m_Plugins.size());
    for (const LoadedPlugin& plugin : m_Plugins) infos.push_back(plugin.info);
    return infos;
}

void PluginManager::SetRootPath(fs::path path) {
    UnloadAll();
    m_RootPath = std::move(path);
    MakeRoot();
}

void PluginManager::MakeRoot() {
    if (m_RootPath.empty()) {
        m_RootPath = PathUtil::UserConfigDirectory() / "plugins";
    }
    std::error_code ec;
    fs::create_directories(m_RootPath, ec);
    if (ec) {
        const fs::path fallback = PathUtil::CurrentDirectory() / ".materialforge" / "plugins";
        fs::create_directories(fallback, ec);
        if (!ec) {
            m_RootPath = fallback;
            if (m_LogHandler) {
                m_LogHandler(1, "Plugin directory fell back to " + PathUtil::ToUtf8(fallback));
            }
        }
    }
}

bool PluginManager::IsValidPluginId(const std::string& id, std::string* error) {
    if (id.empty()) {
        if (error) *error = "Plugin id cannot be empty.";
        return false;
    }
    for (unsigned char ch : id) {
        if (!(std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.')) {
            if (error) *error =
                "Plugin id may only contain letters, numbers, '.', '_' and '-'.";
            return false;
        }
    }
    return true;
}

bool PluginManager::ReadManifest(const fs::path& folder, PluginInfo* info,
                                 std::string* error) const {
    if (!info) return false;
    std::ifstream stream(folder / ManifestPathName(), std::ios::binary);
    if (!stream) {
        if (error) *error = "Missing plugin.json.";
        return false;
    }
    try {
        nlohmann::json document;
        stream >> document;
        if (!document.is_object()) {
            if (error) *error = "plugin.json root must be an object.";
            return false;
        }
        if (document.value("schema", std::string()) != kPluginSchema) {
            if (error) *error = "Unsupported plugin schema.";
            return false;
        }
        PluginInfo parsed;
        parsed.folder = folder;
        parsed.id = document.value("id", PathUtil::ToUtf8(folder.filename()));
        parsed.name = document.value("name", parsed.id);
        parsed.version = document.value("version", "1.0.0");
        parsed.author = document.value("author", std::string());
        parsed.description = document.value("description", std::string());
        parsed.entry = document.value("entry", std::string());
        parsed.enabled = document.value("enabled", true);
        if (!IsValidPluginId(parsed.id, error)) return false;
        if (error) error->clear();
        *info = std::move(parsed);
        return true;
    } catch (const std::exception& exception) {
        if (error) *error = std::string("Could not parse plugin.json: ") + exception.what();
        return false;
    }
}

void PluginManager::Refresh() {
    UnloadAll();
    m_Plugins.clear();
    m_Commands.clear();
    MakeRoot();
    std::error_code ec;
    if (!fs::is_directory(m_RootPath, ec)) {
        if (m_LogHandler) m_LogHandler(2, "Plugin directory does not exist.");
        return;
    }
    std::vector<fs::path> folders;
    for (fs::directory_iterator it(m_RootPath, fs::directory_options::skip_permission_denied, ec),
         end;
         !ec && it != end; it.increment(ec)) {
        if (it->is_directory(ec)) folders.push_back(it->path());
    }
    std::sort(folders.begin(), folders.end());
    for (const fs::path& folder : folders) {
        LoadedPlugin plugin;
        std::string readError;
        if (!ReadManifest(folder, &plugin.info, &readError)) {
            plugin.info.id = PathUtil::ToUtf8(folder.filename());
            plugin.info.name = plugin.info.id;
            plugin.info.folder = folder;
            plugin.info.enabled = false;
            plugin.info.error = readError;
            m_Plugins.push_back(std::move(plugin));
            continue;
        }
        if (plugin.info.enabled) {
            m_Plugins.push_back(std::move(plugin));
            LoadPlugin(m_Plugins.back());
        } else {
            m_Plugins.push_back(std::move(plugin));
        }
    }
    if (m_LogHandler) {
        m_LogHandler(0, "Loaded " + std::to_string(m_Plugins.size()) + " plugin folder(s).");
    }
}

void PluginManager::UnloadAll() {
    for (LoadedPlugin& plugin : m_Plugins) {
        if (plugin.library) UnloadPlugin(plugin);
    }
    m_Plugins.clear();
    m_Commands.clear();
}

void PluginManager::LoadPlugin(LoadedPlugin& plugin) {
    plugin.library = nullptr;
    plugin.hostContext = nullptr;
    plugin.shutdown = nullptr;
    plugin.info.loaded = false;
    if (plugin.info.entry.empty()) {
        plugin.info.error = "plugin.json has no entry DLL.";
        return;
    }
    fs::path entryPath = PathUtil::FromUtf8(plugin.info.entry);
    if (entryPath.is_relative()) entryPath = plugin.info.folder / entryPath;
    std::error_code ec;
    if (!fs::is_regular_file(entryPath, ec)) {
        plugin.info.error = "Plugin DLL not found: " + PathUtil::ToUtf8(entryPath);
        return;
    }
#if defined(_WIN32)
    HMODULE library = ::LoadLibraryW(entryPath.c_str());
    if (!library) {
        plugin.info.error = "LoadLibrary failed (error " +
                            std::to_string(static_cast<unsigned long>(::GetLastError())) + ").";
        return;
    }
    using InitFn = int (*)(const MfPluginApi*);
    auto init = reinterpret_cast<InitFn>(
        reinterpret_cast<void*>(::GetProcAddress(library, "MaterialForgePluginInit")));
    if (!init) {
        ::FreeLibrary(library);
        plugin.info.error = "DLL does not export MaterialForgePluginInit.";
        return;
    }
    auto* context = new HostContext();
    context->manager = this;
    context->pluginId = plugin.info.id;
    // The plugin keeps the API pointer for its whole lifetime, so it must not
    // point at a stack local (or at an element that may move with the vector).
    auto* apiBlock = new MfPluginApi();
    apiBlock->api_version = MF_PLUGIN_API_VERSION;
    apiBlock->host = context;
    apiBlock->log = &PluginManager::HostLog;
    apiBlock->runtime_version = &PluginManager::HostRuntimeVersion;
    apiBlock->register_command = &PluginManager::HostRegisterCommand;
    if (init(apiBlock) == 0) {
        delete apiBlock;
        delete context;
        ::FreeLibrary(library);
        plugin.info.error = "MaterialForgePluginInit returned false.";
        return;
    }
    plugin.library = library;
    plugin.hostContext = context;
    plugin.apiBlock = apiBlock;
    plugin.shutdown = reinterpret_cast<void (*)()>(
        reinterpret_cast<void*>(::GetProcAddress(library, "MaterialForgePluginShutdown")));
    plugin.info.loaded = true;
    plugin.info.error.clear();
    if (m_LogHandler) {
        m_LogHandler(0, "Loaded plugin " + plugin.info.name + " " + plugin.info.version);
    }
#else
    plugin.info.error = "DLL plugins are only supported on Windows in this build.";
#endif
}

void PluginManager::UnloadPlugin(LoadedPlugin& plugin) {
#if defined(_WIN32)
    if (plugin.shutdown) {
        plugin.shutdown();
        plugin.shutdown = nullptr;
    }
    if (plugin.library) {
        ::FreeLibrary(static_cast<HMODULE>(plugin.library));
        plugin.library = nullptr;
    }
    if (plugin.hostContext) {
        delete static_cast<HostContext*>(plugin.hostContext);
        plugin.hostContext = nullptr;
    }
    if (plugin.apiBlock) {
        delete static_cast<MfPluginApi*>(plugin.apiBlock);
        plugin.apiBlock = nullptr;
    }
#endif
    m_Commands.erase(std::remove_if(m_Commands.begin(), m_Commands.end(),
                                    [&](const PluginCommandInfo& command) {
                                        return command.pluginId == plugin.info.id;
                                    }),
                     m_Commands.end());
    plugin.info.loaded = false;
}

bool PluginManager::Install(const fs::path& sourceDirectory, std::string* error) {
    std::error_code ec;
    if (!fs::is_directory(sourceDirectory, ec)) {
        if (error) *error = "The selected plugin folder does not exist.";
        return false;
    }
    PluginInfo info;
    std::string manifestError;
    if (!ReadManifest(sourceDirectory, &info, &manifestError)) {
        if (error) *error = manifestError;
        return false;
    }
    MakeRoot();
    const fs::path target = m_RootPath / PathUtil::ToUtf8(info.id);
    if (fs::exists(target, ec)) {
        if (fs::equivalent(sourceDirectory, target, ec)) {
            Refresh();
            if (error) error->clear();
            return true;
        }
        if (error) *error = "A plugin with this id is already installed: " + info.id;
        return false;
    }
    fs::copy(sourceDirectory, target, fs::copy_options::recursive, ec);
    if (ec) {
        if (error) *error = "Could not copy the plugin folder: " + ec.message();
        return false;
    }
    Refresh();
    if (error) error->clear();
    return true;
}

bool PluginManager::Uninstall(const std::string& id, std::string* error) {
    auto it = std::find_if(m_Plugins.begin(), m_Plugins.end(),
                           [&](const LoadedPlugin& plugin) { return plugin.info.id == id; });
    if (it == m_Plugins.end()) {
        if (error) *error = "Plugin not found: " + id;
        return false;
    }
    if (it->library) UnloadPlugin(*it);
    std::error_code ec;
    fs::remove_all(it->info.folder, ec);
    if (ec) {
        if (error) *error = "Could not remove the plugin folder: " + ec.message();
        return false;
    }
    m_Plugins.erase(it);
    if (error) error->clear();
    return true;
}

bool PluginManager::SetEnabled(const std::string& id, bool enabled, std::string* error) {
    auto it = std::find_if(m_Plugins.begin(), m_Plugins.end(),
                           [&](const LoadedPlugin& plugin) { return plugin.info.id == id; });
    if (it == m_Plugins.end()) {
        if (error) *error = "Plugin not found: " + id;
        return false;
    }
    if (!WriteManifestEnabled(it->info, enabled, error)) return false;
    Refresh();
    if (error) error->clear();
    return true;
}

bool PluginManager::Reload(const std::string& id, std::string* error) {
    (void)id;
    Refresh();
    if (error) error->clear();
    return true;
}

bool PluginManager::WriteManifestEnabled(const PluginInfo& info, bool enabled,
                                         std::string* error) {
    const fs::path path = info.folder / ManifestPathName();
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        if (error) *error = "Could not open plugin.json for writing.";
        return false;
    }
    nlohmann::json document;
    try {
        input >> document;
    } catch (const std::exception& exception) {
        if (error) *error = std::string("Could not parse plugin.json: ") + exception.what();
        return false;
    }
    document["enabled"] = enabled;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        if (error) *error = "Could not write plugin.json.";
        return false;
    }
    output << document.dump(2) << "\n";
    if (!output) {
        if (error) *error = "Could not finish writing plugin.json.";
        return false;
    }
    if (error) error->clear();
    return true;
}

bool PluginManager::RegisterCommand(const std::string& pluginId, const std::string& name,
                                    const std::string& description,
                                    void (*callback)(const char*, void*), void* user) {
    if (name.empty() || !callback) return false;
    const std::string commandName = ToLower(name);
    const auto exists = std::find_if(m_Commands.begin(), m_Commands.end(),
                                     [&](const PluginCommandInfo& command) {
                                         return command.name == commandName;
                                     });
    if (exists != m_Commands.end()) {
        if (m_LogHandler) {
            m_LogHandler(2, "Plugin command already exists: " + commandName);
        }
        return false;
    }
    m_Commands.push_back({pluginId, commandName, description, user, callback});
    for (LoadedPlugin& plugin : m_Plugins) {
        if (plugin.info.id == pluginId) {
            plugin.info.commands.push_back(commandName);
            break;
        }
    }
    return true;
}

bool PluginManager::ExecuteCommand(const std::string& name, const std::string& args) {
    const std::string commandName = ToLower(name);
    const auto it = std::find_if(m_Commands.begin(), m_Commands.end(),
                                 [&](const PluginCommandInfo& command) {
                                     return command.name == commandName;
                                 });
    if (it == m_Commands.end()) return false;
    it->callback(args.c_str(), it->user);
    return true;
}

void PluginManager::HostLog(void* host, int level, const char* message) {
    auto* context = static_cast<HostContext*>(host);
    if (!context || !context->manager || !context->manager->m_LogHandler) return;
    const std::string text = message ? message : "";
    context->manager->m_LogHandler(level, "[" + context->pluginId + "] " + text);
}

const char* PluginManager::HostRuntimeVersion(void* host) {
    (void)host;
    return kRuntimeVersion;
}

int PluginManager::HostRegisterCommand(void* host, const char* name, const char* description,
                                       void (*callback)(const char*, void*), void* user) {
    auto* context = static_cast<HostContext*>(host);
    if (!context || !context->manager) return 0;
    return context->manager->RegisterCommand(context->pluginId, name ? name : "",
                                             description ? description : "", callback, user)
               ? 1
               : 0;
}

} // namespace mf
