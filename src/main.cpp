#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#include <dbghelp.h>
#include <fstream>
#pragma comment(lib, "dbghelp.lib")
#endif

#include "app/Application.h"
#include "core/NodeRegistry.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

#if defined(_WIN32)
LONG WINAPI CrashFilter(EXCEPTION_POINTERS* exceptionInfo) {
    std::ofstream log("MaterialForge-crash.log", std::ios::trunc);
    if (exceptionInfo && exceptionInfo->ExceptionRecord) {
        log << "Exception code: 0x" << std::hex
            << static_cast<unsigned long>(exceptionInfo->ExceptionRecord->ExceptionCode)
            << " address: 0x"
            << reinterpret_cast<std::uintptr_t>(exceptionInfo->ExceptionRecord->ExceptionAddress)
            << std::dec << "\n";
    }
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    void* frames[64] = {};
    const USHORT count = CaptureStackBackTrace(0, 64, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) {
        DWORD64 displacement = 0;
        char buffer[sizeof(SYMBOL_INFO) + 512] = {};
        SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 511;
        if (SymFromAddr(GetCurrentProcess(), reinterpret_cast<DWORD64>(frames[i]),
                        &displacement, symbol)) {
            log << "  " << symbol->Name << " +0x" << std::hex << displacement << std::dec;
            IMAGEHLP_LINE64 line = {};
            line.SizeOfStruct = sizeof(line);
            DWORD lineDisplacement = 0;
            if (SymGetLineFromAddr64(GetCurrentProcess(),
                                     reinterpret_cast<DWORD64>(frames[i]),
                                     &lineDisplacement, &line)) {
                log << " (" << line.FileName << ":" << line.LineNumber << ")";
            }
            log << "\n";
        } else {
            log << "  0x" << std::hex << reinterpret_cast<std::uintptr_t>(frames[i])
                << std::dec << "\n";
        }
    }
    log.flush();
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

void PrintUsage() {
    std::cout
        << "MaterialForge - UE5-inspired material node editor\n\n"
        << "Usage:\n"
        << "  MaterialForge [project.mfproject] [graph.json]\n"
        << "                [--project <project.mfproject>] [--open <graph.json>]\n"
        << "                [--renderer <opengl|vulkan|dx12>]\n"
        << "                [--capture <screenshot.png>] [--capture-frame <n>]\n\n"
        << "Options:\n"
        << "  --project <path>       Load a MaterialForge project at startup.\n"
        << "  --open <path>          Load a material graph JSON document at startup.\n"
        << "  <path>                 A .mfproject or .json path is opened automatically.\n"
        << "  --renderer <api>       Override the preview pipeline for this run.\n"
        << "  --capture <path>       Render a screenshot and exit (development helper).\n"
        << "  --capture-frame <n>    Frame at which to capture (default: 45).\n"
        << "  --preferences          Open the Preferences window at startup.\n"
        << "  --console              Show the CMD console at startup.\n"
        << "  --collapse-library     Start with the node library collapsed.\n"
        << "  --detach-preview       Start with the live preview floating.\n"
        << "  --dump-shaders <base>  Write generated .glsl/.hlsl sources and exit.\n"
        << "  --command <command>    Execute one console command after startup.\n"
        << "  --export-node-catalog <path>  Write the node catalog JSON and exit.\n"
        << "  --help                 Show this help.\n";
}

nlohmann::json PropertyToCatalogJson(const mf::PropertyValue& value) {
    using mf::PropertyKind;
    switch (value.kind) {
        case PropertyKind::Float: return value.number;
        case PropertyKind::Bool: return value.boolean;
        case PropertyKind::Color3:
            return nlohmann::json::array({value.vec3.x, value.vec3.y, value.vec3.z});
        case PropertyKind::Color4:
        case PropertyKind::Vector4:
            return nlohmann::json::array(
                {value.vec4.x, value.vec4.y, value.vec4.z, value.vec4.w});
        case PropertyKind::Vector2:
            return nlohmann::json::array({value.vec2.x, value.vec2.y});
        case PropertyKind::Vector3:
            return nlohmann::json::array({value.vec3.x, value.vec3.y, value.vec3.z});
        case PropertyKind::String:
        case PropertyKind::AssetPath: return value.text;
        default: return nullptr;
    }
}

bool ExportNodeCatalog(const std::string& path, std::string* error) {
    nlohmann::json document;
    document["schema"] = "materialforge.node_catalog";
    document["version"] = 1;
    document["nodes"] = nlohmann::json::array();
    for (const mf::NodeDefinition& definition : mf::NodeRegistry::Get().All()) {
        nlohmann::json node;
        node["type"] = definition.type;
        node["displayName"] = definition.displayName;
        node["category"] = definition.category;
        node["ueClass"] = definition.ueClass;
        node["description"] = definition.description;
        node["isMaterialOutput"] = definition.isMaterialOutput;
        node["isFunctionOnly"] = definition.isFunctionOnly;
        node["hiddenInLibrary"] = definition.hiddenInLibrary;
        node["inputs"] = nlohmann::json::array();
        for (const mf::PinDefinition& pin : definition.inputs) {
            node["inputs"].push_back({{"name", pin.name},
                                      {"type", mf::PinTypeToString(pin.type)}});
        }
        node["outputs"] = nlohmann::json::array();
        for (const mf::PinDefinition& pin : definition.outputs) {
            node["outputs"].push_back({{"name", pin.name},
                                       {"type", mf::PinTypeToString(pin.type)}});
        }
        node["properties"] = nlohmann::json::array();
        for (const mf::PropertySpec& property : definition.properties) {
            node["properties"].push_back(
                {{"key", property.key},
                 {"label", property.label},
                 {"default", PropertyToCatalogJson(property.defaultValue)}});
        }
        document["nodes"].push_back(std::move(node));
    }
    std::ofstream stream(std::filesystem::u8path(path), std::ios::binary);
    if (!stream) {
        if (error) *error = "Could not write node catalog: " + path;
        return false;
    }
    stream << document.dump(2) << "\n";
    if (!stream) {
        if (error) *error = "Failed while writing node catalog.";
        return false;
    }
    return true;
}

void ReportError(const std::string& message) {
#if defined(_WIN32) && defined(MF_WINDOWS_GUI)
    MessageBoxA(nullptr, message.c_str(), "MaterialForge", MB_OK | MB_ICONERROR);
#else
    std::cerr << message << "\n";
#endif
}

int RunMaterialForge(const std::vector<std::string>& arguments) {
    mf::ApplicationOptions options;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const std::string& argument = arguments[i];
        if (argument == "--help" || argument == "-h") {
            PrintUsage();
            return 0;
        }
        if (argument == "--project" && i + 1 < arguments.size()) {
            options.projectPath = arguments[++i];
            continue;
        }
        if (argument == "--open" && i + 1 < arguments.size()) {
            options.openPath = arguments[++i];
            continue;
        }
        if (argument == "--capture" && i + 1 < arguments.size()) {
            options.capturePath = arguments[++i];
            continue;
        }
        if (argument == "--capture-frame" && i + 1 < arguments.size()) {
            options.captureFrame = std::max(1, std::atoi(arguments[++i].c_str()));
            continue;
        }
        if (argument == "--renderer" && i + 1 < arguments.size()) {
            options.rendererOverride = arguments[++i];
            continue;
        }
        if (argument == "--preferences") {
            options.openPreferences = true;
            continue;
        }
        if (argument == "--console") {
            options.openConsole = true;
            continue;
        }
        if (argument == "--collapse-library") {
            options.collapseLibrary = true;
            continue;
        }
        if (argument == "--detach-preview") {
            options.detachPreview = true;
            continue;
        }
        if (argument == "--dump-shaders" && i + 1 < arguments.size()) {
            options.dumpShaderPath = arguments[++i];
            continue;
        }
        if (argument == "--command" && i + 1 < arguments.size()) {
            options.startupCommand = arguments[++i];
            continue;
        }
        if (argument == "--export-node-catalog" && i + 1 < arguments.size()) {
            std::string catalogError;
            if (!ExportNodeCatalog(arguments[++i], &catalogError)) {
                ReportError(catalogError);
                return 2;
            }
            std::cout << "Node catalog written to " << arguments[i] << "\n";
            return 0;
        }
        if (argument.rfind("--", 0) != 0) {
            const std::size_t dot = argument.find_last_of('.');
            std::string extension = dot == std::string::npos ? std::string() : argument.substr(dot);
            std::transform(extension.begin(), extension.end(), extension.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (extension == ".mfproject" && options.projectPath.empty()) {
                options.projectPath = argument;
                continue;
            }
            if (extension == ".json" && options.openPath.empty()) {
                options.openPath = argument;
                continue;
            }
        }
        ReportError("Unknown argument: " + argument + "\n\nUse --help for usage.");
        return 2;
    }

    try {
#if defined(_WIN32)
        SetUnhandledExceptionFilter(&CrashFilter);
#endif
        mf::Application application(options);
        if (!application.Initialize()) {            ReportError("MaterialForge could not initialize its OpenGL window. "
                        "An OpenGL 3.3 capable GPU and driver are required.");            return 1;
        }
        application.Run();
    } catch (const std::exception& exception) {
        ReportError(std::string("Fatal error: ") + exception.what());
        return 1;
    }
    return 0;
}

#if defined(_WIN32)
std::string WideToUtf8(const wchar_t* text) {
    if (!text) return {};
    const int required = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1) return {};
    // The destination size includes the trailing NUL, so the buffer must be
    // allocated with `required` bytes and trimmed afterwards.
    std::string utf8(static_cast<std::size_t>(required), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8.data(), required,
                                            nullptr, nullptr);
    if (written <= 1) return {};
    utf8.resize(static_cast<std::size_t>(written - 1));
    return utf8;
}

std::vector<std::string> WindowsCommandLineArguments() {
    int count = 0;
    LPWSTR* wideArguments = CommandLineToArgvW(GetCommandLineW(), &count);
    std::vector<std::string> arguments;
    if (!wideArguments) {
        return arguments;
    }
    arguments.reserve(static_cast<std::size_t>(std::max(0, count - 1)));
    for (int i = 1; i < count; ++i) {
        arguments.push_back(WideToUtf8(wideArguments[i]));
    }
    LocalFree(wideArguments);
    return arguments;
}
#endif

} // namespace

#if defined(_WIN32) && defined(MF_WINDOWS_GUI)
int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    return RunMaterialForge(WindowsCommandLineArguments());
}
#elif defined(_WIN32)
int main() {
    return RunMaterialForge(WindowsCommandLineArguments());
}
#else
int main(int argc, char** argv) {
    std::vector<std::string> arguments;
    arguments.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0);
    for (int i = 1; i < argc; ++i) {
        arguments.emplace_back(argv[i]);
    }
    return RunMaterialForge(arguments);
}
#endif
