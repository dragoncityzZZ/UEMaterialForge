#include "app/PluginManager.h"

#include "app/UiWidgets.h"
#include "plugin/MaterialForgePlugin.h"
#include "util/PathUtil.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
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
#ifndef MF_VERSION_STRING
#define MF_VERSION_STRING "2.6.7"
#endif
constexpr const char* kRuntimeVersion = MF_VERSION_STRING;

std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string ManifestPathName() {
    return "plugin.json";
}

#if defined(_WIN32)
std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0,
                                           nullptr, nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), length, nullptr, nullptr);
    return result;
}

std::string WindowsErrorMessage(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::string message;
    if (length > 0 && buffer) {
        message = WideToUtf8(std::wstring(buffer, length));
        while (!message.empty() &&
               (message.back() == '\r' || message.back() == '\n' ||
                message.back() == ' ')) {
            message.pop_back();
        }
    }
    if (buffer) LocalFree(buffer);
    if (message.empty()) message = "unknown Windows error";
    return message;
}

std::string PluginMachine(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return "unreadable";
    IMAGE_DOS_HEADER dos = {};
    stream.read(reinterpret_cast<char*>(&dos), sizeof(dos));
    if (!stream || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0) {
        return "not a PE file";
    }
    stream.seekg(dos.e_lfanew, std::ios::beg);
    DWORD signature = 0;
    stream.read(reinterpret_cast<char*>(&signature), sizeof(signature));
    if (!stream || signature != IMAGE_NT_SIGNATURE) return "not a PE file";
    IMAGE_FILE_HEADER header = {};
    stream.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!stream) return "unreadable PE header";
    switch (header.Machine) {
    case IMAGE_FILE_MACHINE_AMD64:
        return "x64";
    case IMAGE_FILE_MACHINE_I386:
        return "x86";
    case IMAGE_FILE_MACHINE_ARM64:
        return "ARM64";
    default:
        return "unknown machine 0x" +
               [&] {
                   std::ostringstream out;
                   out << std::hex << header.Machine;
                   return out.str();
               }();
    }
}

std::string LoadErrorDetails(DWORD code, const fs::path& path) {
    std::ostringstream out;
    out << "LoadLibraryW failed for '" << PathUtil::ToUtf8(path) << "'.";
    out << " Windows error " << code << ": " << WindowsErrorMessage(code) << ".";
    if (code == ERROR_BAD_EXE_FORMAT) {
        out << " The plugin DLL is likely the wrong architecture; build it for x64.";
    } else if (code == ERROR_MOD_NOT_FOUND || code == ERROR_PROC_NOT_FOUND) {
        out << " A dependency DLL or an exported function is missing. "
               "If the plugin uses the C++ runtime, rebuild it with the static "
               "MSVC runtime (/MT) or ship the required VC++ runtime DLLs.";
    } else if (code == ERROR_ACCESS_DENIED) {
        out << " Windows blocked loading the DLL (permissions, antivirus, or "
               "security policy).";
    }
    out << " Detected DLL architecture: " << PluginMachine(path) << ".";
    return out.str();
}
#endif

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
    const fs::path manifestPath = folder / ManifestPathName();
    std::ifstream stream(manifestPath, std::ios::binary);
    if (!stream) {
        if (error) {
            *error = "Missing plugin.json in '" + PathUtil::ToUtf8(folder) + "'.";
        }
        return false;
    }
    try {
        nlohmann::json document;
        stream >> document;
        if (!document.is_object()) {
            if (error) *error = "plugin.json root must be a JSON object.";
            return false;
        }
        if (!document.contains("schema") || !document["schema"].is_string() ||
            document["schema"].get<std::string>() != kPluginSchema) {
            if (error) {
                *error = "plugin.json field 'schema' must be \"" +
                         std::string(kPluginSchema) + "\".";
            }
            return false;
        }
        if (document.contains("schemaVersion") &&
            (!document["schemaVersion"].is_number_integer() ||
             document["schemaVersion"].get<int>() != 1)) {
            if (error) *error = "plugin.json field 'schemaVersion' must be 1.";
            return false;
        }
        PluginInfo parsed;
        parsed.folder = folder;
        if (!document.contains("id") || !document["id"].is_string() ||
            document["id"].get<std::string>().empty()) {
            if (error) *error = "plugin.json field 'id' is required and must be a non-empty string.";
            return false;
        }
        parsed.id = document["id"].get<std::string>();
        if (!IsValidPluginId(parsed.id, error)) {
            if (error) *error = "Invalid plugin id: " + *error;
            return false;
        }
        parsed.name = document.value("name", parsed.id);
        parsed.version = document.value("version", "1.0.0");
        parsed.author = document.value("author", std::string());
        parsed.description = document.value("description", std::string());
        parsed.entry = document.value("entry", std::string());
        parsed.enabled = document.value("enabled", true);
        if (parsed.entry.empty()) {
            if (error) {
                *error = "plugin.json field 'entry' is required and must name the plugin DLL "
                         "(for example \"MyPlugin.dll\").";
            }
            return false;
        }
        if (error) error->clear();
        *info = std::move(parsed);
        return true;
    } catch (const std::exception& exception) {
        if (error) {
            *error = "Could not parse plugin.json ('" + PathUtil::ToUtf8(manifestPath) +
                     "'): " + exception.what();
        }
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
    plugin.apiBlock = nullptr;
    plugin.shutdown = nullptr;
    plugin.info.loaded = false;
    plugin.info.error.clear();
    plugin.info.warning.clear();
    if (plugin.info.entry.empty()) {
        plugin.info.error = "plugin.json has no entry DLL.";
        return;
    }
    fs::path entryPath = PathUtil::FromUtf8(plugin.info.entry);
    if (entryPath.is_relative()) entryPath = plugin.info.folder / entryPath;
    std::error_code ec;
    if (!fs::is_regular_file(entryPath, ec)) {
        plugin.info.error = "Plugin entry DLL was not found: '" +
                            PathUtil::ToUtf8(entryPath) +
                            "'. Check the 'entry' field in plugin.json and build the DLL.";
        return;
    }
    const fs::path canonicalFolder = fs::weakly_canonical(plugin.info.folder, ec);
    const fs::path canonicalEntry = fs::weakly_canonical(entryPath, ec);
    if (!ec) {
        const fs::path relative = fs::relative(canonicalEntry, canonicalFolder, ec);
        if (ec || relative.empty() ||
            relative.generic_u8string().rfind("..", 0) == 0) {
            plugin.info.error =
                "plugin.json 'entry' must point to a DLL inside the plugin folder. "
                "Absolute paths and '..' escapes are rejected.";
            return;
        }
    }
#if defined(_WIN32)
    const std::string machine = PluginMachine(entryPath);
    if (machine != "x64") {
        plugin.info.error = "Plugin DLL architecture is '" + machine +
                            "', but MaterialForge is x64. Build the plugin with "
                            "the x64 MSVC toolchain.";
        return;
    }
    HMODULE library = ::LoadLibraryW(entryPath.c_str());
    if (!library) {
        plugin.info.error = LoadErrorDetails(::GetLastError(), entryPath);
        return;
    }
    using InitFn = int (*)(const MfPluginApi*);
    auto init = reinterpret_cast<InitFn>(
        reinterpret_cast<void*>(::GetProcAddress(library, "MaterialForgePluginInit")));
    if (!init) {
        const DWORD code = ::GetLastError();
        ::FreeLibrary(library);
        plugin.info.error =
            "DLL does not export MaterialForgePluginInit (Windows error " +
            std::to_string(static_cast<unsigned long>(code)) + ": " +
            WindowsErrorMessage(code) +
            "). Required signature: extern \"C\" __declspec(dllexport) "
            "int MaterialForgePluginInit(const MfPluginApi* api). "
            "If the function is C++, it is name-mangled and cannot be loaded.";
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
    apiBlock->set_theme = &PluginManager::HostSetTheme;
    apiBlock->set_style_color = &PluginManager::HostSetStyleColor;
    apiBlock->clear_style_color = &PluginManager::HostClearStyleColor;
    apiBlock->set_node_color = &PluginManager::HostSetNodeColor;
    apiBlock->clear_node_color = &PluginManager::HostClearNodeColor;
    apiBlock->set_link_style = &PluginManager::HostSetLinkStyle;
    apiBlock->clear_link_style = &PluginManager::HostClearLinkStyle;
    apiBlock->set_ui_scale = &PluginManager::HostSetUiScale;
    apiBlock->set_style_float = &PluginManager::HostSetStyleFloat;
    apiBlock->set_node_style = &PluginManager::HostSetNodeStyle;
    apiBlock->set_canvas_style = &PluginManager::HostSetCanvasStyle;
    int initResult = 0;
    std::string initException;
    try {
        initResult = init(apiBlock);
    } catch (const std::exception& exception) {
        initException = exception.what();
    } catch (...) {
        initException = "unknown exception";
    }
    if (initResult == 0 || !initException.empty()) {
        std::string message = "MaterialForgePluginInit failed";
        if (!initException.empty()) {
            message += " with an exception: " + initException + ".";
        } else {
            message += " and returned 0.";
        }
        if (!context->lastError.empty()) {
            message += "\nPlugin reported: " + context->lastError;
        } else {
            message +=
                "\nNo diagnostic was written by the plugin. Common causes: API "
                "version mismatch, initialization validation failed, or a required "
                "dependency/component is missing. Log an error with api->log(host, 2, ...) "
                "before returning 0 to see the exact reason here.";
        }
        delete apiBlock;
        delete context;
        ::FreeLibrary(library);
        plugin.info.error = std::move(message);
        return;
    }
    plugin.library = library;
    plugin.hostContext = context;
    plugin.apiBlock = apiBlock;
    plugin.shutdown = reinterpret_cast<void (*)()>(
        reinterpret_cast<void*>(::GetProcAddress(library, "MaterialForgePluginShutdown")));
    plugin.info.loaded = true;
    if (!plugin.shutdown) {
        plugin.info.warning =
            "Plugin loaded, but does not export MaterialForgePluginShutdown. "
            "Resources will not be released cleanly.";
    }
    if (plugin.info.commands.empty()) {
        if (!plugin.info.warning.empty()) plugin.info.warning += "\n";
        plugin.info.warning +=
            "Plugin loaded, but registered no CMD commands. Check register_command "
            "return values and the command name.";
    }
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
        try {
            plugin.shutdown();
        } catch (const std::exception& exception) {
            if (m_LogHandler) {
                m_LogHandler(2, "Plugin '" + plugin.info.id +
                                    "' threw during shutdown: " + exception.what());
            }
        } catch (...) {
            if (m_LogHandler) {
                m_LogHandler(2, "Plugin '" + plugin.info.id +
                                    "' threw an unknown exception during shutdown.");
            }
        }
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
    if (!context || !context->manager) return;
    const std::string text = message ? message : "";
    if (level >= 2) {
        if (!context->lastError.empty()) context->lastError.push_back('\n');
        context->lastError += text;
    }
    if (context->manager->m_LogHandler) {
        context->manager->m_LogHandler(level, "[" + context->pluginId + "] " + text);
    }
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

int PluginManager::HostSetTheme(void* host, const char* name) {
    if (!host || !name) return 0;
    const std::string theme = ToLower(name);
    if (theme != "graphite" && theme != "midnight") return 0;
    Ui::SetPluginTheme(theme);
    return 1;
}

int PluginManager::HostSetStyleColor(void* host, const char* key, uint32_t rgba) {
    if (!host || !key) return 0;
    return Ui::SetPluginStyleColor(key, rgba) ? 1 : 0;
}

int PluginManager::HostClearStyleColor(void* host, const char* key) {
    if (!host || !key) return 0;
    return Ui::ClearPluginStyleColor(key) ? 1 : 0;
}

int PluginManager::HostSetNodeColor(void* host, const char* key, uint32_t rgba) {
    if (!host || !key || !key[0]) return 0;
    Ui::SetPluginNodeColor(key, rgba);
    return 1;
}

int PluginManager::HostClearNodeColor(void* host, const char* key) {
    if (!host || !key || !key[0]) return 0;
    Ui::ClearPluginNodeColor(key);
    return 1;
}

int PluginManager::HostSetLinkStyle(void* host, uint32_t rgba, float thickness, int shape) {
    if (!host) return 0;
    Ui::SetPluginLinkStyle(rgba, thickness, shape);
    return 1;
}

int PluginManager::HostClearLinkStyle(void* host) {
    if (!host) return 0;
    Ui::ClearPluginLinkStyle();
    return 1;
}

int PluginManager::HostSetUiScale(void* host, float scale) {
    if (!host) return 0;
    Ui::SetScale(scale);
    Ui::ApplyStyle();
    return 1;
}

int PluginManager::HostSetStyleFloat(void* host, const char* key, float value) {
    if (!host || !key) return 0;
    return Ui::SetPluginStyleFloat(key, value) ? 1 : 0;
}

int PluginManager::HostSetNodeStyle(void* host, float rounding, float border,
                                    float pinRounding, float pinBorder) {
    if (!host) return 0;
    Ui::SetPluginNodeStyle(rounding, border, pinRounding, pinBorder);
    return 1;
}

int PluginManager::HostSetCanvasStyle(void* host, uint32_t background, uint32_t grid,
                                      uint32_t nodeBackground, uint32_t nodeBorder,
                                      uint32_t selection) {
    if (!host) return 0;
    Ui::SetPluginCanvasStyle(background, grid, nodeBackground, nodeBorder, selection);
    return 1;
}

} // namespace mf
