#include "app/Application.h"

#include "app/NodeSearch.h"
#include "app/NodeThumbnail.h"
#include "app/UiWidgets.h"
#include "core/DefaultGraph.h"
#include "core/NodeRegistry.h"
#include "core/TypeResolver.h"
#include "io/GraphSerializer.h"
#include "io/TextureLoader.h"
#include "io/Ue5Clipboard.h"
#include "render/HlslPreviewShader.h"
#include "util/PathUtil.h"

#include <glad/gl.h>
#include <GLFW/glfw3.h>
#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <shobjidl.h>
#endif
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_stdlib.h>
#include <nlohmann/json.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <limits>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace mf {
namespace {

constexpr float kLeftPanelWidth = 244.0f;
constexpr float kRightPanelWidth = 440.0f;
constexpr float kPanelGap = 10.0f;
constexpr float kToolbarHeight = 72.0f;
constexpr float kStatusHeight = 26.0f;
constexpr std::size_t kMaxConsoleEntries = 800;

std::vector<std::string> g_GlfwErrors;

GLFWwindow* CreateBestContextWindow(int width, int height) {
    struct ContextVersion {
        int major;
        int minor;
    };
    const ContextVersion versions[] = {{4, 6}, {4, 3}, {3, 3}};
    for (const ContextVersion& version : versions) {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, version.major);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, version.minor);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
        glfwWindowHint(GLFW_SAMPLES, 0);
        glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
        GLFWwindow* window = glfwCreateWindow(width, height, "MaterialForge", nullptr, nullptr);
        if (window) {
            return window;
        }
    }
    return nullptr;
}

std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string Trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::vector<std::string> SplitCsvLine(const std::string& line) {
    std::vector<std::string> fields;
    std::string field;
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (ch == '"') {
            if (quoted && i + 1 < line.size() && line[i + 1] == '"') {
                field.push_back('"');
                ++i;
            } else {
                quoted = !quoted;
            }
        } else if (ch == ',' && !quoted) {
            fields.push_back(Trim(field));
            field.clear();
        } else if (ch != '\r') {
            field.push_back(ch);
        }
    }
    fields.push_back(Trim(field));
    return fields;
}

bool IsTextureFile(const fs::path& path) {
    const std::string extension = ToLower(PathUtil::ToUtf8(path.extension()));
    return extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
           extension == ".tga" || extension == ".bmp" || extension == ".hdr";
}

const ImWchar* CjkGlyphRanges(ImFontAtlas* atlas) {
    static ImVector<ImWchar> ranges;
    if (ranges.empty()) {
        ImFontGlyphRangesBuilder builder;
        builder.AddRanges(atlas->GetGlyphRangesChineseSimplifiedCommon());
        // The common 2500-glyph subset omits a few characters used by the
        // Chinese UI (for example 编辑, 矢量 and 菲涅尔).
        builder.AddText(u8"编辑矢量菲涅尔幂流墨浏ƒ函");
        // Characters used by the preferences, console and preview-detach UI.
        builder.AddText(u8"渲染管线浮动停靠偏好设置检测当前可用不可界面工作区域收起隐藏控制台命令"
                        u8"清空复制详情上下文接口合成错误警告信息分离拖动选项尺寸主题语言");
        builder.BuildRanges(&ranges);
    }
    return ranges.Data;
}

std::string FormatLocalized(const char* key, ...) {
    const char* format = Tr(key);
    char buffer[2048];
    va_list args;
    va_start(args, key);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return buffer;
}

std::string LocalizedErrorText(const std::string& error) {
    const char* exact = Tr(error.c_str());
    if (exact != error.c_str()) {
        return exact;
    }

    static const char* const prefixes[] = {
        "Could not create folder: ",
        "Could not create project folder: ",
        "Could not build the new material: ",
        "Could not open file: ",
        "Could not create directory: ",
    };
    for (const char* prefix : prefixes) {
        const std::string key(prefix);
        if (error.rfind(key, 0) == 0) {
            return std::string(Tr(prefix)) + error.substr(key.size());
        }
    }
    return error;
}

std::string SafeFileBase(std::string value) {
    if (value.empty()) value = "M_MaterialForge";
    for (char& ch : value) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (!(std::isalnum(c) || ch == '_' || ch == '-' || ch == '.')) {
            ch = '_';
        }
    }
    return value;
}

std::string BaseName(const fs::path& path) {
    return PathUtil::ToUtf8(path.filename());
}

std::string DocumentTitle(const fs::path& path, const MaterialGraph& graph, bool function) {
    if (!path.empty()) {
        return PathUtil::ToUtf8(path.stem());
    }
    if (!graph.Settings().name.empty()) {
        return graph.Settings().name;
    }
    return function ? Tr("Untitled Function") : Tr("Untitled Material");
}

std::string NormalizedPathKey(const fs::path& path) {
    if (path.empty()) return {};
    std::error_code ec;
    fs::path absolute = fs::absolute(path, ec);
    if (ec) absolute = path;
    return PathUtil::ToUtf8(absolute.lexically_normal());
}

#if defined(_WIN32)
enum class NativeFolderResult {
    Selected,
    Cancelled,
    Failed
};

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), length);
    return result;
}

NativeFolderResult PickFolderDialog(const fs::path& initialDirectory, fs::path* selected,
                                    HWND owner, const std::wstring& title) {
    if (!selected) return NativeFolderResult::Failed;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool uninitialize = SUCCEEDED(init);

    IFileOpenDialog* dialog = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || !dialog) {
        if (uninitialize) CoUninitialize();
        return NativeFolderResult::Failed;
    }

    FILEOPENDIALOGOPTIONS options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                           FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT);
    }
    if (!title.empty()) {
        dialog->SetTitle(title.c_str());
    }
    if (!initialDirectory.empty()) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(initialDirectory.c_str(), nullptr,
                                                  IID_PPV_ARGS(&folder))) &&
            folder) {
            dialog->SetFolder(folder);
            folder->Release();
        }
    }

    NativeFolderResult result = NativeFolderResult::Failed;
    const HRESULT shown = dialog->Show(owner);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        result = NativeFolderResult::Cancelled;
    } else if (SUCCEEDED(shown)) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                *selected = fs::path(path);
                CoTaskMemFree(path);
                result = NativeFolderResult::Selected;
            }
            item->Release();
        }
    }
    dialog->Release();
    if (uninitialize) CoUninitialize();
    return result;
}

std::vector<fs::path> FindProjectFiles(const fs::path& folder) {
    std::vector<fs::path> projects;
    std::error_code ec;
    fs::directory_iterator iterator(folder, ec);
    if (ec) return projects;
    for (const fs::directory_entry& entry : iterator) {
        std::error_code entryError;
        if (!entry.is_regular_file(entryError)) continue;
        if (ToLower(PathUtil::ToUtf8(entry.path().extension())) == ".mfproject") {
            projects.push_back(entry.path());
        }
    }
    std::sort(projects.begin(), projects.end());
    return projects;
}

fs::path ChooseProjectFile(const fs::path& folder, const std::vector<fs::path>& projects) {
    if (projects.empty()) return {};
    if (projects.size() == 1) return projects.front();
    const std::string folderName = ToLower(PathUtil::ToUtf8(folder.filename()));
    for (const fs::path& project : projects) {
        if (ToLower(PathUtil::ToUtf8(project.stem())) == folderName) return project;
    }
    fs::path newest = projects.front();
    fs::file_time_type newestTime{};
    for (const fs::path& project : projects) {
        std::error_code error;
        const fs::file_time_type time = fs::last_write_time(project, error);
        if (!error && (newestTime == fs::file_time_type{} || time > newestTime)) {
            newest = project;
            newestTime = time;
        }
    }
    return newest;
}
#endif

ImU32 FolderAccentColor(const std::string& hex, ImU32 fallback) {
    std::string value = hex;
    if (!value.empty() && value.front() == '#') value.erase(value.begin());
    if (value.size() != 6) return fallback;
    const auto hexDigit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    int channels[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        const int high = hexDigit(value[i * 2]);
        const int low = hexDigit(value[i * 2 + 1]);
        if (high < 0 || low < 0) return fallback;
        channels[i] = high * 16 + low;
    }
    return IM_COL32(channels[0], channels[1], channels[2], 255);
}

std::string HumanReadableSize(std::uintmax_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 3) {
        value /= 1024.0;
        ++unit;
    }
    char buffer[64];
    if (unit == 0) {
        std::snprintf(buffer, sizeof(buffer), "%llu B", static_cast<unsigned long long>(bytes));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.1f %s", value, units[unit]);
    }
    return buffer;
}

std::string KindLabel(const std::string& kind) {
    if (kind == "folder") return Tr("Folder");
    if (kind == "texture") return Tr("Texture");
    if (kind == "model") return Tr("Model");
    if (kind == "material") return Tr("Material");
    if (kind == "instance") return Tr("Material Instance");
    if (kind == "function") return Tr("Blueprint Function");
    if (kind == "project") return Tr("Project");
    if (kind == "text") return Tr("Text");
    if (kind == "shader") return Tr("Shader");
    return Tr("File");
}

ImVec4 KindColor(const std::string& kind) {
    if (kind == "folder") return ImVec4(0.42f, 0.82f, 0.92f, 1.0f);
    if (kind == "texture") return ImVec4(1.00f, 0.67f, 0.32f, 1.0f);
    if (kind == "model") return ImVec4(0.72f, 0.58f, 1.00f, 1.0f);
    if (kind == "material") return ImVec4(0.31f, 0.84f, 0.78f, 1.0f);
    if (kind == "instance") return ImVec4(0.45f, 0.92f, 0.66f, 1.0f);
    if (kind == "function") return ImVec4(0.56f, 0.72f, 1.00f, 1.0f);
    if (kind == "project") return ImVec4(0.98f, 0.72f, 0.31f, 1.0f);
    if (kind == "text") return ImVec4(0.55f, 0.70f, 0.76f, 1.0f);
    if (kind == "shader") return ImVec4(0.52f, 0.66f, 1.00f, 1.0f);
    return ImVec4(0.56f, 0.64f, 0.68f, 1.0f);
}

void DrawStatusPill(const char* label, bool ok, bool neutral = false) {
    const ImVec2 textSize = ImGui::CalcTextSize(label);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float height = 26.0f;
    const ImVec2 end(start.x + textSize.x + 30.0f, start.y + height);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImU32 background = neutral ? IM_COL32(40, 46, 50, 255)
                                     : (ok ? IM_COL32(18, 54, 48, 255) : IM_COL32(70, 30, 29, 255));
    const ImU32 border = neutral ? IM_COL32(112, 124, 130, 110)
                                 : (ok ? IM_COL32(52, 156, 135, 110) : IM_COL32(178, 73, 67, 120));
    const ImU32 dot = neutral ? IM_COL32(168, 182, 188, 255)
                              : (ok ? IM_COL32(69, 222, 191, 255) : IM_COL32(247, 102, 92, 255));
    drawList->AddRectFilled(start, end, background, 13.0f);
    drawList->AddRect(start, end, border, 13.0f);
    drawList->AddCircleFilled(ImVec2(start.x + 13.0f, start.y + height * 0.5f), 3.5f, dot, 12);
    drawList->AddText(ImVec2(start.x + 21.0f, start.y + (height - textSize.y) * 0.5f),
                      neutral ? IM_COL32(196, 208, 212, 255)
                              : (ok ? IM_COL32(147, 234, 214, 255) : IM_COL32(244, 162, 156, 255)),
                      label);
    ImGui::Dummy(ImVec2(textSize.x + 30.0f, height));
}

const char* NodeDisplayName(const Node& node) {
    const NodeDefinition* definition = NodeRegistry::Get().Find(node.type);
    return definition ? definition->displayName.c_str() : node.type.c_str();
}

void WriteToVector(void* context, void* data, int size) {
    auto* output = static_cast<std::vector<unsigned char>*>(context);
    const auto* bytes = static_cast<const unsigned char*>(data);
    output->insert(output->end(), bytes, bytes + size);
}

std::size_t Utf8Step(const std::string& text, std::size_t index) {
    if (index >= text.size()) return text.size();
    const unsigned char lead = static_cast<unsigned char>(text[index]);
    std::size_t step = 1;
    if ((lead & 0xF8) == 0xF0) step = 4;
    else if ((lead & 0xF0) == 0xE0) step = 3;
    else if ((lead & 0xE0) == 0xC0) step = 2;
    return std::min(text.size(), index + step);
}

std::string EllipsizeToWidth(const std::string& text, float maxWidth) {
    if (ImGui::CalcTextSize(text.c_str()).x <= maxWidth) return text;
    const float ellipsisWidth = ImGui::CalcTextSize("...").x;
    if (maxWidth <= ellipsisWidth) return "...";
    std::string result;
    for (std::size_t i = 0; i < text.size();) {
        const std::size_t next = Utf8Step(text, i);
        const std::string candidate = text.substr(0, next) + "...";
        if (ImGui::CalcTextSize(candidate.c_str()).x > maxWidth) break;
        i = next;
        if (i >= text.size()) break;
        result = text.substr(0, i);
    }
    return result.empty() ? "..." : result + "...";
}

void DrawFolderGlyph(ImDrawList* drawList, ImVec2 center, float size, bool hovered,
                     ImU32 accent) {
    const float width = size;
    const float height = size * 0.70f;
    const float left = center.x - width * 0.5f;
    const float top = center.y - height * 0.5f;
    const float right = left + width;
    const float bottom = top + height;
    const ImVec4 accentColor = ImGui::ColorConvertU32ToFloat4(accent);
    const ImU32 tabColor = ImGui::ColorConvertFloat4ToU32(
        ImVec4(accentColor.x * 0.78f, accentColor.y * 0.78f, accentColor.z * 0.78f, 1.0f));
    const ImU32 bodyColor = accent;
    const ImU32 innerColor = ImGui::ColorConvertFloat4ToU32(
        ImVec4(std::min(1.0f, accentColor.x * (hovered ? 1.18f : 1.10f)),
               std::min(1.0f, accentColor.y * (hovered ? 1.18f : 1.10f)),
               std::min(1.0f, accentColor.z * (hovered ? 1.18f : 1.10f)), 1.0f));
    drawList->AddRectFilled(ImVec2(left + 3.0f, top + 5.0f),
                            ImVec2(right + 3.0f, bottom + 5.0f),
                            IM_COL32(0, 0, 0, 72), 7.0f);
    drawList->AddRectFilled(ImVec2(left, top + height * 0.10f),
                            ImVec2(left + width * 0.44f, top + height * 0.31f),
                            tabColor, 4.0f);
    drawList->AddRectFilled(ImVec2(left, top + height * 0.20f), ImVec2(right, bottom),
                            bodyColor, 7.0f);
    drawList->AddRectFilled(ImVec2(left + 5.0f, top + height * 0.30f),
                            ImVec2(right - 5.0f, bottom - 6.0f), innerColor, 5.0f);
    drawList->AddLine(ImVec2(left + 8.0f, top + height * 0.34f),
                      ImVec2(right - 9.0f, top + height * 0.34f),
                      IM_COL32(255, 244, 207, 90), 1.0f);
    drawList->AddRect(ImVec2(left, top + height * 0.20f), ImVec2(right, bottom),
                      IM_COL32(129, 87, 28, 150), 7.0f, 0, 1.0f);
}

void DrawMaterialGlyph(ImDrawList* drawList, ImVec2 center, float size, bool hovered) {
    const float radius = size * 0.36f;
    const ImVec2 ballCenter(center.x, center.y + size * 0.01f);
    drawList->AddCircleFilled(ImVec2(ballCenter.x + 2.0f, ballCenter.y + 4.0f),
                              radius + 1.0f, IM_COL32(0, 0, 0, 100), 40);
    const ImU32 bands[] = {
        IM_COL32(9, 39, 44, 255), IM_COL32(12, 69, 73, 255),
        IM_COL32(21, 112, 110, 255), IM_COL32(41, 166, 151, 255),
        IM_COL32(87, 215, 194, 255), IM_COL32(147, 244, 224, 255)
    };
    for (int i = 0; i < 6; ++i) {
        const float t = static_cast<float>(i) / 5.0f;
        drawList->AddCircleFilled(
            ImVec2(ballCenter.x - radius * 0.20f * t, ballCenter.y - radius * 0.22f * t),
            radius * (1.0f - 0.12f * t), bands[i], 48);
    }
    drawList->AddCircle(ballCenter, radius, hovered ? IM_COL32(143, 247, 228, 220)
                                                    : IM_COL32(111, 210, 195, 130),
                        48, 1.5f);
    drawList->AddCircleFilled(ImVec2(ballCenter.x - radius * 0.35f, ballCenter.y - radius * 0.40f),
                              radius * 0.15f, IM_COL32(227, 255, 250, 150), 20);
    drawList->AddCircleFilled(ImVec2(ballCenter.x + radius * 0.48f, ballCenter.y + radius * 0.30f),
                              radius * 0.08f, IM_COL32(255, 190, 100, 170), 16);
}

void DrawModelGlyph(ImDrawList* drawList, ImVec2 center, float size, bool hovered) {
    const float r = size * 0.33f;
    const float h = size * 0.22f;
    const ImU32 topFace = hovered ? IM_COL32(187, 158, 255, 255) : IM_COL32(167, 137, 244, 255);
    const ImU32 leftFace = IM_COL32(91, 72, 154, 255);
    const ImU32 rightFace = IM_COL32(124, 98, 195, 255);
    const ImVec2 top(center.x, center.y - r);
    const ImVec2 left(center.x - r, center.y - r * 0.30f);
    const ImVec2 right(center.x + r, center.y - r * 0.30f);
    const ImVec2 middle(center.x, center.y + r * 0.22f);
    const ImVec2 bottomLeft(center.x - r, center.y + r * 0.34f);
    const ImVec2 bottomRight(center.x + r, center.y + r * 0.34f);
    const ImVec2 bottom(center.x, center.y + r * 0.82f);
    drawList->AddQuadFilled(top, right, middle, left, topFace);
    drawList->AddQuadFilled(left, middle, bottom, bottomLeft, leftFace);
    drawList->AddQuadFilled(middle, right, bottomRight, bottom, rightFace);
    const ImU32 outline = IM_COL32(31, 24, 56, 190);
    drawList->AddLine(left, right, outline, 1.4f);
    drawList->AddLine(right, middle, outline, 1.4f);
    drawList->AddLine(middle, left, outline, 1.4f);
    drawList->AddLine(left, bottomLeft, outline, 1.4f);
    drawList->AddLine(bottomLeft, bottom, outline, 1.4f);
    drawList->AddLine(bottom, bottomRight, outline, 1.4f);
    drawList->AddLine(bottomRight, right, outline, 1.4f);
    drawList->AddLine(middle, bottom, outline, 1.4f);
    (void)h;
}

void DrawTextureGlyph(ImDrawList* drawList, ImVec2 center, float size) {
    const float half = size * 0.29f;
    const float left = center.x - half;
    const float top = center.y - half;
    const int cells = 4;
    const float cellSize = (half * 2.0f) / cells;
    for (int y = 0; y < cells; ++y) {
        for (int x = 0; x < cells; ++x) {
            const bool dark = ((x + y) & 1) == 0;
            drawList->AddRectFilled(
                ImVec2(left + x * cellSize, top + y * cellSize),
                ImVec2(left + (x + 1) * cellSize, top + (y + 1) * cellSize),
                dark ? IM_COL32(64, 83, 88, 255) : IM_COL32(117, 139, 143, 255));
        }
    }
    drawList->AddRect(ImVec2(left, top), ImVec2(left + half * 2.0f, top + half * 2.0f),
                      IM_COL32(255, 255, 255, 35), 3.0f);
}

void DrawProjectGlyph(ImDrawList* drawList, ImVec2 center, float size, bool hovered) {
    const float r = size * 0.32f;
    const ImVec2 top(center.x, center.y - r);
    const ImVec2 right(center.x + r, center.y);
    const ImVec2 bottom(center.x, center.y + r);
    const ImVec2 left(center.x - r, center.y);
    drawList->AddQuadFilled(top, right, bottom, left,
                            hovered ? IM_COL32(47, 109, 118, 255) : IM_COL32(36, 90, 98, 255));
    drawList->AddQuad(top, right, bottom, left,
                      IM_COL32(103, 235, 215, 220), 1.7f);
    drawList->AddCircleFilled(center, r * 0.18f, IM_COL32(177, 255, 240, 230), 16);
    drawList->AddLine(ImVec2(center.x - r * 0.45f, center.y - r * 0.45f), center,
                      IM_COL32(103, 235, 215, 180), 1.4f);
}

void DrawFunctionGlyph(ImDrawList* drawList, ImVec2 center, float size, bool hovered) {
    const float half = size * 0.30f;
    const ImVec2 min(center.x - half, center.y - half);
    const ImVec2 max(center.x + half, center.y + half);
    drawList->AddRectFilled(ImVec2(min.x + 2.0f, min.y + 3.0f),
                            ImVec2(max.x + 2.0f, max.y + 3.0f), IM_COL32(0, 0, 0, 65), 7.0f);
    drawList->AddRectFilled(min, max,
                            hovered ? IM_COL32(47, 72, 112, 255) : IM_COL32(38, 57, 90, 255),
                            7.0f);
    drawList->AddRect(min, max, IM_COL32(128, 173, 255, 225), 7.0f, 0, 1.6f);
    ImFont* font = ImGui::GetFont();
    const float fontSize = size * 0.48f;
    const ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, u8"ƒ");
    drawList->AddText(font, fontSize,
                      ImVec2(center.x - textSize.x * 0.5f, center.y - textSize.y * 0.5f),
                      IM_COL32(195, 216, 255, 245), u8"ƒ");
}

void DrawDocumentGlyph(ImDrawList* drawList, ImVec2 center, float size, bool shader) {
    const float width = size * 0.42f;
    const float height = size * 0.56f;
    const float left = center.x - width * 0.5f;
    const float top = center.y - height * 0.5f;
    const float right = left + width;
    const float bottom = top + height;
    drawList->AddRectFilled(ImVec2(left + 2.0f, top + 3.0f),
                            ImVec2(right + 2.0f, bottom + 3.0f),
                            IM_COL32(0, 0, 0, 70), 4.0f);
    drawList->AddRectFilled(ImVec2(left, top), ImVec2(right, bottom),
                            IM_COL32(213, 220, 221, 255), 4.0f);
    drawList->AddTriangleFilled(ImVec2(right - width * 0.28f, top),
                                ImVec2(right, top + height * 0.28f),
                                ImVec2(right - width * 0.28f, top + height * 0.28f),
                                IM_COL32(155, 170, 173, 255));
    const ImU32 lineColor = shader ? IM_COL32(45, 128, 126, 210)
                                   : IM_COL32(120, 138, 141, 210);
    drawList->AddLine(ImVec2(left + 6.0f, top + height * 0.40f),
                      ImVec2(right - 8.0f, top + height * 0.40f), lineColor, 1.3f);
    drawList->AddLine(ImVec2(left + 6.0f, top + height * 0.56f),
                      ImVec2(right - 13.0f, top + height * 0.56f), lineColor, 1.3f);
    drawList->AddLine(ImVec2(left + 6.0f, top + height * 0.72f),
                      ImVec2(right - 17.0f, top + height * 0.72f), lineColor, 1.3f);
}

bool BuildStarterMaterialGraph(MaterialGraph& graph, const std::string& materialName,
                               std::string* error) {
    graph.Settings().name = materialName.empty() ? "M_NewMaterial" : materialName;
    Node* output = graph.AddNodeByType("MaterialOutput", Vec2(320.0f, -80.0f), error);
    Node* baseColor = graph.AddNodeByType("VectorParameter", Vec2(-520.0f, -280.0f), error);
    Node* roughness = graph.AddNodeByType("ScalarParameter", Vec2(-520.0f, -80.0f), error);
    Node* metallic = graph.AddNodeByType("ScalarParameter", Vec2(-520.0f, 100.0f), error);
    if (!output || !baseColor || !roughness || !metallic) {
        return false;
    }

    baseColor->properties["parameterName"] = PropertyValue::StringValue("BaseColor");
    baseColor->properties["value"] = PropertyValue::Color4Value(0.10f, 0.55f, 0.65f, 1.0f);
    roughness->properties["parameterName"] = PropertyValue::StringValue("Roughness");
    roughness->properties["value"] = PropertyValue::Float(0.36f);
    metallic->properties["parameterName"] = PropertyValue::StringValue("Metallic");
    metallic->properties["value"] = PropertyValue::Float(0.0f);

    const auto connect = [&](Node* from, const char* outputName,
                             Node* to, const char* inputName) {
        const Pin* outputPin = from ? from->FindOutput(outputName) : nullptr;
        const Pin* inputPin = to ? to->FindInput(inputName) : nullptr;
        if (outputPin && inputPin) {
            graph.Connect(outputPin->id, inputPin->id, error);
        }
    };
    connect(baseColor, "Value", output, "Base Color");
    connect(roughness, "Value", output, "Roughness");
    connect(metallic, "Value", output, "Metallic");
    return true;
}

bool BuildStarterFunctionGraph(MaterialGraph& graph, const std::string& functionName,
                               std::string* error) {
    graph.Clear();
    graph.Settings().name = functionName.empty() ? "MF_NewFunction" : functionName;
    Node* input = graph.AddNodeByType("FunctionInput", Vec2(-260.0f, 0.0f), error);
    Node* output = graph.AddNodeByType("FunctionOutput", Vec2(280.0f, 0.0f), error);
    if (!input || !output) return false;
    if (input->FindOutput("Value") && output->FindInput("Value")) {
        graph.Connect(input->FindOutput("Value")->id, output->FindInput("Value")->id, error);
    }
    return true;
}

} // namespace

Application::Application(ApplicationOptions options)
    : m_Options(std::move(options)) {}

Application::~Application() {
    Shutdown();
}

void Application::LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig config;
    config.OversampleH = 2;
    config.OversampleV = 2;
    config.PixelSnapH = true;
    config.RasterizerMultiply = 1.05f;

    const char* regularPaths[] = {
        "C:/Windows/Fonts/segoeui.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/System/Library/Fonts/SFNS.ttf"
    };
    for (const char* path : regularPaths) {
        if (PathUtil::Exists(PathUtil::FromUtf8(path))) {
            ImFont* font = io.Fonts->AddFontFromFileTTF(path, Ui::S(16.0f), &config);
            if (font) {
                io.FontDefault = font;
                break;
            }
        }
    }
    if (io.Fonts->Fonts.empty()) {
        ImGui::GetIO().Fonts->AddFontDefault();
        io.FontDefault = io.Fonts->Fonts.back();
    }

    // Merge a CJK fallback for user-provided project and file names. A limited
    // common-glyph range keeps the atlas small while still covering the names
    // users type in the New Project dialog.
    ImFontConfig cjkConfig;
    cjkConfig.MergeMode = true;
    cjkConfig.OversampleH = 1;
    cjkConfig.OversampleV = 1;
    cjkConfig.PixelSnapH = true;
    cjkConfig.GlyphRanges = CjkGlyphRanges(io.Fonts);
    const char* cjkRegular = "C:/Windows/Fonts/msyh.ttc";
    if (PathUtil::Exists(PathUtil::FromUtf8(cjkRegular))) {
        io.Fonts->AddFontFromFileTTF(cjkRegular, Ui::S(16.0f), &cjkConfig);
    }

    const char* boldPaths[] = {
        "C:/Windows/Fonts/segoeuib.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
    };
    for (const char* path : boldPaths) {
        if (PathUtil::Exists(PathUtil::FromUtf8(path))) {
            m_BoldFont = io.Fonts->AddFontFromFileTTF(path, Ui::S(20.0f), &config);
            if (m_BoldFont) break;
        }
    }
    const char* cjkBold = "C:/Windows/Fonts/msyhbd.ttc";
    if (m_BoldFont && PathUtil::Exists(PathUtil::FromUtf8(cjkBold))) {
        ImFontConfig boldCjkConfig = cjkConfig;
        io.Fonts->AddFontFromFileTTF(cjkBold, Ui::S(20.0f), &boldCjkConfig);
    }

    const char* monoPaths[] = {
        "C:/Windows/Fonts/CascadiaMono.ttf",
        "C:/Windows/Fonts/consola.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
    };
    for (const char* path : monoPaths) {
        if (PathUtil::Exists(PathUtil::FromUtf8(path))) {
            m_MonoFont = io.Fonts->AddFontFromFileTTF(path, Ui::S(13.0f), &config);
            if (m_MonoFont) break;
        }
    }
    if (m_MonoFont && PathUtil::Exists(PathUtil::FromUtf8(cjkRegular))) {
        ImFontConfig monoCjkConfig = cjkConfig;
        monoCjkConfig.MergeMode = true;
        io.Fonts->AddFontFromFileTTF(cjkRegular, Ui::S(13.0f), &monoCjkConfig);
    }

    ImFontConfig smallConfig;
    smallConfig.OversampleH = 2;
    smallConfig.OversampleV = 2;
    smallConfig.PixelSnapH = true;
    for (const char* path : regularPaths) {
        if (PathUtil::Exists(PathUtil::FromUtf8(path))) {
            m_SmallFont = io.Fonts->AddFontFromFileTTF(path, Ui::S(12.0f), &smallConfig);
            if (m_SmallFont) break;
        }
    }
    if (m_SmallFont && PathUtil::Exists(PathUtil::FromUtf8(cjkRegular))) {
        ImFontConfig smallCjkConfig = cjkConfig;
        smallCjkConfig.MergeMode = true;
        io.Fonts->AddFontFromFileTTF(cjkRegular, Ui::S(12.0f), &smallCjkConfig);
    }
}

void Application::PushBoldFont() {
    if (m_BoldFont) ImGui::PushFont(m_BoldFont);
}

void Application::PopBoldFont() {
    if (m_BoldFont) ImGui::PopFont();
}

void Application::PushMonoFont() {
    if (m_MonoFont) ImGui::PushFont(m_MonoFont);
}

void Application::PopMonoFont() {
    if (m_MonoFont) ImGui::PopFont();
}

bool Application::Initialize() {
    glfwSetErrorCallback([](int code, const char* description) {
        std::cerr << "GLFW error " << code << ": " << description << "\n";
        if (description) {
            g_GlfwErrors.push_back(std::string("GLFW error ") + std::to_string(code) + ": " +
                                   description);
        }
    });

    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW.\n";
        return false;
    }
    m_GlfwInitialized = true;

    m_Window = CreateBestContextWindow(1600, 980);
    if (!m_Window) {
        std::cerr << "Failed to create the MaterialForge window. OpenGL 3.3 is required.\n";
        Shutdown();
        return false;
    }

    glfwSetWindowUserPointer(m_Window, this);
    glfwSetDropCallback(m_Window, &Application::FileDropCallback);

    glfwMakeContextCurrent(m_Window);
    glfwSwapInterval(1);

    m_SettingsPath = PathUtil::UserConfigDirectory() / "settings.json";
    LoadUiSettings();
    m_UiScale = m_RequestedUiScale > 0.0f ? std::clamp(m_RequestedUiScale, 0.75f, 2.5f)
                                          : ResolveAutoUiScale();
    m_FileBarHeight = std::clamp(m_Settings.fileBarHeight * m_UiScale,
                                 190.0f * m_UiScale, 420.0f * m_UiScale);
    ApplyUiScale(m_UiScale);
    Ui::SetTheme(m_Theme == "midnight" ? Ui::Theme::Midnight : Ui::Theme::Graphite);

    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    if (monitor) {
        int workX = 0;
        int workY = 0;
        int workW = 0;
        int workH = 0;
        glfwGetMonitorWorkarea(monitor, &workX, &workY, &workW, &workH);
        if (workW > 0 && workH > 0) {
            const int desiredW = std::min(workW, std::max(1180, static_cast<int>(1600.0f * m_UiScale)));
            const int desiredH = std::min(workH, std::max(760, static_cast<int>(980.0f * m_UiScale)));
            glfwSetWindowSize(m_Window, desiredW, desiredH);
            glfwSetWindowSizeLimits(m_Window,
                                    std::min(desiredW, static_cast<int>(1180.0f * m_UiScale)),
                                    std::min(desiredH, static_cast<int>(760.0f * m_UiScale)),
                                    GLFW_DONT_CARE, GLFW_DONT_CARE);
            glfwSetWindowPos(m_Window,
                             workX + std::max(0, (workW - desiredW) / 2),
                             workY + std::max(0, (workH - desiredH) / 2));
        }
    }

    if (!gladLoadGL(reinterpret_cast<GLADloadfunc>(glfwGetProcAddress))) {
        std::cerr << "Failed to load OpenGL functions.\n";
        Shutdown();
        return false;
    }

    const char* glVersionText = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    m_GlVersionString = glVersionText ? glVersionText : "unknown";
    GLint contextMajor = 0;
    GLint contextMinor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &contextMajor);
    glGetIntegerv(GL_MINOR_VERSION, &contextMinor);
    m_GlMajor = contextMajor > 0 ? contextMajor : 3;
    m_GlMinor = contextMinor > 0 ? contextMinor : 3;
    if (m_GlMajor > 4 || (m_GlMajor == 4 && m_GlMinor >= 6)) {
        m_GlslVersion = 460;
    } else if (m_GlMajor == 4 && m_GlMinor >= 3) {
        m_GlslVersion = 430;
    } else if (m_GlMajor == 4 && m_GlMinor >= 1) {
        m_GlslVersion = 410;
    } else {
        m_GlslVersion = 330;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    m_ImGuiContextCreated = true;
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    Ui::ApplyStyle();
    LoadFonts();

    if (!ImGui_ImplGlfw_InitForOpenGL(m_Window, true)) {
        std::cerr << "Failed to initialize the ImGui GLFW backend.\n";
        Shutdown();
        return false;
    }
    m_ImGuiGlfwInitialized = true;
    std::string imguiGlslVersion = "#version " + std::to_string(m_GlslVersion);
    if (m_GlslVersion <= 330) {
        imguiGlslVersion += " core";
    }
    if (!ImGui_ImplOpenGL3_Init(imguiGlslVersion.c_str())) {
        std::cerr << "Failed to initialize the ImGui OpenGL backend.\n";
        Shutdown();
        return false;
    }
    m_ImGuiOpenGLInitialized = true;

    const fs::path sourceRoot = PathUtil::FindProjectRoot();
    m_ContentRoot = sourceRoot.empty() ? PathUtil::ExecutableDirectory() : sourceRoot;

    m_PluginManager.SetLogHandler([this](int level, const std::string& message) {
        LogMessage(level >= 2 ? LogLevel::Warning : LogLevel::Info, message);
    });
    m_PluginManager.SetRootPath(PathUtil::UserConfigDirectory() / "plugins");
    m_PluginManager.Refresh();

    const fs::path iconPath = m_ContentRoot / "assets" / "MaterialForge.png";
    if (PathUtil::Exists(iconPath)) {
        int iconWidth = 0;
        int iconHeight = 0;
        int iconChannels = 0;
        unsigned char* pixels = stbi_load(PathUtil::ToUtf8(iconPath).c_str(), &iconWidth,
                                          &iconHeight, &iconChannels, 4);
        if (pixels && iconWidth > 0 && iconHeight > 0) {
            GLFWimage icon{iconWidth, iconHeight, pixels};
            glfwSetWindowIcon(m_Window, 1, &icon);
        }
        if (pixels) stbi_image_free(pixels);
    }

    m_RecentProjects.SetStorePath(PathUtil::UserConfigDirectory() / "recent_projects.json");
    std::string recentError;
    m_RecentProjectsLoaded = m_RecentProjects.Load(&recentError);
    if (!m_RecentProjectsLoaded && !recentError.empty()) {
        std::cerr << "Could not load recent projects: " << recentError << "\n";
    }

    m_GraphView = std::make_unique<NodeGraphView>(m_Graph);
    m_GraphView->Initialize();

    std::string desiredApi = m_Settings.renderApi;
    if (!m_Options.rendererOverride.empty()) {
        desiredApi = m_Options.rendererOverride;
    }
    m_PreviewRenderer.SetPreferredBackend(ParseRenderApi(desiredApi));
    std::string previewError;
    if (!m_PreviewRenderer.Initialize(&previewError)) {
        std::cerr << "Preview renderer initialization failed: " << previewError << "\n";
        Shutdown();
        return false;
    }
    if (m_PreviewRenderer.FellBackToOpenGL()) {
        LogMessage(LogLevel::Warning,
                   std::string("Requested preview backend '") + desiredApi +
                       "' could not start; fell back to OpenGL. " +
                       m_PreviewRenderer.LastBackendError());
    }
    ApplyPreviewSceneSettings();

    bool loaded = false;
    std::string loadError;
    if (!m_Options.projectPath.empty()) {
        loaded = OpenProjectFile(m_Options.projectPath, &loadError);
        if (!loaded) {
            std::cerr << "Could not open startup project: " << loadError << "\n";
        }
    }

    if (!loaded && !m_Options.openPath.empty()) {
        loaded = LoadDocument(m_Options.openPath, &loadError);
        if (!loaded) {
            std::cerr << "Could not open startup document: " << loadError << "\n";
        }
    }

    if (!loaded) {
        // Start with an empty workspace instead of silently loading the bundled
        // example material. The preview stays empty until the user opens or
        // creates a material.
        m_Graph.Clear();
        m_DocumentPath.clear();
        m_DocumentKind = DocumentKind::Material;
        m_GraphView->OnGraphReplaced();
        m_LastContentRevision = 0;
        m_ForceShaderRefresh = true;
    }

    m_StartTime = glfwGetTime();
    m_LastContentRevision = 0;
    m_ForceShaderRefresh = true;
    UpdateMaterialIfNeeded();

    m_StatusMessage = Tr("Ready");
    m_StatusError = false;
    m_StatusExpiresAt = m_StartTime + 3600.0;
    if (m_Project.IsOpen() && m_Project.IsBroken()) {
        SetStatus(Tr("Project is open but broken: some marked files are missing."), true);
        m_StatusExpiresAt = m_StartTime + 3600.0;
    }

    if (!m_Options.capturePath.empty()) {
        m_CapturePath = m_Options.capturePath;
        m_CaptureFrame = std::max(1, m_Options.captureFrame);
    }

    UpdateWindowTitle();
    if (m_Options.openPreferences) m_OpenPreferences = true;
    if (m_Options.openConsole) {
        m_ConsoleVisible = true;
        m_ConsoleFocusInput = true;
    }
    if (m_Options.collapseLibrary) m_NodeLibraryCollapsed = true;
    if (m_Options.detachPreview) {
        m_PreviewDetached = true;
        m_DetachedPreviewPlacementPending = true;
        m_DetachedPreviewWidth = 680.0f * std::max(0.75f, m_UiScale);
        m_DetachedPreviewHeight = 500.0f * std::max(0.75f, m_UiScale);
        m_DetachedPreviewX = Ui::S(180.0f);
        m_DetachedPreviewY = Ui::S(110.0f);
    }

    CheckAutosaveRecovery();

    LogMessage(LogLevel::Info, "MaterialForge initialized.");
    LogMessage(LogLevel::Info, "OpenGL context: " + m_GlVersionString);
    const char* rendererInfo = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    if (rendererInfo && rendererInfo[0]) {
        LogMessage(LogLevel::Info, std::string("OpenGL renderer: ") + rendererInfo);
    }
    LogMessage(LogLevel::Info, std::string("Preview backend: ") +
                                   m_PreviewRenderer.BackendName() + " / " +
                                   m_PreviewRenderer.BackendDetails());
    if (m_Project.IsOpen()) {
        LogMessage(LogLevel::Info, "Project: " + PathUtil::ToUtf8(m_Project.ProjectFilePath()));
    }
    if (m_Project.IsOpen() && m_Project.IsBroken()) {
        LogMessage(LogLevel::Warning,
                   "The project is broken; some marked files are missing.");
    }

    std::cout << "MaterialForge initialized.\n"
              << "  Renderer: " << (rendererInfo ? rendererInfo : "unknown") << "\n"
              << "  GL version: " << m_GlVersionString << "\n"
              << "  Preview backend: " << m_PreviewRenderer.BackendName() << "\n"
              << "  Content root: " << PathUtil::ToUtf8(m_ContentRoot) << "\n"
              << "  Project: "
              << (m_Project.IsOpen() ? PathUtil::ToUtf8(m_Project.ProjectFilePath())
                                     : std::string("(none)"))
              << "\n"
              << "  Document: "
              << (m_DocumentPath.empty() ? std::string("(unsaved)") : PathUtil::ToUtf8(m_DocumentPath))
              << "\n";

    if (!m_Options.dumpShaderPath.empty()) {
        const std::string base = m_Options.dumpShaderPath;
        std::ofstream glslStream(base + ".glsl", std::ios::binary | std::ios::trunc);
        if (glslStream) {
            glslStream << m_GeneratedShader.fragmentSource;
        }
        const HlslPreviewShader hlsl = HlslPreviewShaderGenerator::Generate(m_GeneratedShader);
        if (hlsl.success) {
            std::ofstream hlslStream(base + ".hlsl", std::ios::binary | std::ios::trunc);
            if (hlslStream) {
                hlslStream << hlsl.source;
            }
        }
        std::cout << "Shader sources written to " << base << ".glsl/.hlsl\n";
        m_WantsExit = true;
    }

    m_Initialized = true;
    StartMcpServerIfEnabled();
    return true;
}

void Application::Run() {
    if (!m_Initialized) {
        return;
    }

    while (!glfwWindowShouldClose(m_Window) && !m_WantsExit) {
        glfwPollEvents();
        ProcessMcpPending();
        if (glfwGetWindowAttrib(m_Window, GLFW_ICONIFIED)) {
            glfwWaitEventsTimeout(0.05);
            continue;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ProcessDroppedFiles();
        m_TimeSeconds = glfwGetTime() - m_StartTime;
        if (!m_Options.startupCommand.empty()) {
            const std::string command = m_Options.startupCommand;
            m_Options.startupCommand.clear();
            ExecuteConsoleCommand(command);
        }
        UpdateMaterialIfNeeded();
        HandleShortcuts();
        DrawUi();
        UpdateUndoAndAutosave();
        UpdateWindowTitle();

        ImGui::Render();

        int framebufferWidth = 0;
        int framebufferHeight = 0;
        glfwGetFramebufferSize(m_Window, &framebufferWidth, &framebufferHeight);
        if (framebufferWidth > 0 && framebufferHeight > 0) {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, framebufferWidth, framebufferHeight);
            glClearColor(0.031f, 0.043f, 0.054f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        }

        ++m_FrameCounter;
        if (!m_CapturePath.empty() && !m_CaptureWritten && m_FrameCounter >= m_CaptureFrame) {
            m_CaptureWritten = CaptureScreenshot(m_CapturePath);
            glfwSetWindowShouldClose(m_Window, GLFW_TRUE);
        }

        glfwSwapBuffers(m_Window);
    }
}

void Application::Shutdown() {
    CloseTextureViewerTexture();
    StopMcpServer();
    m_PluginManager.UnloadAll();
    if (m_MaterialThumbnailRenderer) {
        m_MaterialThumbnailRenderer->Shutdown();
        m_MaterialThumbnailRenderer.reset();
    }
    if (m_CapturePath.empty() &&
        (!m_AutosaveRecoveryPending || !m_Settings.autosaveEnabled)) {
        DeleteAutosave();
    }
    if (m_RecentProjectsLoaded && !m_RecentProjects.StorePath().empty()) {
        std::string recentError;
        m_RecentProjects.Save(&recentError);
    }
    if (m_Window) {
        ClearThumbnailCache();
    }
    if (m_GraphView) {
        m_GraphView->Shutdown();
        m_GraphView.reset();
    }
    if (m_Window) {
        m_PreviewRenderer.Shutdown();
    }
    if (m_ImGuiOpenGLInitialized) {
        ImGui_ImplOpenGL3_Shutdown();
        m_ImGuiOpenGLInitialized = false;
    }
    if (m_ImGuiGlfwInitialized) {
        ImGui_ImplGlfw_Shutdown();
        m_ImGuiGlfwInitialized = false;
    }
    if (m_ImGuiContextCreated) {
        ImGui::DestroyContext();
        m_ImGuiContextCreated = false;
    }
    if (m_Window) {
        glfwDestroyWindow(m_Window);
        m_Window = nullptr;
    }
    if (m_GlfwInitialized) {
        glfwTerminate();
        m_GlfwInitialized = false;
    }
    m_Initialized = false;
}

void Application::UpdateMaterialIfNeeded() {
    if (m_DocumentKind == DocumentKind::Function) {
        m_ShaderGenerated = false;
        m_ShaderCompiled = false;
        m_ShaderError.clear();
        m_LastContentRevision = m_Graph.ContentRevision();
        m_ForceShaderRefresh = false;
        return;
    }
    if (m_NodePreviewActive) {
        if (m_ForceShaderRefresh ||
            m_Graph.ContentRevision() != m_NodePreviewRevision) {
            RefreshNodePreview();
        }
        return;
    }
    if (!m_Graph.FindMaterialOutput()) {
        m_ShaderGenerated = false;
        m_ShaderCompiled = false;
        m_ShaderError.clear();
        m_GeneratedShader = GeneratedShader{};
        m_LastContentRevision = m_Graph.ContentRevision();
        m_ForceShaderRefresh = false;
        return;
    }
    if (!m_ForceShaderRefresh && m_Graph.ContentRevision() == m_LastContentRevision) {
        return;
    }
    m_PreviewNeedsRender = true;
    m_LastContentRevision = m_Graph.ContentRevision();
    m_ForceShaderRefresh = false;

    const fs::path functionBase =
        m_Project.IsOpen() ? m_Project.RootPath() : m_DocumentPath.parent_path();
    m_GeneratedShader = GlslGenerator().Generate(m_Graph, m_GlslVersion, functionBase,
                                                 m_PreviewBuffer, PreviewLighting());
    m_ShaderGenerated = m_GeneratedShader.success;

    if (!m_ShaderGenerated) {
        m_ShaderCompiled = false;
        m_ShaderError = m_GeneratedShader.diagnostic;
        m_PreviewRenderer.SetMaterial(m_GeneratedShader, nullptr);
        if (m_LastShaderDiagnosticKey != m_GeneratedShader.diagnostic) {
            m_LastShaderDiagnosticKey = m_GeneratedShader.diagnostic;
            LogMessage(LogLevel::Error, m_GeneratedShader.diagnostic);
        }
        SetStatus(Tr("Shader generation failed. Open the GLSL tab for details."), true);
        return;
    }

    std::string warningKey;
    for (const std::string& warning : m_GeneratedShader.warnings) {
        warningKey += warning;
        warningKey.push_back('\n');
    }
    if (!warningKey.empty() && warningKey != m_LastShaderDiagnosticKey) {
        m_LastShaderDiagnosticKey = warningKey;
        for (const std::string& warning : m_GeneratedShader.warnings) {
            LogMessage(LogLevel::Warning, warning);
        }
    }
    if (!warningKey.empty()) {
        m_LastShaderDiagnosticKey = warningKey;
    }

    GeneratedShader runtimeShader = m_GeneratedShader;
    for (ShaderTextureBinding& texture : runtimeShader.textures) {
        if (texture.assetPath.empty()) {
            continue;
        }
        fs::path resolved;
        if (m_Project.IsOpen()) {
            const fs::path projectCandidate = m_Project.ResolveRelative(texture.assetPath);
            std::error_code ec;
            if (!projectCandidate.empty() && fs::is_regular_file(projectCandidate, ec)) {
                resolved = projectCandidate;
            }
        }
        if (resolved.empty()) {
            resolved = PathUtil::ResolveResource(texture.assetPath);
        }
        texture.assetPath = PathUtil::ToUtf8(resolved);
    }

    std::string compileError;
    m_ShaderCompiled = m_PreviewRenderer.SetMaterial(runtimeShader, &compileError);
    m_ShaderError = m_ShaderCompiled ? std::string() : compileError;
    if (!m_ShaderCompiled) {
        if (m_LastShaderDiagnosticKey != m_ShaderError) {
            m_LastShaderDiagnosticKey = m_ShaderError;
            LogMessage(LogLevel::Error, m_ShaderError);
        }
        SetStatus(Tr("Shader compile failed. Open the GLSL tab for details."), true);
    } else {
        const std::string& textureDiagnostics = m_PreviewRenderer.TextureDiagnostics();
        if (!textureDiagnostics.empty() &&
            textureDiagnostics != m_LastTextureDiagnosticKey) {
            m_LastTextureDiagnosticKey = textureDiagnostics;
            LogMessage(LogLevel::Warning, textureDiagnostics);
        }
    }
}

void Application::DrawUi() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Ui::S(12.0f), Ui::S(8.0f)));

    const ImGuiWindowFlags rootFlags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_MenuBar;

    if (ImGui::Begin("##MaterialForgeRoot", nullptr, rootFlags)) {
        DrawMenuBar();
        DrawToolbar();
        DrainExternalMessages();
        m_PreviewRenderedThisFrame = false;

        const float availableHeight = ImGui::GetContentRegionAvail().y;
        const float gap = Ui::S(6.0f);
        float consoleHeight = 0.0f;
        if (m_ConsoleVisible) {
            consoleHeight = std::clamp(Ui::S(m_ConsoleHeight),
                                       Ui::S(130.0f),
                                       std::max(Ui::S(130.0f), availableHeight * 0.46f));
        }
        const float bodyHeight = std::max(
            Ui::S(240.0f),
            availableHeight - Ui::S(kStatusHeight) - Ui::S(4.0f) -
                (m_ConsoleVisible ? consoleHeight + gap : 0.0f));
        DrawBody(bodyHeight);
        DrawTextureViewer();
        DrawDocumentDialogs();
        if (m_ConsoleVisible) {
            DrawConsolePanel(consoleHeight);
        }
        DrawStatusBar();

        DrawProjectDialogs();
        DrawAboutPopup();
        DrawPreferences();
        const bool fileBrowserWasActive = m_FileBrowser.IsActive();
        const std::optional<std::string> browserResult = m_FileBrowser.Draw();
        if (browserResult) {
            HandleFileBrowserResult(*browserResult);
        } else if (m_PendingCloseSaveAs && fileBrowserWasActive &&
                   !m_FileBrowser.IsActive()) {
            m_PendingCloseSaveAs = false;
            m_OpenCloseDocumentModal = true;
        }
        if (m_ReturnToNewProjectAfterBrowse && !m_FileBrowser.IsActive()) {
            m_ReturnToNewProjectAfterBrowse = false;
            m_OpenNewProjectModal = true;
            m_NewProjectModalPending = true;
        }
    }

    ImGui::PopStyleVar(3);
    ImGui::End();

    if (m_PreviewDetached) {
        DrawDetachedPreviewWindow();
    }
    DrawAutosaveRecoveryModal();
}

void Application::LoadUiSettings() {
    AppSettingsStore store(m_SettingsPath);
    const bool hadSettings = PathUtil::Exists(m_SettingsPath);
    std::string error;
    if (!store.Load(m_Settings, &error) && !error.empty()) {
        std::cerr << "Could not load UI settings: " << error << "\n";
    }

    Language language = DetectSystemLanguage();
    if (m_Settings.language == "zh-CN" || m_Settings.language == "zh") {
        language = Language::Chinese;
    } else if (m_Settings.language == "en") {
        language = Language::English;
    }
    Localization::Get().SetLanguage(language);
    m_Settings.language = Localization::Get().LanguageCode();
    m_Theme = m_Settings.theme.empty() ? "graphite" : m_Settings.theme;
    m_Settings.theme = m_Theme;
    m_RequestedUiScale = std::clamp(m_Settings.uiScale, 0.0f, 2.5f);
    m_NodeLibraryCollapsed = m_Settings.nodeLibraryCollapsed;
    m_ConsoleVisible = m_Settings.consoleVisible;
    m_ConsoleHeight = std::clamp(m_Settings.consoleHeight, 110.0f, 520.0f);
    m_PreviewDetached = m_Settings.previewDetached;
    m_PreviewLive = m_Settings.previewLive;
    m_PreviewNeedsRender = true;
    m_DetachedPreviewX = m_Settings.previewWindowX;
    m_DetachedPreviewY = m_Settings.previewWindowY;
    m_DetachedPreviewWidth = std::clamp(m_Settings.previewWindowWidth, 320.0f, 1600.0f);
    m_DetachedPreviewHeight = std::clamp(m_Settings.previewWindowHeight, 260.0f, 1200.0f);
    m_DetachedPreviewPlacementPending = m_PreviewDetached;
    if (!hadSettings) {
        SaveUiSettings();
    }
}

void Application::SaveUiSettings() {
    m_Settings.language = Localization::Get().LanguageCode();
    m_Settings.fileBarHeight = m_FileBarHeight / std::max(0.1f, m_UiScale);
    m_Settings.theme = m_Theme;
    m_Settings.uiScale = m_RequestedUiScale;
    m_Settings.nodeLibraryCollapsed = m_NodeLibraryCollapsed;
    m_Settings.consoleVisible = m_ConsoleVisible;
    m_Settings.consoleHeight = m_ConsoleHeight;
    m_Settings.previewDetached = m_PreviewDetached;
    m_Settings.previewLive = m_PreviewLive;
    m_Settings.previewWindowX = m_DetachedPreviewX;
    m_Settings.previewWindowY = m_DetachedPreviewY;
    m_Settings.previewWindowWidth = m_DetachedPreviewWidth;
    m_Settings.previewWindowHeight = m_DetachedPreviewHeight;
    AppSettingsStore store(m_SettingsPath);
    std::string error;
    if (!store.Save(m_Settings, &error)) {
        std::cerr << "Could not save UI settings: " << error << "\n";
    }
}

void Application::SetTheme(const std::string& theme) {
    m_Theme = theme == "midnight" ? "midnight" : "graphite";
    m_Settings.theme = m_Theme;
    Ui::SetTheme(m_Theme == "midnight" ? Ui::Theme::Midnight : Ui::Theme::Graphite);
    Ui::ApplyStyle();
    SaveUiSettings();
    if (m_GraphView) {
        m_GraphView->OnGraphReplaced();
    }
}

void Application::ApplyUiScale(float scale) {
    m_UiScale = std::clamp(scale, 0.75f, 2.5f);
    Ui::SetScale(m_UiScale);
}

float Application::ResolveAutoUiScale() const {
    float scale = 1.0f;
    if (m_Window) {
        float sx = 1.0f;
        float sy = 1.0f;
        glfwGetWindowContentScale(m_Window, &sx, &sy);
        scale = std::max(1.0f, std::max(sx, sy));
    }
    if (scale < 1.01f) {
        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        int workX = 0;
        int workY = 0;
        int workW = 0;
        int workH = 0;
        if (monitor) {
            glfwGetMonitorWorkarea(monitor, &workX, &workY, &workW, &workH);
        }
        if (workH >= 2000) {
            scale = 1.5f;
        } else if (workH >= 1400) {
            scale = 1.25f;
        }
    }
    return std::clamp(scale, 1.0f, 2.0f);
}

void Application::DrawThemeMenu() {
    if (!ImGui::BeginMenu(Tr("Appearance"))) {
        return;
    }
    const bool graphite = m_Theme != "midnight";
    if (ImGui::MenuItem(Tr("Graphite Gray"), nullptr, graphite)) {
        SetTheme("graphite");
    }
    if (ImGui::MenuItem(Tr("Midnight Blue"), nullptr, !graphite)) {
        SetTheme("midnight");
    }
    ImGui::Separator();
    ImGui::TextDisabled("%s", Tr("UI Scale (restart to apply)"));
    struct ScaleOption {
        const char* label;
        float value;
    };
    const ScaleOption options[] = {
        {"Auto", 0.0f},
        {"100%", 1.0f},
        {"125%", 1.25f},
        {"150%", 1.5f},
        {"175%", 1.75f},
        {"200%", 2.0f},
    };
    for (const ScaleOption& option : options) {
        const bool selected = std::abs(m_RequestedUiScale - option.value) < 0.001f;
        if (ImGui::MenuItem(Tr(option.label), nullptr, selected)) {
            m_RequestedUiScale = option.value;
            SaveUiSettings();
            SetStatus(Tr("UI scale will apply after restart."));
        }
    }
    ImGui::EndMenu();
}

void Application::SetLanguage(Language language) {
    if (Localization::Get().CurrentLanguage() == language) {
        return;
    }
    Localization::Get().SetLanguage(language);
    SaveUiSettings();
    SetStatus(Tr("Language switched."));
}

void Application::DrawLanguageMenu() {
    if (!ImGui::BeginMenu(Tr("Language"))) {
        return;
    }
    const bool english = Localization::Get().CurrentLanguage() == Language::English;
    if (ImGui::MenuItem(Tr("English"), nullptr, english)) {
        SetLanguage(Language::English);
    }
    if (ImGui::MenuItem(Tr("Chinese"), nullptr, !english)) {
        SetLanguage(Language::Chinese);
    }
    ImGui::EndMenu();
}

void Application::DrawMenuBar() {
    if (!ImGui::BeginMenuBar()) {
        return;
    }
    if (ImGui::BeginMenu(Tr("File"))) {
        if (ImGui::MenuItem(Tr("New Material"), "Ctrl+N")) RequestNewDocument();
        if (ImGui::MenuItem(Tr("Open..."), "Ctrl+O")) RequestOpenDialog();
        ImGui::Separator();
        if (ImGui::MenuItem(Tr("Save Project"), "Ctrl+S")) SaveProject();
        if (ImGui::MenuItem(Tr("Save As..."), "Ctrl+Shift+S")) RequestSave(true);
        if (ImGui::MenuItem(Tr("Export UE5 JSON..."), "Ctrl+E", false,
                            m_DocumentKind == DocumentKind::Material)) {
            RequestExportDialog();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(Tr("Exit"))) {
            glfwSetWindowShouldClose(m_Window, GLFW_TRUE);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(Tr("Edit"))) {
        if (ImGui::MenuItem(Tr("Undo"), "Ctrl+Z", false, CanUndo())) {
            Undo();
        }
        if (ImGui::MenuItem(Tr("Redo"), "Ctrl+Y", false, CanRedo())) {
            Redo();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(Tr("Plugin Manager..."))) {
            m_OpenPluginManager = true;
            m_PluginManagerPending = true;
        }
        if (ImGui::MenuItem(Tr("Select All Nodes"), "Ctrl+A")) {
            if (m_GraphView) m_GraphView->SelectAllNodes();
        }
        const int selectedNodes = m_GraphView ? m_GraphView->SelectedNodeCount() : 0;
        if (ImGui::MenuItem(Tr("Add Comment"))) {
            if (m_GraphView) m_GraphView->AddCommentAtViewCenter();
        }
        if (ImGui::MenuItem(Tr("Group Selected"), nullptr, false, selectedNodes > 0)) {
            if (m_GraphView) m_GraphView->GroupSelectedNodes();
        }
        if (ImGui::MenuItem(Tr("Collapse to Function..."), nullptr, false,
                            selectedNodes > 0)) {
            CollapseSelectionToFunction();
        }
        if (ImGui::BeginMenu(Tr("Align"), selectedNodes >= 2)) {
            if (ImGui::MenuItem(Tr("Align Left"))) {
                m_GraphView->AlignSelected(NodeGraphView::AlignMode::Left);
            }
            if (ImGui::MenuItem(Tr("Align Center"))) {
                m_GraphView->AlignSelected(NodeGraphView::AlignMode::CenterHorizontal);
            }
            if (ImGui::MenuItem(Tr("Align Right"))) {
                m_GraphView->AlignSelected(NodeGraphView::AlignMode::Right);
            }
            ImGui::Separator();
            if (ImGui::MenuItem(Tr("Align Top"))) {
                m_GraphView->AlignSelected(NodeGraphView::AlignMode::Top);
            }
            if (ImGui::MenuItem(Tr("Align Middle"))) {
                m_GraphView->AlignSelected(NodeGraphView::AlignMode::CenterVertical);
            }
            if (ImGui::MenuItem(Tr("Align Bottom"))) {
                m_GraphView->AlignSelected(NodeGraphView::AlignMode::Bottom);
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(Tr("Distribute Horizontally"), nullptr, false,
                            selectedNodes >= 3)) {
            m_GraphView->DistributeSelected(true);
        }
        if (ImGui::MenuItem(Tr("Distribute Vertically"), nullptr, false,
                            selectedNodes >= 3)) {
            m_GraphView->DistributeSelected(false);
        }
        if (ImGui::BeginMenu(Tr("Bookmarks"))) {
            if (ImGui::MenuItem(Tr("Add Bookmark from Selection"), nullptr, false,
                                selectedNodes > 0)) {
                m_GraphView->AddBookmarkFromSelection();
            }
            const auto& bookmarks = m_GraphView->Bookmarks();
            if (!bookmarks.empty()) {
                ImGui::Separator();
                for (std::size_t i = 0; i < bookmarks.size(); ++i) {
                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::MenuItem(bookmarks[i].name.c_str())) {
                        m_GraphView->GoToBookmark(i);
                    }
                    ImGui::PopID();
                }
                if (ImGui::BeginMenu(Tr("Remove Bookmark"))) {
                    for (std::size_t i = 0; i < bookmarks.size(); ++i) {
                        ImGui::PushID(static_cast<int>(i));
                        if (ImGui::MenuItem(bookmarks[i].name.c_str())) {
                            m_GraphView->RemoveBookmark(i);
                            ImGui::PopID();
                            break;
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndMenu();
                }
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(Tr("Project"))) {
        if (ImGui::MenuItem(Tr("New Project..."))) RequestNewProject();
        if (ImGui::MenuItem(Tr("Open Project..."))) RequestOpenProject();
        DrawRecentProjectsMenu();
        ImGui::Separator();
        if (ImGui::MenuItem(Tr("Close Project"), nullptr, false, m_Project.IsOpen())) {
            CloseProject();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(Tr("New Folder..."), nullptr, false, m_Project.IsOpen())) {
            RequestNewFolder();
        }
        if (ImGui::MenuItem(Tr("New Material..."), nullptr, false, m_Project.IsOpen())) {
            RequestNewMaterial();
        }
        if (ImGui::MenuItem(Tr("New Blueprint Function..."), nullptr, false,
                            m_Project.IsOpen())) {
            RequestNewFunction();
        }
        if (ImGui::MenuItem(Tr("Add File / Model..."), nullptr, false, m_Project.IsOpen())) {
            RequestAddProjectFile();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(Tr("UE5 Push / Pull..."))) {
            m_UeBridgePending = true;
        }
        if (ImGui::MenuItem(Tr("Preferences..."), "Ctrl+,")) {
            m_OpenPreferences = true;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(Tr("View"))) {
        if (ImGui::MenuItem(Tr("Frame All"), "Home")) m_GraphView->FrameAll();
        if (ImGui::MenuItem(Tr("Frame Selection"), "F", false, m_GraphView && m_GraphView->SelectedNodeId() != 0)) {
            m_GraphView->FrameSelection();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(Tr("Node Library"), "Ctrl+L", !m_NodeLibraryCollapsed)) {
            SetNodeLibraryCollapsed(!m_NodeLibraryCollapsed);
        }
        if (ImGui::MenuItem(Tr("Console"), "Ctrl+`", m_ConsoleVisible)) {
            SetConsoleVisible(!m_ConsoleVisible);
        }
        if (ImGui::MenuItem(m_PreviewDetached ? Tr("Dock Preview") : Tr("Detach Preview"),
                            nullptr, m_PreviewDetached)) {
            if (m_PreviewDetached) {
                DockPreview();
            } else {
                DetachPreview();
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem(Tr("Preview Scene..."))) {
            m_PreviewSceneModalPending = true;
        }
        if (ImGui::MenuItem(Tr("Sphere Preview"), nullptr, m_PreviewRenderer.GetMesh() ==
                                                                  PreviewMesh::Sphere)) {
            m_Settings.previewMeshMode = "Sphere";
            m_PreviewRenderer.SetMesh(PreviewMesh::Sphere);
            SaveUiSettings();
        }
        if (ImGui::MenuItem(Tr("Plane Preview"), nullptr, m_PreviewRenderer.GetMesh() ==
                                                                 PreviewMesh::Plane)) {
            m_Settings.previewMeshMode = "Plane";
            m_PreviewRenderer.SetMesh(PreviewMesh::Plane);
            SaveUiSettings();
        }
        if (ImGui::MenuItem(Tr("Custom Mesh Preview"), nullptr,
                            m_PreviewRenderer.GetMesh() == PreviewMesh::Custom,
                            !m_Settings.previewMeshPath.empty())) {
            m_Settings.previewMeshMode = "Custom";
            ApplyPreviewSceneSettings();
            SaveUiSettings();
        }
        if (ImGui::MenuItem(Tr("Reset Preview Camera"), "R")) {
            m_PreviewRenderer.Camera().Reset();
            m_PreviewNeedsRender = true;
        }
        ImGui::EndMenu();
    }
    DrawThemeMenu();
    DrawLanguageMenu();
    if (ImGui::BeginMenu(Tr("Help"))) {
        if (ImGui::MenuItem(Tr("About MaterialForge"))) {
            m_OpenAbout = true;
        }
        ImGui::EndMenu();
    }

    const char* rightText = "JSON schema materialforge.ue5.material v1";
    const char* languageLabel = IsChineseUi() ? "English" : "中文";
    const float textWidth = ImGui::CalcTextSize(rightText).x;
    const float languageWidth = ImGui::CalcTextSize(languageLabel).x + 24.0f;
    ImGui::SameLine(ImGui::GetWindowWidth() - textWidth - languageWidth - 24.0f);
    if (ImGui::TextLink(languageLabel)) {
        SetLanguage(IsChineseUi() ? Language::English : Language::Chinese);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", Tr("Switch language"));
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", Tr(rightText));
    ImGui::EndMenuBar();
}

void Application::DrawRecentProjectsMenu() {
    const auto& entries = m_RecentProjects.Entries();
    if (!ImGui::BeginMenu(Tr("Recent Projects"), !entries.empty())) {
        if (entries.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", Tr("No recent projects yet."));
        }
        return;
    }
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const RecentProjectEntry& entry = entries[i];
        const bool missing = !PathUtil::Exists(entry.projectFile);
        ImGui::PushID(static_cast<int>(i));
        const std::string label = entry.name + (missing ? "  (" + std::string(Tr("Missing")) + ")" : "");
        if (ImGui::MenuItem(label.c_str())) {
            OpenProjectFile(PathUtil::ToUtf8(entry.projectFile));
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(PathUtil::ToUtf8(entry.projectFile).c_str());
            if (!entry.lastOpened.empty()) {
                ImGui::TextDisabled(Tr("Last opened: %s"), entry.lastOpened.c_str());
            }
            if (missing) {
                ImGui::TextColored(ImVec4(0.98f, 0.48f, 0.42f, 1.0f),
                                   "%s", Tr("The project file no longer exists."));
            }
            ImGui::EndTooltip();
        }
        if (missing) {
            if (ImGui::MenuItem(Tr("Remove From Recent"))) {
                m_RecentProjects.Remove(entry.projectFile);
                std::string saveError;
                m_RecentProjects.Save(&saveError);
                ImGui::PopID();
                break;
            }
        }
        ImGui::PopID();
    }
    ImGui::Separator();
    ImGui::BeginDisabled(entries.empty());
    if (ImGui::MenuItem(Tr("Clear Recent Projects"))) {
        m_RecentProjects.Clear();
        std::string saveError;
        m_RecentProjects.Save(&saveError);
    }
    ImGui::EndDisabled();
    ImGui::EndMenu();
}

void Application::DrawToolbar() {
    ImGui::BeginChild("##TopToolbar", ImVec2(0.0f, Ui::S(kToolbarHeight)), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    const ImVec2 childPos = ImGui::GetWindowPos();
    const ImVec2 childSize = ImGui::GetWindowSize();
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    const ImVec2 logoPos = ImGui::GetCursorScreenPos();
    Ui::DrawBrandMark(logoPos, Ui::S(40.0f));
    ImGui::Dummy(ImVec2(Ui::S(40.0f), Ui::S(40.0f)));
    ImGui::SameLine(0.0f, Ui::S(12.0f));
    ImGui::BeginGroup();
    ImGui::Dummy(ImVec2(0.0f, Ui::S(2.0f)));
    PushBoldFont();
    ImGui::TextUnformatted("MATERIALFORGE");
    PopBoldFont();
    ImGui::TextDisabled("%s", Tr("UE5 MATERIAL GRAPH WORKBENCH"));
    ImGui::EndGroup();

    ImGui::SameLine(0.0f, Ui::S(24.0f));
    const ImVec2 dividerStart = ImGui::GetCursorScreenPos();
    drawList->AddLine(ImVec2(dividerStart.x, dividerStart.y + Ui::S(4.0f)),
                      ImVec2(dividerStart.x, dividerStart.y + Ui::S(36.0f)),
                      IM_COL32(255, 255, 255, 26), 1.0f);
    ImGui::Dummy(ImVec2(Ui::S(1.0f), Ui::S(38.0f)));
    ImGui::SameLine(0.0f, Ui::S(24.0f));
    ImGui::BeginGroup();
    ImGui::TextDisabled("%s", m_Project.IsOpen() ? Tr("PROJECT") : Tr("DOCUMENT"));
    std::string documentLabel;
    if (m_Project.IsOpen()) {
        documentLabel = m_Project.Name() + "  /  ";
    }
    if (m_DocumentKind == DocumentKind::Instance) {
        documentLabel += m_Instance.Name().empty() ? Tr("Material Instance")
                                                    : m_Instance.Name();
    } else {
        documentLabel += m_Graph.Settings().name.empty() ? Tr("Untitled Material")
                                                         : m_Graph.Settings().name;
    }
    ImGui::TextUnformatted(documentLabel.c_str());
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        if (m_Project.IsOpen()) {
            ImGui::TextUnformatted(PathUtil::ToUtf8(m_Project.ProjectFilePath()).c_str());
        }
        ImGui::TextDisabled("%s", m_DocumentPath.empty()
                                     ? Tr("Unsaved MaterialForge document")
                                     : PathUtil::ToUtf8(m_DocumentPath).c_str());
        ImGui::EndTooltip();
    }
    if (m_Graph.IsDirty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.98f, 0.72f, 0.31f, 1.0f), "*");
    }
    if (m_Project.IsOpen() && m_Project.IsBroken()) {
        ImGui::SameLine();
        Ui::Badge(Tr("BROKEN"), ImVec4(0.98f, 0.40f, 0.34f, 1.0f));
    }
    ImGui::EndGroup();

    const float rightWidth = Ui::S(342.0f);
    const float rightX = ImGui::GetWindowContentRegionMax().x - rightWidth;
    ImGui::SetCursorPos(ImVec2(rightX, Ui::S(19.0f)));
    if (m_DocumentKind == DocumentKind::Function) {
        DrawStatusPill(Tr("BLUEPRINT FUNCTION"), true);
    } else if (!m_Graph.FindMaterialOutput()) {
        DrawStatusPill(Tr("NO MATERIAL"), false, true);
    } else {
        DrawStatusPill(m_ShaderGenerated && m_ShaderCompiled ? Tr("GLSL LIVE")
                                                             : Tr("SHADER ERROR"),
                       m_ShaderGenerated && m_ShaderCompiled);
    }
    ImGui::SameLine(0.0f, 10.0f);
    if (Ui::GhostButton(Tr("Save Project"), ImVec2(Ui::S(94.0f), Ui::S(28.0f)))) {
        SaveProject();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", Tr("Save the project manifest (Ctrl+S). Use the graph Save button for the material."));
    }
    if (m_DocumentKind == DocumentKind::Material) {
        ImGui::SameLine(0.0f, 8.0f);
        if (Ui::AccentButton(Tr("Export JSON"), ImVec2(Ui::S(116.0f), Ui::S(28.0f)))) {
            RequestExportDialog();
        }
    }

    drawList->AddLine(ImVec2(childPos.x + Ui::S(12.0f), childPos.y + childSize.y - 1.0f),
                      ImVec2(childPos.x + childSize.x - Ui::S(12.0f), childPos.y + childSize.y - 1.0f),
                      IM_COL32(255, 255, 255, 20), 1.0f);

    ImGui::EndChild();
}

void Application::DrawDocumentTabs(float width, float height) {
    if (m_Documents.empty()) return;
    ImGui::BeginChild("##DocumentTabs", ImVec2(width, height), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const float tabHeight = Ui::S(30.0f);
    const float closeWidth = Ui::S(24.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Ui::S(6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    int closeRequest = -1;
    for (int i = 0; i < static_cast<int>(m_Documents.size()); ++i) {
        const EditorDocument& document = m_Documents[static_cast<std::size_t>(i)];
        const bool active = i == m_ActiveDocument;
        const bool dirty = active ? m_Graph.IsDirty() : document.graph.IsDirty();
        std::string title = active
            ? DocumentTitle(m_DocumentPath, m_Graph, m_DocumentKind == DocumentKind::Function)
            : document.title;
        if (document.kind == DocumentKind::Function) title = "Fn  " + title;
        std::string visibleTitle = title;
        if (dirty) visibleTitle += " *";

        ImGui::PushID(i);
        const float titleWidth = std::min(Ui::S(320.0f),
                                          ImGui::CalcTextSize(visibleTitle.c_str()).x +
                                              Ui::S(24.0f));
        const ImVec4 buttonColor = active ? ImVec4(0.18f, 0.39f, 0.38f, 1.0f)
                                          : ImVec4(0.19f, 0.20f, 0.21f, 1.0f);
        const ImVec4 hoverColor = active ? ImVec4(0.22f, 0.47f, 0.45f, 1.0f)
                                         : ImVec4(0.24f, 0.25f, 0.27f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, buttonColor);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hoverColor);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, buttonColor);
        ImGui::PushStyleColor(ImGuiCol_Text,
                              active ? ImVec4(0.95f, 1.0f, 0.99f, 1.0f)
                                     : ImVec4(0.73f, 0.77f, 0.79f, 1.0f));
        const std::string label = visibleTitle + "###tab_" + std::to_string(i);
        if (ImGui::Button(label.c_str(), ImVec2(titleWidth, tabHeight))) {
            ActivateDocument(i);
        }
        ImGui::PopStyleColor(4);
        if (active) {
            const ImVec2 min = ImGui::GetItemRectMin();
            const ImVec2 max = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddLine(ImVec2(min.x + Ui::S(8.0f), max.y - 2.0f),
                                                ImVec2(max.x - Ui::S(8.0f), max.y - 2.0f),
                                                IM_COL32(94, 234, 211, 255), Ui::S(2.0f));
        }
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, active ? ImVec4(0.18f, 0.39f, 0.38f, 1.0f)
                                                       : ImVec4(0.19f, 0.20f, 0.21f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.42f, 0.22f, 0.21f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.88f, 0.89f, 1.0f));
        if (ImGui::Button("x", ImVec2(closeWidth, tabHeight))) {
            closeRequest = i;
        }
        ImGui::PopStyleColor(3);
        ImGui::SameLine(0.0f, Ui::S(4.0f));
        ImGui::PopID();
    }
    ImGui::PopStyleVar(2);
    ImGui::EndChild();
    if (closeRequest >= 0) {
        RequestCloseDocument(closeRequest);
    }
}

void Application::DrawDocumentDialogs() {
    const std::string popupTitle =
        std::string(Tr("Unsaved Changes")) + "###CloseDocumentWithoutSaving";
    if (m_OpenCloseDocumentModal) {
        ImGui::OpenPopup(popupTitle.c_str());
        m_OpenCloseDocumentModal = false;
    }
    ImGui::SetNextWindowSize(ImVec2(Ui::S(500.0f), 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(popupTitle.c_str(), nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }

    if (m_PendingCloseDocument < 0 ||
        m_PendingCloseDocument >= static_cast<int>(m_Documents.size())) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    const EditorDocument& document = m_Documents[static_cast<std::size_t>(m_PendingCloseDocument)];
    ImGui::TextWrapped("%s", Tr("The document has unsaved changes. Save before closing?"));
    ImGui::Spacing();
    ImGui::TextDisabled("%s", document.title.c_str());
    ImGui::Spacing();
    ImGui::Separator();

    const float buttonWidth = Ui::S(120.0f);
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - buttonWidth * 3.0f -
                         ImGui::GetStyle().ItemSpacing.x * 2.0f);
    if (ImGui::Button(Tr("Save"), ImVec2(buttonWidth, Ui::S(32.0f)))) {
        const int index = m_PendingCloseDocument;
        if (index != m_ActiveDocument) {
            ActivateDocument(index);
        }
        if (m_Documents[static_cast<std::size_t>(index)].path.empty()) {
            m_PendingCloseSaveAs = true;
            m_PendingCloseDocument = index;
            ImGui::CloseCurrentPopup();
            RequestSave(false);
            ImGui::EndPopup();
            return;
        }
        std::string error;
        bool saved = false;
        if (index == m_ActiveDocument) {
            saved = SaveDocument(PathUtil::ToUtf8(m_DocumentPath), true, &error);
        } else {
            saved = GraphSerializer::Save(m_Documents[static_cast<std::size_t>(index)].graph,
                                          PathUtil::ToUtf8(m_Documents[static_cast<std::size_t>(index)].path),
                                          &error);
            if (saved) m_Documents[static_cast<std::size_t>(index)].graph.MarkSaved();
        }
        if (saved) {
            ImGui::CloseCurrentPopup();
            CloseDocumentNow(index);
        } else {
            SetStatus(FormatLocalized("Save failed: %s", error.c_str()), true);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr("Don't Save"), ImVec2(buttonWidth, Ui::S(32.0f)))) {
        const int index = m_PendingCloseDocument;
        ImGui::CloseCurrentPopup();
        CloseDocumentNow(index);
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr("Cancel"), ImVec2(buttonWidth, Ui::S(32.0f)))) {
        m_PendingCloseDocument = -1;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void Application::DrawBody(float height) {
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const float documentTabsHeight = Ui::S(34.0f);
    DrawDocumentTabs(availableWidth, documentTabsHeight);
    height = std::max(Ui::S(240.0f),
                      height - documentTabsHeight - ImGui::GetStyle().ItemSpacing.y);
    float leftWidth = m_NodeLibraryCollapsed ? Ui::S(46.0f) : Ui::S(kLeftPanelWidth);
    float rightWidth = Ui::S(kRightPanelWidth);
    float centerWidth = availableWidth - leftWidth - rightWidth - Ui::S(kPanelGap) * 2.0f;

    if (centerWidth < Ui::S(420.0f) && !m_NodeLibraryCollapsed) {
        leftWidth = Ui::S(214.0f);
        rightWidth = Ui::S(380.0f);
        centerWidth = availableWidth - leftWidth - rightWidth - Ui::S(kPanelGap) * 2.0f;
    }
    centerWidth = std::max(Ui::S(320.0f), centerWidth);

    const float spacingY = ImGui::GetStyle().ItemSpacing.y;
    const float splitterHeight = Ui::S(6.0f);
    const float minFileBarHeight = Ui::S(190.0f);
    const float maxFileBarHeight =
        std::min(Ui::S(420.0f),
                 std::max(minFileBarHeight,
                          std::min(height - Ui::S(250.0f) - splitterHeight - spacingY * 2.0f,
                                   height * 0.42f)));
    const float fileBarHeight =
        std::clamp(m_FileBarHeight, minFileBarHeight,
                   std::min(Ui::S(420.0f), maxFileBarHeight));
    const float contentHeight =
        std::max(Ui::S(240.0f),
                 height - fileBarHeight - splitterHeight - spacingY * 2.0f);

    DrawLeftSidebar(leftWidth, contentHeight);
    ImGui::SameLine(0.0f, Ui::S(kPanelGap));
    DrawNodeEditorPanel(centerWidth, contentHeight);
    ImGui::SameLine(0.0f, Ui::S(kPanelGap));
    DrawRightSidebar(rightWidth, contentHeight);

    const float splitterWidth = ImGui::GetContentRegionAvail().x;
    ImGui::InvisibleButton("##FileBarSplitter", ImVec2(splitterWidth, splitterHeight));
    const bool splitterHovered = ImGui::IsItemHovered();
    const bool splitterActive = ImGui::IsItemActive();
    if (splitterHovered || splitterActive) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }
    if (splitterActive) {
        m_FileBarHeight = std::clamp(m_FileBarHeight - ImGui::GetIO().MouseDelta.y,
                                     minFileBarHeight,
                                     std::min(Ui::S(420.0f), maxFileBarHeight));
    }
    if (ImGui::IsItemDeactivated()) {
        SaveUiSettings();
    }
    const ImVec2 splitterMin = ImGui::GetItemRectMin();
    const ImVec2 splitterMax = ImGui::GetItemRectMax();
    ImDrawList* splitterDrawList = ImGui::GetWindowDrawList();
    const ImU32 splitterColor = splitterActive
        ? IM_COL32(89, 211, 191, 190)
        : splitterHovered ? IM_COL32(89, 211, 191, 130) : IM_COL32(255, 255, 255, 28);
    splitterDrawList->AddLine(ImVec2(splitterMin.x + 10.0f, splitterMin.y + 2.0f),
                              ImVec2(splitterMax.x - 10.0f, splitterMin.y + 2.0f),
                              splitterColor, 1.0f);
    const float gripX = (splitterMin.x + splitterMax.x) * 0.5f;
    splitterDrawList->AddRectFilled(ImVec2(gripX - 18.0f, splitterMin.y + 2.0f),
                                    ImVec2(gripX + 18.0f, splitterMin.y + 4.0f),
                                    splitterHovered ? IM_COL32(89, 211, 191, 170)
                                                    : IM_COL32(255, 255, 255, 40),
                                    2.0f);

    DrawFilesPanel(fileBarHeight);
}

void Application::AddNodeFromLibrary(const std::string& type) {
    if (!m_GraphView) {
        return;
    }
    std::string error;
    Node* node = m_GraphView->AddNodeAtViewCenter(type, &error);
    if (!node) {
        SetStatus(error.empty() ? Tr("Could not add node.") : error, true);
        return;
    }
    const std::string nodeName = NodeDisplayName(*node);
    SetStatus(FormatLocalized("Added %s", nodeName.c_str()));
}

void Application::DrawLeftSidebar(float width, float height) {
    if (m_NodeLibraryCollapsed) {
        ImGui::BeginChild("##NodeLibraryRail", ImVec2(width, height), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        const float buttonWidth = std::max(20.0f, width - Ui::S(12.0f));
        if (Ui::GhostButton(">>", ImVec2(buttonWidth, Ui::S(28.0f)))) {
            SetNodeLibraryCollapsed(false);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", Tr("Expand the node library (Ctrl+L)"));
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.0f, Ui::S(8.0f)));
        const std::string label = IsChineseUi() ? std::string(u8"节点库") : std::string("NODES");
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        float y = ImGui::GetCursorScreenPos().y;
        const float centerX = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() * 0.5f;
        for (std::size_t offset = 0; offset < label.size();) {
            const std::size_t next = Utf8Step(label, offset);
            const std::string glyph = label.substr(offset, next - offset);
            const ImVec2 glyphSize = ImGui::CalcTextSize(glyph.c_str());
            drawList->AddText(ImVec2(centerX - glyphSize.x * 0.5f, y),
                              ImGui::GetColorU32(ImGuiCol_TextDisabled), glyph.c_str());
            y += Ui::S(19.0f);
            offset = next;
        }
        ImGui::EndChild();
        return;
    }

    ImGui::BeginChild("##NodeLibrary", ImVec2(width, height), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    DrawNodeLibraryContent();
    ImGui::EndChild();
}

void Application::DrawNodeLibraryContent() {
    if (m_DocumentKind == DocumentKind::Instance) {
        Ui::SectionLabel(Tr("MATERIAL INSTANCE"));
        ImGui::Spacing();
        ImGui::TextWrapped("%s",
                           Tr("This document is a Material Instance. Edit parameter "
                              "overrides in the center instance panel or the Inspector; "
                              "the parent material graph is not edited here."));
        ImGui::Spacing();
        ImGui::TextDisabled("%s", Tr("Parent material"));
        ImGui::TextWrapped("%s", m_Instance.ParentMaterial().c_str());
        return;
    }
    const bool functionMode = m_DocumentKind == DocumentKind::Function;
    const bool hasOutput = m_Graph.FindMaterialOutput() != nullptr;
    int availableNodeCount = 0;
    for (const NodeDefinition& definition : NodeRegistry::Get().All()) {
        if (definition.hiddenInLibrary) continue;
        if (definition.isMaterialOutput && hasOutput) continue;
        if (functionMode && definition.isMaterialOutput) continue;
        if (!functionMode && definition.isFunctionOnly) continue;
        ++availableNodeCount;
    }
    Ui::SectionLabel(functionMode ? Tr("FUNCTION LIBRARY") : Tr("NODE LIBRARY"));
    ImGui::SameLine();
    ImGui::TextDisabled("  ");
    ImGui::SameLine(0.0f, 2.0f);
    ImGui::TextDisabled(Tr("%d nodes"), availableNodeCount);
    const float collapseWidth = Ui::S(22.0f);
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - collapseWidth);
    if (Ui::GhostButton("<<", ImVec2(collapseWidth, Ui::S(20.0f)))) {
        SetNodeLibraryCollapsed(true);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", Tr("Collapse the node library (Ctrl+L)"));
    }
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##library_search", Tr("Search nodes..."), m_LibrarySearch,
                             sizeof(m_LibrarySearch));
    ImGui::TextDisabled("%s", Tr("Double-click a row or press + to add."));
    ImGui::Spacing();

    ImGui::BeginChild("##library_list", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
    const std::string query = Trim(m_LibrarySearch);
    const bool searching = !query.empty();
    bool anyResult = false;
    const NodeRegistry& registry = NodeRegistry::Get();

    auto addRow = [&](const NodeDefinition& definition) {
        ImGui::PushID(definition.type.c_str());
        const float rowHeight = Ui::S(24.0f);
        const float contentWidth = ImGui::GetContentRegionAvail().x;
        const float plusWidth = Ui::S(22.0f);
        const float rowWidth = std::max(60.0f, contentWidth - plusWidth - Ui::S(5.0f));
        ImGui::Selectable("##node_row", false, ImGuiSelectableFlags_AllowDoubleClick,
                          ImVec2(rowWidth, rowHeight));
        const bool rowHovered =
            ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        const bool rowDoubleClicked =
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("MF_NODE_TYPE", definition.type.c_str(),
                                      definition.type.size() + 1);
            ImGui::TextUnformatted(definition.displayName.c_str());
            ImGui::TextDisabled("%s", Tr("Drop into the graph to add."));
            ImGui::EndDragDropSource();
        }
        const ImVec2 rowMin = ImGui::GetItemRectMin();
        const ImVec2 rowMax = ImGui::GetItemRectMax();
        const bool hovered = ImGui::IsItemHovered();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const float centerY = (rowMin.y + rowMax.y) * 0.5f;
        const float thumbnailSize = Ui::S(20.0f);
        Ui::DrawNodeThumbnail(drawList,
                              ImVec2(rowMin.x + Ui::S(2.0f), centerY - thumbnailSize * 0.5f),
                              thumbnailSize, definition.type, definition.category);
        drawList->AddText(ImVec2(rowMin.x + Ui::S(28.0f), rowMin.y + (rowHeight - ImGui::GetTextLineHeight()) * 0.5f),
                          ImGui::GetColorU32(ImGuiCol_Text), definition.displayName.c_str());
        if (hovered) {
            ImGui::BeginTooltip();
            ImGui::TextColored(Ui::CategoryColor(definition.category), "%s", definition.ueClass.c_str());
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 18.0f);
            ImGui::TextWrapped("%s", Tr(definition.description.c_str()));
            ImGui::PopTextWrapPos();
            ImGui::TextDisabled("%s", Tr("Double-click or + to add."));
            ImGui::EndTooltip();
        }

        bool add = rowDoubleClicked;
        ImGui::SameLine(0.0f, Ui::S(5.0f));
        if (ImGui::SmallButton("+")) {
            add = true;
        }
        ImGui::PopID();
        if (add) {
            AddNodeFromLibrary(definition.type);
        }
    };

    anyResult = DrawMaterialFunctionLibrary(query) || anyResult;

    std::vector<std::string> categories = registry.Categories();
    if (functionMode) {
        const auto functionCategory = std::find(categories.begin(), categories.end(), "Function");
        if (functionCategory != categories.end()) {
            std::rotate(categories.begin(), functionCategory, functionCategory + 1);
        }
    }
    for (const std::string& category : categories) {
        std::vector<const NodeDefinition*> matching;
        for (const NodeDefinition& definition : registry.All()) {
            if (definition.category != category) continue;
            if (definition.hiddenInLibrary) continue;
            if (definition.isMaterialOutput && hasOutput) continue;
            if (functionMode && definition.isMaterialOutput) continue;
            if (!functionMode && definition.isFunctionOnly) continue;
            if (searching && !NodeMatchesSearch(definition, query)) continue;
            matching.push_back(&definition);
        }
        if (matching.empty()) continue;
        anyResult = true;

        bool sectionOpen = true;
        const std::string categoryLabel = Tr(category.c_str());
        if (searching) {
            ImGui::PushStyleColor(ImGuiCol_Text, Ui::CategoryColor(category));
            ImGui::TextUnformatted(categoryLabel.c_str());
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, Ui::CategoryColor(category));
            const std::string categoryHeader = categoryLabel + "###node_category_" + category;
            sectionOpen = ImGui::CollapsingHeader(categoryHeader.c_str(),
                                                   ImGuiTreeNodeFlags_DefaultOpen |
                                                       ImGuiTreeNodeFlags_SpanAvailWidth);
            ImGui::PopStyleColor();
        }
        if (sectionOpen) {
            for (const NodeDefinition* definition : matching) {
                addRow(*definition);
            }
        }
        ImGui::Spacing();
    }

    if (!anyResult) {
        ImGui::TextDisabled("%s", Tr("No nodes match the search."));
    }
    ImGui::EndChild();
}

void Application::RefreshFunctionAssets() {
    if (!m_FunctionAssetsDirty) {
        return;
    }
    m_FunctionAssets.clear();
    std::unordered_set<std::string> seen;
    auto addAsset = [&](const fs::path& path, bool builtIn) {
        std::error_code ec;
        if (!fs::is_regular_file(path, ec)) return;
        if (path.extension() != ".mffunction") return;
        const std::string key = PathUtil::ToUtf8(fs::weakly_canonical(path, ec));
        if (!seen.insert(key).second) return;
        FunctionAssetInfo info;
        info.name = path.stem().string();
        info.path = PathUtil::ToUtf8(path);
        info.builtIn = builtIn;
        m_FunctionAssets.push_back(std::move(info));
    };

    const fs::path presets = m_ContentRoot / "assets" / "functions";
    std::error_code ec;
    if (fs::is_directory(presets, ec)) {
        for (const auto& entry : fs::directory_iterator(presets, ec)) {
            if (entry.is_regular_file(ec)) {
                addAsset(entry.path(), true);
            }
        }
    }
    if (m_Project.IsOpen()) {
        for (const auto& entry : fs::recursive_directory_iterator(
                 m_Project.RootPath(), fs::directory_options::skip_permission_denied, ec)) {
            if (entry.is_regular_file(ec) && entry.path().extension() == ".mffunction") {
                addAsset(entry.path(), false);
            }
            if (ec) {
                ec.clear();
            }
        }
    }
    std::sort(m_FunctionAssets.begin(), m_FunctionAssets.end(),
              [](const FunctionAssetInfo& a, const FunctionAssetInfo& b) {
                  if (a.builtIn != b.builtIn) return a.builtIn > b.builtIn;
                  return ToLower(a.name) < ToLower(b.name);
              });
    m_FunctionAssetsDirty = false;
}

std::filesystem::path Application::ResolveFunctionPath(const std::string& path) const {
    if (path.empty()) return {};
    std::error_code ec;
    fs::path candidate = PathUtil::FromUtf8(path);
    if (candidate.is_absolute()) {
        return fs::is_regular_file(candidate, ec) ? candidate : fs::path();
    }
    if (m_Project.IsOpen()) {
        candidate = m_Project.ResolveRelative(path);
        if (!candidate.empty() && fs::is_regular_file(candidate, ec)) {
            return candidate;
        }
    }
    candidate = m_ContentRoot / PathUtil::FromUtf8(path);
    if (fs::is_regular_file(candidate, ec)) {
        return candidate;
    }
    if (!m_DocumentPath.empty()) {
        candidate = m_DocumentPath.parent_path() / PathUtil::FromUtf8(path);
        if (fs::is_regular_file(candidate, ec)) {
            return candidate;
        }
    }
    candidate = PathUtil::ResolveResource(path);
    return fs::is_regular_file(candidate, ec) ? candidate : fs::path();
}

void Application::OpenMaterialFunction(const std::string& path) {
    const fs::path resolved = ResolveFunctionPath(path);
    if (resolved.empty()) {
        SetStatus(FormatLocalized("Material function not found: %s", path.c_str()), true);
        return;
    }
    OpenFunctionAsset(resolved);
}

void Application::RevealMaterialFunction(const std::string& path) {
    const fs::path resolved = ResolveFunctionPath(path);
    if (resolved.empty()) {
        SetStatus(FormatLocalized("Material function not found: %s", path.c_str()), true);
        return;
    }
    OpenMaterialFunction(path);
    if (m_Project.IsOpen() && m_Project.Contains(resolved)) {
        const std::string relative = m_Project.RelativePathOf(resolved);
        if (!relative.empty()) {
            const std::string parent = fs::path(relative).parent_path().generic_u8string();
            NavigateProjectFolder(parent);
            m_ProjectSelectedEntry = relative;
            m_ProjectSearch.clear();
            m_ProjectEntriesDirty = true;
            m_ScrollProjectSelection = true;
        }
    }
}

bool Application::DrawMaterialFunctionLibrary(const std::string& query) {
    RefreshFunctionAssets();
    const std::string loweredQuery = ToLower(Trim(query));
    std::vector<const FunctionAssetInfo*> matching;
    for (const FunctionAssetInfo& asset : m_FunctionAssets) {
        if (!loweredQuery.empty()) {
            const std::string haystack = ToLower(asset.name + " " + asset.path);
            if (haystack.find(loweredQuery) == std::string::npos) continue;
        }
        matching.push_back(&asset);
    }
    if (matching.empty()) {
        return false;
    }

    ImGui::Spacing();
    const std::string header = std::string(Tr("MATERIAL FUNCTIONS")) +
                               "###material_functions";
    if (!ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen |
                                                    ImGuiTreeNodeFlags_SpanAvailWidth)) {
        return true;
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", Tr("Double-click to open a function; drag into a material graph to call it."));
    ImGui::PopTextWrapPos();
    for (const FunctionAssetInfo* asset : matching) {
        ImGui::PushID(asset->path.c_str());
        const float rowWidth = ImGui::GetContentRegionAvail().x;
        const bool selected = false;
        ImGui::Selectable("##function_row", selected,
                          ImGuiSelectableFlags_AllowDoubleClick,
                          ImVec2(std::max(60.0f, rowWidth), Ui::S(24.0f)));
        const bool rowHovered =
            ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        const bool rowDoubleClicked =
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("MF_FUNCTION_ASSET", asset->path.c_str(),
                                      asset->path.size() + 1);
            ImGui::TextUnformatted(asset->name.c_str());
            ImGui::TextDisabled("%s", Tr("Drop into a material graph to create a call node."));
            ImGui::EndDragDropSource();
        }
        const ImVec2 rowMin = ImGui::GetItemRectMin();
        const ImVec2 rowMax = ImGui::GetItemRectMax();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const float centerY = (rowMin.y + rowMax.y) * 0.5f;
        drawList->AddCircleFilled(ImVec2(rowMin.x + Ui::S(9.0f), centerY), Ui::S(3.2f),
                                  Ui::CategoryU32("Functions"), 10);
        drawList->AddText(ImVec2(rowMin.x + Ui::S(19.0f),
                                 rowMin.y + (Ui::S(24.0f) - ImGui::GetTextLineHeight()) * 0.5f),
                          ImGui::GetColorU32(ImGuiCol_Text), asset->name.c_str());
        if (rowHovered) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::BeginTooltip();
            ImGui::TextColored(Ui::CategoryColor("Functions"), "%s",
                               asset->builtIn ? Tr("Built-in Material Function")
                                              : Tr("Project Material Function"));
            ImGui::TextWrapped("%s", asset->path.c_str());
            ImGui::TextDisabled("%s", Tr("Double-click to open; drag into the graph to call."));
            ImGui::EndTooltip();
        }
        if (rowDoubleClicked) {
            OpenMaterialFunction(asset->path);
        }
        ImGui::PopID();
    }
    return true;
}

void Application::DrawFilesPanel(float height) {
    ImGui::BeginChild("##ContentBrowser", ImVec2(0.0f, height), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 browserPos = ImGui::GetWindowPos();
    const ImVec2 browserSize = ImGui::GetWindowSize();
    m_ContentBrowserMinX = browserPos.x;
    m_ContentBrowserMinY = browserPos.y;
    m_ContentBrowserMaxX = browserPos.x + browserSize.x;
    m_ContentBrowserMaxY = browserPos.y + browserSize.y;
    m_ContentBrowserRectValid = browserSize.x > 1.0f && browserSize.y > 1.0f;

    const float contentMaxX = ImGui::GetWindowContentRegionMax().x;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float gap = Ui::S(8.0f);
    const bool projectOpen = m_Project.IsOpen();

    const auto widthFor = [](const char* label, float minimum) {
        return std::max(minimum, ImGui::CalcTextSize(label).x + Ui::S(24.0f));
    };
    const char* newMaterialLabel = Tr("New Material");
    const char* newFolderLabel = Tr("New Folder");
    const char* addFileLabel = Tr("Add File");
    const char* addPackageLabel = Tr("Add Package");
    const char* refreshLabel = Tr("Refresh");
    const char* newProjectLabel = Tr("New Project");
    const char* openProjectLabel = Tr("Open Project...");
    const char* clearRecentLabel = Tr("Clear Recent Projects");

    float newMaterialWidth = widthFor(newMaterialLabel, 96.0f);
    float newFolderWidth = widthFor(newFolderLabel, 82.0f);
    float addFileWidth = widthFor(addFileLabel, 70.0f);
    float addPackageWidth = widthFor(addPackageLabel, 88.0f);
    float refreshWidth = widthFor(refreshLabel, 62.0f);
    float actionsWidth = newMaterialWidth + newFolderWidth + addFileWidth + addPackageWidth +
                         refreshWidth + spacing * 4.0f;
    if (!projectOpen) {
        actionsWidth = widthFor(newProjectLabel, 96.0f) + widthFor(openProjectLabel, 92.0f) +
                       widthFor(clearRecentLabel, 116.0f) + spacing * 2.0f;
    }

    Ui::SectionLabel(Tr("CONTENT BROWSER"));
    ImGui::SameLine();
    if (projectOpen) {
        const float nameStart = ImGui::GetCursorPosX();
        const float reserved = actionsWidth + Ui::S(250.0f);
        const float nameWidth = std::max(70.0f, contentMaxX - nameStart - reserved);
        const std::string title = EllipsizeToWidth(m_Project.Name(), nameWidth);
        PushBoldFont();
        ImGui::TextUnformatted(title.c_str());
        PopBoldFont();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip("%s", PathUtil::ToUtf8(m_Project.ProjectFilePath()).c_str());
        }
        ImGui::SameLine();
        if (m_Project.IsBroken()) {
            Ui::Badge(Tr("BROKEN"), ImVec4(0.98f, 0.40f, 0.34f, 1.0f));
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.98f, 0.48f, 0.42f, 1.0f),
                               Tr("%d missing reference(s)"),
                               static_cast<int>(m_Project.Issues().size()));
            ImGui::SameLine();
            if (Ui::GhostButton(Tr("Details"), ImVec2(Ui::S(62.0f), Ui::S(22.0f)))) {
                ImGui::OpenPopup("##missing_project_refs");
            }
            if (ImGui::BeginPopup("##missing_project_refs")) {
                ImGui::TextUnformatted(Tr("Missing marked files"));
                ImGui::Separator();
                ImGui::BeginChild("##missing_project_list", ImVec2(640.0f, 190.0f),
                                  ImGuiChildFlags_Borders);
                for (const ProjectIntegrityIssue& issue : m_Project.Issues()) {
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextColored(ImVec4(0.96f, 0.58f, 0.51f, 1.0f), "%s",
                                       issue.path.c_str());
                    ImGui::TextDisabled("%s", issue.reason.c_str());
                    ImGui::PopTextWrapPos();
                }
                ImGui::EndChild();
                if (ImGui::Button(Tr("Close"), ImVec2(90.0f, Ui::S(24.0f)))) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        } else {
            Ui::Badge(Tr("OK"), ImVec4(0.31f, 0.84f, 0.78f, 1.0f));
        }
    } else {
        ImGui::TextDisabled("%s", Tr("No project open"));
        ImGui::SameLine();
        ImGui::TextDisabled(" - %s", Tr("create or open a project to manage folders, materials and assets"));
    }

    const float desiredActionX = contentMaxX - actionsWidth;
    if (ImGui::GetCursorPosX() + 18.0f > desiredActionX) {
        ImGui::NewLine();
    } else {
        ImGui::SameLine();
    }
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), desiredActionX));
    if (projectOpen) {
        if (Ui::AccentButton(newMaterialLabel, ImVec2(newMaterialWidth, Ui::S(24.0f)))) {
            RequestNewMaterial();
        }
        ImGui::SameLine(0.0f, spacing);
        if (Ui::GhostButton(newFolderLabel, ImVec2(newFolderWidth, Ui::S(24.0f)))) {
            RequestNewFolder();
        }
        ImGui::SameLine(0.0f, spacing);
        if (Ui::GhostButton(addFileLabel, ImVec2(addFileWidth, Ui::S(24.0f)))) {
            RequestAddProjectFile();
        }
        ImGui::SameLine(0.0f, spacing);
        if (Ui::GhostButton(addPackageLabel, ImVec2(addPackageWidth, Ui::S(24.0f)))) {
            RequestTexturePackages();
        }
        ImGui::SameLine(0.0f, spacing);
        if (Ui::GhostButton(refreshLabel, ImVec2(refreshWidth, Ui::S(24.0f)))) {
            RefreshProjectEntries(true);
            RefreshProjectIntegrity();
            SetStatus(m_Project.IsBroken()
                          ? Tr("Project refreshed; it is still broken because marked files are missing.")
                          : Tr("Project files refreshed."),
                      m_Project.IsBroken());
        }
    } else {
        if (Ui::AccentButton(newProjectLabel, ImVec2(widthFor(newProjectLabel, 96.0f), Ui::S(24.0f)))) {
            RequestNewProject();
        }
        ImGui::SameLine(0.0f, spacing);
        if (Ui::GhostButton(openProjectLabel, ImVec2(widthFor(openProjectLabel, 92.0f), Ui::S(24.0f)))) {
            RequestOpenProject();
        }
        ImGui::SameLine(0.0f, spacing);
        ImGui::BeginDisabled(m_RecentProjects.Entries().empty());
        if (Ui::GhostButton(clearRecentLabel, ImVec2(widthFor(clearRecentLabel, 116.0f), Ui::S(24.0f)))) {
            m_RecentProjects.Clear();
            std::string error;
            m_RecentProjects.Save(&error);
        }
        ImGui::EndDisabled();
    }

    ImGui::Spacing();

    if (!projectOpen) {
        ImGui::BeginChild("##recent_projects_tiles", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
        const auto drawNoProjectBlankMenu = [&]() {
            if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
                !ImGui::IsAnyItemHovered() &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                ImGui::OpenPopup("##content_browser_blank_menu");
            }
            if (ImGui::BeginPopup("##content_browser_blank_menu")) {
                if (ImGui::MenuItem(Tr("New Project..."))) RequestNewProject();
                if (ImGui::MenuItem(Tr("Open Project..."))) RequestOpenProject();
                if (!m_RecentProjects.Entries().empty()) {
                    ImGui::Separator();
                    if (ImGui::MenuItem(Tr("Clear Recent Projects"))) {
                        m_RecentProjects.Clear();
                        std::string error;
                        m_RecentProjects.Save(&error);
                    }
                }
                ImGui::EndPopup();
            }
        };

        const auto& recent = m_RecentProjects.Entries();
        const float tileWidth = Ui::S(120.0f);
        const float tileHeight = Ui::S(108.0f);
        const int columns = std::max(
            1, static_cast<int>((ImGui::GetContentRegionAvail().x + gap) / (tileWidth + gap)));
        int index = 0;
        std::string projectToOpen;
        for (const RecentProjectEntry& entry : recent) {
            const bool missing = !PathUtil::Exists(entry.projectFile);
            if (index % columns != 0) ImGui::SameLine(0.0f, gap);
            ImGui::PushID(static_cast<int>(index));
            const ImVec2 cardMin = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##recent_project_tile", ImVec2(tileWidth, tileHeight));
            const bool hovered = ImGui::IsItemHovered();
            // A recent project opens with a single left click. Trigger on
            // release so a drag that leaves the tile does not launch a project.
            const bool clicked = ImGui::IsItemDeactivated() && ImGui::IsItemHovered();
            if (hovered) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            const ImVec2 cardMax(cardMin.x + tileWidth, cardMin.y + tileHeight);
            const bool graphiteTheme = Ui::CurrentTheme() == Ui::Theme::Graphite;
            const ImU32 cardBackground =
                hovered ? (graphiteTheme ? IM_COL32(48, 53, 58, 255)
                                         : IM_COL32(21, 38, 44, 255))
                        : (graphiteTheme ? IM_COL32(37, 41, 45, 255)
                                         : IM_COL32(14, 26, 32, 255));
            drawList->AddRectFilled(cardMin, cardMax, cardBackground, 8.0f);
            drawList->AddRect(cardMin, cardMax,
                              hovered ? IM_COL32(82, 157, 158, 170)
                                      : IM_COL32(255, 255, 255, 18),
                              8.0f, 0, hovered ? 1.4f : 1.0f);
            const ImVec2 thumbMin(cardMin.x + Ui::S(6.0f), cardMin.y + Ui::S(6.0f));
            const ImVec2 thumbMax(cardMax.x - Ui::S(6.0f), thumbMin.y + (tileHeight - Ui::S(46.0f)));
            drawList->AddRectFilled(thumbMin, thumbMax, IM_COL32(20, 23, 26, 255), 6.0f);
            drawList->AddRect(thumbMin, thumbMax, IM_COL32(255, 255, 255, 16), 6.0f);
            DrawProjectGlyph(drawList,
                             ImVec2((thumbMin.x + thumbMax.x) * 0.5f,
                                    (thumbMin.y + thumbMax.y) * 0.5f),
                             52.0f, hovered);
            const std::string title =
                EllipsizeToWidth(entry.name, tileWidth - Ui::S(20.0f));
            const float nameWidth = ImGui::CalcTextSize(title.c_str()).x;
            drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                              ImVec2(cardMin.x + (tileWidth - nameWidth) * 0.5f,
                                     thumbMax.y + Ui::S(3.0f)),
                              missing ? IM_COL32(248, 132, 122, 255)
                                      : ImGui::GetColorU32(ImGuiCol_Text),
                              title.c_str());
            ImFont* smallFont = m_SmallFont ? m_SmallFont : ImGui::GetFont();
            const float smallSize = m_SmallFont ? m_SmallFont->FontSize : ImGui::GetFontSize();
            const std::string caption = missing ? Tr("Missing") : Tr("Recent project");
            const ImVec2 captionSize = smallFont->CalcTextSizeA(smallSize, FLT_MAX, 0.0f,
                                                                 caption.c_str());
            const float captionX = cardMin.x + (tileWidth - captionSize.x) * 0.5f;
            drawList->AddText(smallFont, smallSize,
                              ImVec2(captionX, cardMax.y - Ui::S(17.0f)),
                              missing ? IM_COL32(247, 120, 110, 235)
                                      : IM_COL32(117, 180, 178, 230),
                              caption.c_str());
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(PathUtil::ToUtf8(entry.projectFile).c_str());
                if (!entry.lastOpened.empty()) {
                    ImGui::TextDisabled(Tr("Last opened: %s"), entry.lastOpened.c_str());
                }
                if (missing) {
                    ImGui::TextColored(ImVec4(0.98f, 0.48f, 0.42f, 1.0f), "%s",
                                       Tr("The project file no longer exists."));
                } else {
                    ImGui::TextDisabled("%s", Tr("Click to open."));
                }
                ImGui::EndTooltip();
            }
            if (clicked) {
                projectToOpen = PathUtil::ToUtf8(entry.projectFile);
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
            ++index;
        }
        if (recent.empty()) {
            ImGui::Dummy(ImVec2(0.0f, 14.0f));
            const std::string message = Tr("No recent projects yet. Create a project to get started.");
            const float messageWidth = ImGui::CalcTextSize(message.c_str()).x;
            const float startX = ImGui::GetCursorPosX();
            const float availableWidth = ImGui::GetContentRegionAvail().x;
            ImGui::SetCursorPosX(startX + std::max(0.0f, (availableWidth - messageWidth) * 0.5f));
            ImGui::TextDisabled("%s", message.c_str());
        }
        drawNoProjectBlankMenu();
        ImGui::EndChild();
        ImGui::EndChild();
        if (!projectToOpen.empty()) {
            OpenProjectFile(projectToOpen);
        }
        return;
    }

    if (m_ProjectEntriesDirty) {
        RefreshProjectEntries();
    }

    ImGui::BeginDisabled(m_ProjectCurrentDirectory.empty());
    if (Ui::GhostButton(Tr("Up"), ImVec2(Ui::S(44.0f), Ui::S(24.0f)))) {
        std::string parentDirectory;
        if (!m_ProjectCurrentDirectory.empty()) {
            parentDirectory = fs::path(m_ProjectCurrentDirectory).parent_path().generic_u8string();
            if (parentDirectory == ".") parentDirectory.clear();
        }
        NavigateProjectFolder(parentDirectory);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();

    const float searchWidth = Ui::S(248.0f);
    const bool hasSearch = !m_ProjectSearch.empty();
    const float searchButtonWidth = hasSearch ? Ui::S(22.0f) : 0.0f;
    const float searchStart = contentMaxX - searchWidth;
    const float crumbWidth = std::max(100.0f, searchStart - ImGui::GetCursorPosX() - 12.0f);
    ImGui::BeginChild("##content_breadcrumbs", ImVec2(crumbWidth, Ui::S(24.0f)),
                      ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushStyleColor(ImGuiCol_TextLink, ImVec4(0.40f, 0.84f, 0.78f, 1.0f));
    if (ImGui::TextLink(Tr("Root"))) {
        NavigateProjectFolder("");
    }
    std::string accumulated;
    std::size_t segmentStart = 0;
    while (segmentStart <= m_ProjectCurrentDirectory.size()) {
        const std::size_t slash = m_ProjectCurrentDirectory.find('/', segmentStart);
        const std::size_t segmentEnd =
            slash == std::string::npos ? m_ProjectCurrentDirectory.size() : slash;
        if (segmentEnd > segmentStart) {
            const std::string segment = m_ProjectCurrentDirectory.substr(segmentStart,
                                                                         segmentEnd - segmentStart);
            accumulated = accumulated.empty() ? segment : accumulated + "/" + segment;
            ImGui::SameLine(0.0f, 5.0f);
            ImGui::TextDisabled("/");
            ImGui::SameLine(0.0f, 5.0f);
            ImGui::PushID(static_cast<int>(segmentStart));
            if (ImGui::TextLink(segment.c_str())) {
                NavigateProjectFolder(accumulated);
            }
            ImGui::PopID();
        }
        if (slash == std::string::npos) break;
        segmentStart = slash + 1;
    }
    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::SetCursorPosX(searchStart);
    ImGui::SetNextItemWidth(searchWidth - searchButtonWidth);
    ImGui::InputTextWithHint("##content_search", Tr("Search files..."), &m_ProjectSearch);
    if (hasSearch) {
        ImGui::SameLine(0.0f, 4.0f);
        if (ImGui::SmallButton("x")) {
            m_ProjectSearch.clear();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", Tr("Clear search"));
        }
    }
    ImGui::Spacing();

    if (!m_ProjectEntriesError.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.48f, 0.42f, 1.0f));
        ImGui::TextWrapped("%s", m_ProjectEntriesError.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::BeginChild("##content_tiles", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
    const auto drawProjectBlankMenu = [&]() {
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
            !ImGui::IsAnyItemHovered() &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            ImGui::OpenPopup("##content_browser_project_blank_menu");
        }
        if (ImGui::BeginPopup("##content_browser_project_blank_menu")) {
            if (ImGui::MenuItem(Tr("New Folder..."))) RequestNewFolder();
            if (ImGui::MenuItem(Tr("New Material..."))) RequestNewMaterial();
            if (ImGui::MenuItem(Tr("New Material Instance..."), nullptr, false,
                                !m_Project.MainMaterialPath().empty())) {
                std::string instanceError;
                if (!CreateMaterialInstanceFromMaterial(m_Project.MainMaterialPath(),
                                                        &instanceError)) {
                    SetStatus(LocalizedErrorText(instanceError), true);
                }
            }
            if (ImGui::MenuItem(Tr("New Blueprint Function..."))) RequestNewFunction();
            ImGui::Separator();
            if (ImGui::MenuItem(Tr("Add File / Model..."))) RequestAddProjectFile();
            if (ImGui::MenuItem(Tr("Add Texture Package..."))) RequestTexturePackages();
            if (!m_ProjectClipboardPath.empty()) {
                if (ImGui::MenuItem(m_ProjectClipboardMove ? Tr("Paste (Move) Here")
                                                            : Tr("Paste (Copy) Here"))) {
                    PasteProjectClipboard(m_ProjectCurrentDirectory);
                }
            }
            if (ImGui::MenuItem(Tr("Refresh"))) {
                RefreshProjectEntries(true);
                RefreshProjectIntegrity();
                SetStatus(m_Project.IsBroken()
                              ? Tr("Project refreshed; missing marked files remain.")
                              : Tr("Project files refreshed."),
                          m_Project.IsBroken());
            }
            ImGui::EndPopup();
        }
    };

    const std::string filter = Trim(m_ProjectSearch);
    const bool searchingFiles = !filter.empty();
    std::vector<const ProjectEntry*> visible;
    visible.reserve(m_ProjectEntries.size());
    for (const ProjectEntry& entry : m_ProjectEntries) {
        if (!searchingFiles || MatchesSearch(entry.name, filter)) {
            visible.push_back(&entry);
        }
    }

    const float tileWidth = Ui::S(120.0f);
    const float tileHeight = Ui::S(108.0f);

    const auto drawTileVisual = [&](ImVec2 cardMin, ImVec2 cardMax, const std::string& name,
                                    const std::string& kind, const std::string& caption,
                                    const fs::path& thumbnailPath, bool selected,
                                    bool hovered, bool missing, ImU32 folderAccent) {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const bool graphiteTheme = Ui::CurrentTheme() == Ui::Theme::Graphite;
        const ImU32 cardBackground = selected
            ? IM_COL32(28, 56, 57, 255)
            : hovered
                ? (graphiteTheme ? IM_COL32(48, 53, 58, 255) : IM_COL32(21, 38, 44, 255))
                : (graphiteTheme ? IM_COL32(37, 41, 45, 255) : IM_COL32(14, 26, 32, 255));
        drawList->AddRectFilled(cardMin, cardMax, cardBackground, 8.0f);
        const ImU32 borderColor = selected
            ? IM_COL32(84, 226, 204, 235)
            : hovered ? IM_COL32(83, 148, 151, 170)
                      : (kind == "folder" ? ((folderAccent & 0x00FFFFFFu) | 0x70000000u)
                                          : IM_COL32(255, 255, 255, 18));
        drawList->AddRect(cardMin, cardMax, borderColor, 8.0f, 0,
                          selected ? 2.0f : 1.0f);
        const ImVec2 thumbMin(cardMin.x + Ui::S(6.0f), cardMin.y + Ui::S(6.0f));
        const ImVec2 thumbMax(cardMax.x - Ui::S(6.0f), thumbMin.y + (tileHeight - Ui::S(46.0f)));
        drawList->AddRectFilled(thumbMin, thumbMax, IM_COL32(20, 23, 26, 255), 6.0f);
        drawList->AddRect(thumbMin, thumbMax, IM_COL32(255, 255, 255, 16), 6.0f);

        bool drewImage = false;
        if (kind == "texture" && !thumbnailPath.empty()) {
            const unsigned int texture = GetThumbnailTexture(thumbnailPath);
            const std::string key = PathUtil::ToUtf8(thumbnailPath);
            const auto found = m_Thumbnails.find(key);
            if (texture != 0 && found != m_Thumbnails.end() && found->second.width > 0 &&
                found->second.height > 0) {
                const float targetAspect = (thumbMax.x - thumbMin.x) / (thumbMax.y - thumbMin.y);
                const float sourceAspect = static_cast<float>(found->second.width) /
                                           static_cast<float>(found->second.height);
                ImVec2 uv0(0.0f, 0.0f);
                ImVec2 uv1(1.0f, 1.0f);
                if (sourceAspect > targetAspect) {
                    const float visible = targetAspect / sourceAspect;
                    uv0.x = (1.0f - visible) * 0.5f;
                    uv1.x = 1.0f - uv0.x;
                } else if (sourceAspect < targetAspect) {
                    const float visible = sourceAspect / targetAspect;
                    uv0.y = (1.0f - visible) * 0.5f;
                    uv1.y = 1.0f - uv0.y;
                }
                drawList->AddImageRounded(
                    static_cast<ImTextureID>(static_cast<intptr_t>(texture)), thumbMin, thumbMax,
                    uv0, uv1, IM_COL32(255, 255, 255, 255), 6.0f);
                drewImage = true;
            }
        } else if (kind == "material" && !thumbnailPath.empty()) {
            const unsigned int materialTexture = GetMaterialThumbnail(thumbnailPath);
            const std::string materialKey = PathUtil::ToUtf8(thumbnailPath);
            const auto found = m_MaterialThumbnails.find(materialKey);
            if (materialTexture != 0) {
                const float destinationWidth = thumbMax.x - thumbMin.x;
                const float destinationHeight = thumbMax.y - thumbMin.y;
                float sourceAspect = 1.0f;
                if (found != m_MaterialThumbnails.end() && found->second.width > 0 &&
                    found->second.height > 0) {
                    sourceAspect = static_cast<float>(found->second.width) /
                                   static_cast<float>(found->second.height);
                }
                // Preserve the offscreen thumbnail's aspect ratio. Without this
                // the square sphere render was stretched into an ellipse by
                // the wide content-browser preview slot.
                ImVec2 imageMin = thumbMin;
                ImVec2 imageMax = thumbMax;
                const float destinationAspect =
                    destinationWidth / std::max(1.0f, destinationHeight);
                if (sourceAspect > destinationAspect) {
                    const float height = destinationWidth / sourceAspect;
                    imageMin.y = thumbMin.y + (destinationHeight - height) * 0.5f;
                    imageMax.y = imageMin.y + height;
                } else if (sourceAspect < destinationAspect) {
                    const float width = destinationHeight * sourceAspect;
                    imageMin.x = thumbMin.x + (destinationWidth - width) * 0.5f;
                    imageMax.x = imageMin.x + width;
                }
                drawList->AddImageRounded(
                    static_cast<ImTextureID>(static_cast<intptr_t>(materialTexture)),
                    imageMin, imageMax, ImVec2(0, 0), ImVec2(1, 1),
                    IM_COL32(255, 255, 255, 255), 6.0f);
                drewImage = true;
            } else {
                drewImage = DrawGraphThumbnail(drawList, thumbMin, thumbMax, thumbnailPath);
            }
        } else if (kind == "function" && !thumbnailPath.empty()) {
            drewImage = DrawGraphThumbnail(drawList, thumbMin, thumbMax, thumbnailPath);
        }

        const ImVec2 iconCenter((thumbMin.x + thumbMax.x) * 0.5f,
                                (thumbMin.y + thumbMax.y) * 0.5f);
        if (!drewImage) {
            if (kind == "folder") {
                DrawFolderGlyph(drawList, iconCenter, 58.0f, hovered, folderAccent);
            } else if (kind == "material") {
                DrawMaterialGlyph(drawList, iconCenter, 58.0f, hovered);
            } else if (kind == "instance") {
                DrawMaterialGlyph(drawList, iconCenter, 58.0f, hovered);
                const ImVec2 badgeMin(iconCenter.x - 18.0f, iconCenter.y + 14.0f);
                drawList->AddRectFilled(badgeMin,
                                        ImVec2(badgeMin.x + 36.0f, badgeMin.y + 17.0f),
                                        IM_COL32(26, 92, 67, 235), 4.0f);
                drawList->AddText(ImVec2(badgeMin.x + 6.0f, badgeMin.y + 1.0f),
                                  IM_COL32(180, 255, 216, 255), "MI");
            } else if (kind == "model") {
                DrawModelGlyph(drawList, iconCenter, 58.0f, hovered);
            } else if (kind == "texture") {
                DrawTextureGlyph(drawList, iconCenter, 58.0f);
            } else if (kind == "project") {
                DrawProjectGlyph(drawList, iconCenter, 58.0f, hovered);
            } else if (kind == "function") {
                DrawFunctionGlyph(drawList, iconCenter, 58.0f, hovered);
            } else {
                DrawDocumentGlyph(drawList, iconCenter, 58.0f, kind == "shader");
            }
        }

        ImFont* smallFont = m_SmallFont ? m_SmallFont : ImGui::GetFont();
        const float smallSize = m_SmallFont ? m_SmallFont->FontSize : ImGui::GetFontSize();
        const std::string shownName = EllipsizeToWidth(name, tileWidth - Ui::S(18.0f));
        const float nameWidth = ImGui::CalcTextSize(shownName.c_str()).x;
        drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                          ImVec2(cardMin.x + (tileWidth - nameWidth) * 0.5f,
                                 thumbMax.y + Ui::S(3.0f)),
                          missing ? IM_COL32(248, 132, 122, 255)
                                  : ImGui::GetColorU32(ImGuiCol_Text),
                          shownName.c_str());

        ImVec4 tint = missing ? ImVec4(0.96f, 0.44f, 0.38f, 1.0f) : KindColor(kind);
        tint.w = 0.88f;
        const ImU32 captionColor = ImGui::ColorConvertFloat4ToU32(tint);
        const ImVec2 captionSize = smallFont->CalcTextSizeA(smallSize, FLT_MAX, 0.0f,
                                                             caption.c_str());
        const float captionX = cardMin.x + (tileWidth - captionSize.x) * 0.5f;
        drawList->AddCircleFilled(ImVec2(captionX - Ui::S(6.0f), cardMax.y - Ui::S(11.0f)), Ui::S(2.2f),
                                  captionColor, 10);
        drawList->AddText(smallFont, smallSize, ImVec2(captionX, cardMax.y - Ui::S(17.0f)),
                          captionColor, caption.c_str());
    };

    if (visible.empty()) {
        ImGui::Dummy(ImVec2(0.0f, 16.0f));
        const std::string message =
            filter.empty() ? Tr("This folder is empty.") : Tr("No matching files.");
        const std::string hint =
            filter.empty() ? Tr("Right-click empty space to create a folder or material, or drop files here from Explorer.") : "";
        const float messageWidth = ImGui::CalcTextSize(message.c_str()).x;
        const float startX = ImGui::GetCursorPosX();
        const float availableWidth = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(startX + std::max(0.0f, (availableWidth - messageWidth) * 0.5f));
        ImGui::TextDisabled("%s", message.c_str());
        if (!hint.empty()) {
            const float hintWidth = ImGui::CalcTextSize(hint.c_str()).x;
            ImGui::SetCursorPosX(startX + std::max(0.0f, (availableWidth - hintWidth) * 0.5f));
            ImGui::TextDisabled("%s", hint.c_str());
        }
    } else {
        const int columns = std::max(
            1, static_cast<int>((ImGui::GetContentRegionAvail().x + gap) / (tileWidth + gap)));
        struct FolderColorChoice {
            const char* label;
            const char* hex;
        };
        static const FolderColorChoice folderColors[] = {
            {"Gold", "#DFAB51"}, {"Blue", "#5AA9E6"}, {"Teal", "#4FD1C5"},
            {"Purple", "#A78BFA"}, {"Red", "#F07167"}, {"Green", "#8BD17C"},
            {"Gray", "#9AA5B1"}
        };
        int index = 0;
        bool pendingMove = false;
        bool pendingPaste = false;
        bool pendingColor = false;
        std::string pendingMoveSource;
        std::string pendingMoveTarget;
        std::string pendingPasteTarget;
        std::string pendingColorPath;
        std::string pendingColorValue;
        for (const ProjectEntry* entry : visible) {
            if (index % columns != 0) ImGui::SameLine(0.0f, gap);
            ImGui::PushID(entry->relativePath.c_str());
            const ImVec2 cardMin = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##file_tile", ImVec2(tileWidth, tileHeight));
            const bool hovered = ImGui::IsItemHovered();
            const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
            if (clicked) {
                const bool shift = ImGui::GetIO().KeyShift;
                const bool ctrl = ImGui::GetIO().KeyCtrl;
                if (shift && !m_ProjectSelectionAnchor.empty()) {
                    int anchorIndex = -1;
                    int clickedIndex = -1;
                    for (int i = 0; i < static_cast<int>(visible.size()); ++i) {
                        if (visible[static_cast<std::size_t>(i)]->relativePath ==
                            m_ProjectSelectionAnchor) {
                            anchorIndex = i;
                        }
                        if (visible[static_cast<std::size_t>(i)]->relativePath ==
                            entry->relativePath) {
                            clickedIndex = i;
                        }
                    }
                    if (anchorIndex >= 0 && clickedIndex >= 0) {
                        m_ProjectSelectedEntries.clear();
                        const int first = std::min(anchorIndex, clickedIndex);
                        const int last = std::max(anchorIndex, clickedIndex);
                        for (int i = first; i <= last; ++i) {
                            const ProjectEntry* rangeEntry = visible[static_cast<std::size_t>(i)];
                            if (!rangeEntry->directory) {
                                m_ProjectSelectedEntries.insert(rangeEntry->relativePath);
                            }
                        }
                    }
                } else if ((shift || ctrl) && !entry->directory) {
                    if (m_ProjectSelectedEntries.count(entry->relativePath) != 0) {
                        m_ProjectSelectedEntries.erase(entry->relativePath);
                    } else {
                        m_ProjectSelectedEntries.insert(entry->relativePath);
                    }
                    m_ProjectSelectionAnchor = entry->relativePath;
                } else {
                    m_ProjectSelectedEntries.clear();
                    if (!entry->directory) {
                        m_ProjectSelectedEntries.insert(entry->relativePath);
                    }
                    m_ProjectSelectionAnchor = entry->relativePath;
                }
                m_ProjectSelectedEntry = entry->relativePath;
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                if (m_ProjectSelectedEntries.count(entry->relativePath) == 0) {
                    m_ProjectSelectedEntries.clear();
                    if (!entry->directory) {
                        m_ProjectSelectedEntries.insert(entry->relativePath);
                    }
                    m_ProjectSelectionAnchor = entry->relativePath;
                    m_ProjectSelectedEntry = entry->relativePath;
                }
            }

            if (entry->kind == "texture") {
                if (ImGui::BeginDragDropSource()) {
                    std::string portable = entry->relativePath;
                    const fs::path absolute = m_Project.ResolveRelative(entry->relativePath);
                    if (absolute.empty() || !m_Project.Contains(absolute)) {
                        portable = PathUtil::MakePortablePath(absolute, m_ContentRoot);
                    }
                    ImGui::SetDragDropPayload("MF_TEXTURE_ASSET", portable.c_str(),
                                              portable.size() + 1);
                    ImGui::Text("%s", entry->name.c_str());
                    ImGui::TextDisabled("%s", Tr("Drop into the graph to create a TextureSample. Drop on a folder to move it."));
                    ImGui::EndDragDropSource();
                }
            } else if (entry->kind == "function") {
                if (ImGui::BeginDragDropSource()) {
                    const fs::path absolute = m_Project.ResolveRelative(entry->relativePath);
                    const std::string path = PathUtil::ToUtf8(absolute);
                    ImGui::SetDragDropPayload("MF_FUNCTION_ASSET", path.c_str(),
                                              path.size() + 1);
                    ImGui::Text("%s", entry->name.c_str());
                    ImGui::TextDisabled("%s", Tr("Drop into a material graph to create a function call; drop on a folder to move it."));
                    ImGui::EndDragDropSource();
                }
            } else {
                if (ImGui::BeginDragDropSource()) {
                    ImGui::SetDragDropPayload("MF_PROJECT_ASSET", entry->relativePath.c_str(),
                                              entry->relativePath.size() + 1);
                    ImGui::Text("%s", entry->name.c_str());
                    ImGui::TextDisabled("%s", Tr("Drop on a folder to move it."));
                    ImGui::EndDragDropSource();
                }
            }

            if (entry->directory) {
                if (ImGui::BeginDragDropTarget()) {
                    const ImGuiPayload* payload = ImGui::GetDragDropPayload();
                    const bool hasAssetPayload =
                        payload && payload->Data && payload->DataSize > 1 &&
                        (payload->IsDataType("MF_PROJECT_ASSET") ||
                         payload->IsDataType("MF_TEXTURE_ASSET") ||
                         payload->IsDataType("MF_FUNCTION_ASSET"));
                    if (hasAssetPayload) {
                        ImGui::GetWindowDrawList()->AddRect(
                            cardMin, ImVec2(cardMin.x + tileWidth, cardMin.y + tileHeight),
                            IM_COL32(255, 171, 74, 240), 8.0f, 0, 2.0f);
                    }
                    std::string source;
                    if (const ImGuiPayload* dropped =
                            ImGui::AcceptDragDropPayload("MF_PROJECT_ASSET")) {
                        if (dropped->Data && dropped->DataSize > 1) {
                            source = static_cast<const char*>(dropped->Data);
                        }
                    } else if (const ImGuiPayload* dropped =
                                   ImGui::AcceptDragDropPayload("MF_TEXTURE_ASSET")) {
                        if (dropped->Data && dropped->DataSize > 1) {
                            source = static_cast<const char*>(dropped->Data);
                        }
                    } else if (const ImGuiPayload* dropped =
                                   ImGui::AcceptDragDropPayload("MF_FUNCTION_ASSET")) {
                        if (dropped->Data && dropped->DataSize > 1) {
                            const fs::path absolute =
                                PathUtil::FromUtf8(static_cast<const char*>(dropped->Data));
                            source = m_Project.RelativePathOf(absolute);
                        }
                    }
                    if (!source.empty() && source != entry->relativePath) {
                        pendingMove = true;
                        pendingMoveSource = source;
                        pendingMoveTarget = entry->relativePath;
                    }
                    ImGui::EndDragDropTarget();
                }
            }

            bool selected = m_ProjectSelectedEntries.count(entry->relativePath) > 0 ||
                            (m_ProjectSelectedEntries.empty() &&
                             entry->relativePath == m_ProjectSelectedEntry);
            if (selected && m_ScrollProjectSelection) {
                ImGui::SetScrollHereY(0.5f);
                m_ScrollProjectSelection = false;
            }
            fs::path thumbnailPath;
            if (entry->kind == "texture" || entry->kind == "material" ||
                entry->kind == "function") {
                thumbnailPath = m_Project.ResolveRelative(entry->relativePath);
            }
            const std::string caption = KindLabel(entry->kind);
            const ImU32 folderAccent = FolderAccentColor(
                m_Project.FolderColor(entry->relativePath), IM_COL32(223, 171, 81, 255));
            drawTileVisual(cardMin, ImVec2(cardMin.x + tileWidth, cardMin.y + tileHeight),
                           entry->name, entry->kind, caption, thumbnailPath, selected, hovered,
                           false, folderAccent);
            const UeSyncEntry* ueSync =
                entry->directory ? nullptr : m_UeSync.Find(entry->relativePath);
            if (ueSync) {
                const ImVec2 badgeMin(cardMin.x + tileWidth - Ui::S(30.0f),
                                      cardMin.y + Ui::S(6.0f));
                const ImU32 badgeColor =
                    ueSync->state == "pushed"
                        ? IM_COL32(38, 112, 88, 235)
                        : (ueSync->state == "pulled" ? IM_COL32(88, 90, 140, 235)
                                                     : IM_COL32(112, 92, 48, 235));
                ImGui::GetWindowDrawList()->AddRectFilled(
                    badgeMin, ImVec2(badgeMin.x + Ui::S(24.0f), badgeMin.y + Ui::S(16.0f)),
                    badgeColor, 4.0f);
                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(badgeMin.x + Ui::S(4.0f), badgeMin.y + Ui::S(1.0f)),
                    IM_COL32(236, 250, 246, 255), "UE");
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::BeginTooltip();
                ImGui::Text("%s%s", entry->relativePath.c_str(), entry->directory ? "/" : "");
                if (!entry->directory) {
                    ImGui::TextDisabled("%s  |  %s", KindLabel(entry->kind).c_str(),
                                        HumanReadableSize(entry->size).c_str());
                    if (ueSync) {
                        ImGui::Separator();
                        ImGui::Text("%s  %s", Tr("UE sync:"), ueSync->state.c_str());
                        ImGui::TextDisabled("%s", ueSync->uePath.c_str());
                        ImGui::TextDisabled("%s", ueSync->updatedAt.c_str());
                    }
                } else {
                    ImGui::TextDisabled("%s", Tr("Folder"));
                }
                if (entry->kind == "material") {
                    ImGui::TextDisabled("%s", Tr("Double-click to open this material graph."));
                } else if (entry->kind == "instance") {
                    ImGui::TextDisabled("%s", Tr("Double-click to edit this material instance."));
                } else if (entry->kind == "function") {
                    ImGui::TextDisabled("%s", Tr("Double-click to open this blueprint function."));
                } else if (entry->kind == "texture") {
                    ImGui::TextDisabled("%s", Tr("Drag into the graph to create a TextureSample."));
                } else if (entry->directory) {
                    ImGui::TextDisabled("%s", Tr("Double-click a folder to browse it."));
                }
                ImGui::EndTooltip();
            }

            bool stopAfterTile = false;
            if (ImGui::BeginPopupContextItem("##tile_context")) {
                std::vector<std::string> actionEntries = EffectiveProjectSelection();
                if (actionEntries.empty() ||
                    m_ProjectSelectedEntries.count(entry->relativePath) == 0) {
                    actionEntries = {entry->relativePath};
                }
                const bool multiple = actionEntries.size() > 1;
                ImGui::TextDisabled("%s%s", entry->name.c_str(),
                                    multiple ? " (+ others)" : "");
                ImGui::Separator();
                if (entry->directory) {
                    if (ImGui::MenuItem(Tr("Open Folder"))) {
                        NavigateProjectFolder(entry->relativePath);
                        stopAfterTile = true;
                    }
                    if (ImGui::MenuItem(Tr("Rename Folder..."))) {
                        RequestRenameProjectFolder(entry->relativePath);
                    }
                    if (ImGui::BeginMenu(Tr("Folder Color"))) {
                        const std::string currentColor = m_Project.FolderColor(entry->relativePath);
                        for (const FolderColorChoice& choice : folderColors) {
                            if (ImGui::MenuItem(Tr(choice.label), nullptr,
                                                currentColor == choice.hex)) {
                                pendingColor = true;
                                pendingColorPath = entry->relativePath;
                                pendingColorValue = choice.hex;
                            }
                        }
                        ImGui::Separator();
                        if (ImGui::MenuItem(Tr("Default"), nullptr, currentColor.empty())) {
                            pendingColor = true;
                            pendingColorPath = entry->relativePath;
                            pendingColorValue.clear();
                        }
                        ImGui::EndMenu();
                    }
                    ImGui::Separator();
                    if (!m_ProjectClipboardPath.empty()) {
                        if (ImGui::MenuItem(m_ProjectClipboardMove ? Tr("Paste (Move) Here")
                                                                    : Tr("Paste (Copy) Here"))) {
                            pendingPaste = true;
                            pendingPasteTarget = entry->relativePath;
                        }
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem(Tr("Delete Folder..."))) {
                        m_DeleteEntryRelative = entry->relativePath;
                        m_DeleteEntryName = entry->name;
                        m_DeleteEntryIsDirectory = true;
                        m_DeleteEntryModalPending = true;
                        stopAfterTile = true;
                    }
                } else {
                    if (entry->kind == "material") {
                        if (ImGui::MenuItem(Tr("Open Material"))) {
                            OpenProjectMaterial(entry->relativePath);
                            stopAfterTile = true;
                        }
                        if (ImGui::MenuItem(Tr("Create Material Instance..."))) {
                            std::string instanceError;
                            if (!CreateMaterialInstanceFromMaterial(entry->relativePath,
                                                                    &instanceError)) {
                                SetStatus(LocalizedErrorText(instanceError), true);
                            }
                            stopAfterTile = true;
                        }
                    } else if (entry->kind == "instance") {
                        if (ImGui::MenuItem(Tr("Open Material Instance"))) {
                            OpenMaterialInstance(m_Project.ResolveRelative(entry->relativePath));
                            stopAfterTile = true;
                        }
                    } else if (entry->kind == "function") {
                        if (ImGui::MenuItem(Tr("Open Blueprint Function"))) {
                            OpenFunctionAsset(m_Project.ResolveRelative(entry->relativePath));
                            stopAfterTile = true;
                        }
                    } else if (entry->kind == "project") {
                        if (ImGui::MenuItem(Tr("Open Project"))) {
                            OpenProjectFile(PathUtil::ToUtf8(
                                m_Project.ResolveRelative(entry->relativePath)));
                            stopAfterTile = true;
                        }
                    }
                    ImGui::Separator();
                    if (entry->kind == "model" &&
                        ToLower(PathUtil::ToUtf8(
                            fs::path(entry->relativePath).extension())) == ".obj") {
                        if (ImGui::MenuItem(Tr("Use as Preview Mesh"))) {
                            m_Settings.previewMeshPath =
                                PathUtil::ToUtf8(m_Project.ResolveRelative(entry->relativePath));
                            m_Settings.previewMeshMode = "Custom";
                            ApplyPreviewSceneSettings();
                            SaveUiSettings();
                            stopAfterTile = true;
                        }
                    } else if (entry->kind == "texture" &&
                               ToLower(PathUtil::ToUtf8(
                                   fs::path(entry->relativePath).extension())) == ".hdr") {
                        if (ImGui::MenuItem(Tr("Use as Environment"))) {
                            m_Settings.previewHdriPath =
                                PathUtil::ToUtf8(m_Project.ResolveRelative(entry->relativePath));
                            ApplyPreviewSceneSettings();
                            SaveUiSettings();
                            stopAfterTile = true;
                        }
                    }
                    if (entry->kind == "texture") {
                        if (ImGui::MenuItem(Tr("View Texture"))) {
                            OpenTextureViewer(m_Project.ResolveRelative(entry->relativePath));
                            stopAfterTile = true;
                        }
                    }
                    ImGui::Separator();
                    if (!multiple && entry->kind != "project") {
                        if (ImGui::MenuItem(Tr("Rename File..."))) {
                            RequestRenameProjectFile(entry->relativePath);
                            stopAfterTile = true;
                        }
                    }
                    if (ImGui::MenuItem(Tr("Move To..."))) {
                        RequestProjectEntryDestination(actionEntries, true);
                        stopAfterTile = true;
                    }
                    if (ImGui::MenuItem(Tr("Copy To..."))) {
                        RequestProjectEntryDestination(actionEntries, false);
                        stopAfterTile = true;
                    }
                    if (!multiple && ImGui::MenuItem(Tr("Duplicate File"))) {
                        DuplicateProjectEntry(entry->relativePath);
                        stopAfterTile = true;
                    }
                    if (multiple && ImGui::MenuItem(Tr("Duplicate Selected Files"))) {
                        DuplicateProjectEntries(actionEntries);
                        stopAfterTile = true;
                    }
                    if (!m_ProjectClipboardPath.empty()) {
                        if (ImGui::MenuItem(m_ProjectClipboardMove ? Tr("Paste (Move) Here")
                                                                    : Tr("Paste (Copy) Here"))) {
                            pendingPaste = true;
                            pendingPasteTarget = m_ProjectCurrentDirectory;
                        }
                    }
                    if (entry->kind != "project") {
                        ImGui::Separator();
                        if (multiple) {
                            if (ImGui::MenuItem(Tr("Delete Selected Files..."))) {
                                RequestDeleteProjectEntries(actionEntries);
                                stopAfterTile = true;
                            }
                        } else if (ImGui::MenuItem(Tr("Delete File..."))) {
                            m_DeleteEntryRelative = entry->relativePath;
                            m_DeleteEntryName = entry->name;
                            m_DeleteEntryIsDirectory = false;
                            m_DeleteEntryModalPending = true;
                            stopAfterTile = true;
                        }
                    }
                }
                ImGui::EndPopup();
            }
            if (stopAfterTile) {
                ImGui::PopID();
                break;
            }
            if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                const std::string relative = entry->relativePath;
                const std::string kind = entry->kind;
                const fs::path absolute = m_Project.ResolveRelative(relative);
                ImGui::PopID();
                if (entry->directory) {
                    NavigateProjectFolder(relative);
                } else if (kind == "project") {
                    OpenProjectFile(PathUtil::ToUtf8(absolute));
                } else if (kind == "material") {
                    OpenProjectMaterial(relative);
                } else if (kind == "instance") {
                    OpenMaterialInstance(absolute);
                } else if (kind == "function") {
                    OpenFunctionAsset(absolute);
                } else if (kind == "texture") {
                    OpenTextureViewer(absolute);
                } else {
                    SetStatus(std::string(Tr("Asset: ")) + relative +
                              Tr(" (stored in this project; not previewable in this build)"));
                }
                break;
            }
            ImGui::PopID();
            ++index;
        }

        if (pendingColor) {
            SetProjectFolderColor(pendingColorPath, pendingColorValue);
        }
        if (pendingMove) {
            MoveProjectEntry(pendingMoveSource, pendingMoveTarget);
        }
        if (pendingPaste) {
            PasteProjectClipboard(pendingPasteTarget);
        }
    }
    drawProjectBlankMenu();
    ImGui::EndChild();
    ImGui::EndChild();
}
void Application::RequestTexturePackages() {
    if (!m_Project.IsOpen()) {
        SetStatus(Tr("Create or open a project before adding a texture package."), true);
        return;
    }
    m_TexturePackages = BuildTexturePackages();
    m_TexturePackagesError.clear();
    if (m_TexturePackages.empty()) {
        SetStatus(Tr("No built-in texture package was found next to the executable."), true);
        return;
    }
    m_OpenTexturePackages = true;
    m_TexturePackagesPending = true;
}

std::vector<Application::TexturePackage> Application::BuildTexturePackages() const {
    std::vector<TexturePackage> packages;
    const fs::path textureDirectory = m_ContentRoot / "assets" / "textures";
    std::error_code ec;
    if (!fs::is_directory(textureDirectory, ec)) {
        return packages;
    }

    std::vector<fs::path> allFiles;
    std::map<std::string, fs::path> fileByStem;
    for (fs::directory_iterator it(textureDirectory, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const fs::path path = it->path();
        if (!IsTextureFile(path)) continue;
        allFiles.push_back(path);
        fileByStem[ToLower(PathUtil::ToUtf8(path.stem()))] = path;
    }
    std::sort(allFiles.begin(), allFiles.end(), [](const fs::path& a, const fs::path& b) {
        return ToLower(PathUtil::ToUtf8(a.filename())) <
               ToLower(PathUtil::ToUtf8(b.filename()));
    });
    if (allFiles.empty()) return packages;

    TexturePackage all;
    all.id = "all";
    all.name = Tr("All Built-in Textures");
    all.description = FormatLocalized("%d texture(s) bundled with MaterialForge.", 
                                      static_cast<int>(allFiles.size()));
    all.files = allFiles;
    packages.push_back(std::move(all));

    std::vector<std::string> categoryOrder;
    std::map<std::string, std::vector<fs::path>> categories;
    std::ifstream manifest(textureDirectory / "default_texture_manifest.csv", std::ios::binary);
    std::string line;
    bool firstLine = true;
    while (std::getline(manifest, line)) {
        if (firstLine) {
            firstLine = false;
            if (line.find("asset") != std::string::npos) continue;
        }
        const std::vector<std::string> fields = SplitCsvLine(line);
        if (fields.size() < 3 || fields[1].empty() || fields[2].empty()) continue;
        std::string asset = ToLower(fields[1]);
        if (!asset.empty() && asset.back() == '"') asset.pop_back();
        const std::string stem = ToLower(PathUtil::ToUtf8(PathUtil::FromUtf8(asset).stem()));
        const auto file = fileByStem.find(stem);
        if (file == fileByStem.end()) continue;
        const std::string category = fields[2];
        if (categories.find(category) == categories.end()) {
            categoryOrder.push_back(category);
        }
        categories[category].push_back(file->second);
    }

    for (const std::string& category : categoryOrder) {
        std::vector<fs::path>& files = categories[category];
        if (files.empty()) continue;
        std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
            return ToLower(PathUtil::ToUtf8(a.filename())) <
                   ToLower(PathUtil::ToUtf8(b.filename()));
        });
        TexturePackage package;
        package.id = category;
        package.name = category;
        package.description = FormatLocalized("%d texture(s).", static_cast<int>(files.size()));
        package.files = std::move(files);
        packages.push_back(std::move(package));
    }
    return packages;
}

bool Application::ImportTexturePackage(const TexturePackage& package, int* imported,
                                       int* skipped, std::string* error) {
    if (imported) *imported = 0;
    if (skipped) *skipped = 0;
    if (!m_Project.IsOpen()) {
        if (error) *error = "Create or open a project before adding a texture package.";
        return false;
    }
    const std::string folder = "textures";
    std::error_code ec;
    fs::path targetFolder = m_Project.ResolveRelative(folder);
    if (targetFolder.empty() || !fs::is_directory(targetFolder, ec)) {
        std::string createError;
        if (!m_Project.CreateFolder("", folder, &createError)) {
            if (error) *error = createError;
            return false;
        }
        targetFolder = m_Project.ResolveRelative(folder);
    }
    if (targetFolder.empty() || !fs::is_directory(targetFolder, ec)) {
        if (error) *error = "Could not create the textures folder.";
        return false;
    }

    int importedCount = 0;
    int skippedCount = 0;
    std::string firstError;
    for (const fs::path& source : package.files) {
        const fs::path destination = targetFolder / source.filename();
        if (fs::is_regular_file(destination, ec)) {
            ++skippedCount;
            continue;
        }
        std::string importedRelative;
        std::string importError;
        if (!m_Project.ImportFile(source, folder, &importedRelative, &importError)) {
            if (firstError.empty()) firstError = importError;
            continue;
        }
        ++importedCount;
    }
    std::string saveError;
    if (!m_Project.Save(&saveError)) {
        if (error) *error = saveError;
        return false;
    }
    m_ProjectEntriesDirty = true;
    RefreshProjectIntegrity();
    RefreshProjectEntries();
    if (imported) *imported = importedCount;
    if (skipped) *skipped = skippedCount;
    if (!firstError.empty() && error) *error = firstError;
    return firstError.empty();
}

void Application::DrawTexturePackagesModal() {
    ImGui::SetNextWindowSize(ImVec2(Ui::S(620.0f), Ui::S(520.0f)), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(Tr("Add Texture Package"), &m_OpenTexturePackages,
                                ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }
    ImGui::TextWrapped("%s", Tr("Choose one or more built-in texture packs. Files are copied into a textures folder in the current project."));
    ImGui::Spacing();
    if (Ui::GhostButton(Tr("Select All"), ImVec2(Ui::S(96.0f), Ui::S(24.0f)))) {
        for (TexturePackage& package : m_TexturePackages) package.selected = true;
    }
    ImGui::SameLine();
    if (Ui::GhostButton(Tr("Clear Selection"), ImVec2(Ui::S(112.0f), Ui::S(24.0f)))) {
        for (TexturePackage& package : m_TexturePackages) package.selected = false;
    }
    ImGui::Spacing();

    const float footerHeight = Ui::S(100.0f);
    ImGui::BeginChild("##texture_packages", ImVec2(0.0f, -footerHeight),
                      ImGuiChildFlags_Borders);
    for (TexturePackage& package : m_TexturePackages) {
        ImGui::PushID(package.id.c_str());
        ImGui::Checkbox(package.name.c_str(), &package.selected);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", FormatLocalized("(%d files)", static_cast<int>(package.files.size())).c_str());
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip("%s", package.description.c_str());
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    if (!m_TexturePackagesError.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.45f, 0.39f, 1.0f));
        ImGui::TextWrapped("%s", LocalizedErrorText(m_TexturePackagesError).c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Separator();
    const float width = Ui::S(112.0f);
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - width * 2.0f -
                         ImGui::GetStyle().ItemSpacing.x);
    bool cancel = ImGui::Button(Tr("Cancel"), ImVec2(width, Ui::S(30.0f)));
    ImGui::SameLine();
    bool selectedAny = false;
    for (const TexturePackage& package : m_TexturePackages) selectedAny |= package.selected;
    ImGui::BeginDisabled(!selectedAny);
    const bool confirm = Ui::AccentButton(Tr("Add Selected"), ImVec2(width, Ui::S(30.0f)));
    ImGui::EndDisabled();
    if (cancel) {
        m_OpenTexturePackages = false;
        ImGui::CloseCurrentPopup();
    } else if (confirm) {
        int imported = 0;
        int skipped = 0;
        std::string firstError;
        for (const TexturePackage& package : m_TexturePackages) {
            if (!package.selected) continue;
            std::string packageError;
            int packageImported = 0;
            int packageSkipped = 0;
            if (!ImportTexturePackage(package, &packageImported, &packageSkipped,
                                      &packageError) &&
                firstError.empty()) {
                firstError = packageError;
            }
            imported += packageImported;
            skipped += packageSkipped;
        }
        if (imported > 0) {
            m_TexturePackagesError.clear();
            m_OpenTexturePackages = false;
            NavigateProjectFolder("textures");
            m_ProjectSelectedEntry = "textures";
            m_ScrollProjectSelection = true;
            SetStatus(FormatLocalized("Imported %d texture(s); %d already existed.",
                                      imported, skipped));
            ImGui::CloseCurrentPopup();
        } else if (!firstError.empty()) {
            m_TexturePackagesError = firstError;
        } else {
            m_TexturePackagesError = "All selected textures already exist in this project.";
        }
    }
    ImGui::EndPopup();
}

void Application::OpenTexturePicker(NodeId nodeId, PinId pinId) {
    m_TexturePickerNode = nodeId;
    m_TexturePickerPin = pinId;
    m_TexturePickerSearch[0] = '\0';
    m_TexturePickerItemsDirty = true;
    m_OpenTexturePicker = true;
    m_TexturePickerPending = true;
}

std::vector<Application::TexturePickerItem> Application::CollectTexturePickerItems() {
    std::vector<TexturePickerItem> items;
    std::error_code ec;
    if (!m_Project.IsOpen()) {
        return items;
    }
    for (fs::recursive_directory_iterator it(m_Project.RootPath(),
                                             fs::directory_options::skip_permission_denied, ec),
         end;
         !ec && it != end; it.increment(ec)) {
        if (it->is_directory(ec)) {
            if (it->path().filename() == ".materialforge") it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec) || !IsTextureFile(it->path())) continue;
        TexturePickerItem item;
        item.path = it->path();
        item.label = PathUtil::ToUtf8(it->path().filename());
        item.relative = m_Project.RelativePathOf(it->path());
        item.projectAsset = true;
        items.push_back(std::move(item));
    }
    std::sort(items.begin(), items.end(), [](const TexturePickerItem& a,
                                             const TexturePickerItem& b) {
        return ToLower(a.label) < ToLower(b.label);
    });
    return items;
}

std::string Application::ImportDefaultTextureIntoProject(const fs::path& source,
                                                         std::string* error) {
    if (error) error->clear();
    if (!m_Project.IsOpen()) {
        return PathUtil::MakePortablePath(source, m_ContentRoot);
    }
    const std::string folder = "textures";
    std::error_code ec;
    fs::path directory = m_Project.ResolveRelative(folder);
    if (directory.empty() || !fs::is_directory(directory, ec)) {
        std::string createError;
        if (!m_Project.CreateFolder("", folder, &createError)) {
            if (error) *error = createError;
            return {};
        }
        directory = m_Project.ResolveRelative(folder);
    }
    const std::string relative = folder + "/" + PathUtil::ToUtf8(source.filename());
    const fs::path destination = m_Project.ResolveRelative(relative);
    if (!destination.empty() && fs::is_regular_file(destination, ec)) {
        m_Project.AddResource(relative, "texture", true);
        std::string saveError;
        m_Project.Save(&saveError);
        return relative;
    }
    std::string imported;
    if (!m_Project.ImportFile(source, folder, &imported, error)) {
        return {};
    }
    m_ProjectEntriesDirty = true;
    return imported;
}

bool Application::ApplyTexturePickerItem(const TexturePickerItem& item, std::string* error) {
    if (error) error->clear();
    if (!m_GraphView || m_TexturePickerNode == 0 || m_TexturePickerPin == 0) {
        if (error) *error = "The texture input is no longer available.";
        return false;
    }
    std::string portable;
    if (item.projectAsset) {
        portable = item.relative;
    } else {
        portable = ImportDefaultTextureIntoProject(item.path, error);
        if (portable.empty()) return false;
    }
    if (!m_GraphView->ApplyTextureToInput(m_TexturePickerNode, m_TexturePickerPin, portable,
                                          error)) {
        return false;
    }
    m_ProjectEntriesDirty = true;
    m_TexturePickerNode = 0;
    m_TexturePickerPin = 0;
    SetStatus(FormatLocalized("Texture set to %s", portable.c_str()));
    return true;
}

void Application::DrawTexturePickerModal() {
    ImGui::SetNextWindowSize(ImVec2(Ui::S(700.0f), Ui::S(560.0f)), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(Tr("Select Texture"), &m_OpenTexturePicker,
                                ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }
    if (m_TexturePickerItemsDirty) {
        m_TexturePickerItems = CollectTexturePickerItems();
        m_TexturePickerItemsDirty = false;
    }
    ImGui::TextWrapped("%s", Tr("Choose a texture from this project. Add a texture package from the Content Browser first if the list is empty."));
    ImGui::Spacing();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##texture_picker_search", Tr("Search textures..."),
                             m_TexturePickerSearch, sizeof(m_TexturePickerSearch));
    const std::string filter = ToLower(Trim(m_TexturePickerSearch));
    ImGui::Spacing();
    const float footerHeight = Ui::S(52.0f);
    ImGui::BeginChild("##texture_picker_list", ImVec2(0.0f, -footerHeight),
                      ImGuiChildFlags_Borders);
    const TexturePickerItem* chosen = nullptr;
    int visibleCount = 0;
    for (const TexturePickerItem& item : m_TexturePickerItems) {
        const std::string search = ToLower(item.label + " " + item.relative);
        if (!filter.empty() && search.find(filter) == std::string::npos) continue;
        ++visibleCount;
        ImGui::PushID(item.relative.c_str());
        const ImVec2 rowMin = ImGui::GetCursorScreenPos();
        const float rowHeight = Ui::S(38.0f);
        const bool clicked = ImGui::Selectable("##texture_row", false,
                                               ImGuiSelectableFlags_AllowDoubleClick,
                                               ImVec2(0.0f, rowHeight));
        const ImVec2 rowMax(rowMin.x + ImGui::GetContentRegionAvail().x, rowMin.y + rowHeight);
        const unsigned int texture = GetThumbnailTexture(item.path);
        if (texture != 0) {
            ImGui::GetWindowDrawList()->AddImage(
                static_cast<ImTextureID>(static_cast<intptr_t>(texture)),
                ImVec2(rowMin.x + Ui::S(4.0f), rowMin.y + Ui::S(3.0f)),
                ImVec2(rowMin.x + Ui::S(36.0f), rowMin.y + Ui::S(35.0f)));
        } else {
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(rowMin.x + Ui::S(4.0f), rowMin.y + Ui::S(3.0f)),
                ImVec2(rowMin.x + Ui::S(36.0f), rowMin.y + Ui::S(35.0f)),
                IM_COL32(40, 45, 50, 255), Ui::S(3.0f));
        }
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(rowMin.x + Ui::S(44.0f), rowMin.y + Ui::S(3.0f)),
            ImGui::GetColorU32(ImGuiCol_Text), item.label.c_str());
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(rowMin.x + Ui::S(44.0f), rowMin.y + Ui::S(20.0f)),
            ImGui::GetColorU32(ImGuiCol_TextDisabled),
            (item.projectAsset ? item.relative : std::string(Tr("Built-in")) + "  " + item.relative).c_str());
        if (clicked) {
            chosen = &item;
        }
        ImGui::PopID();
    }
    if (visibleCount == 0) {
        ImGui::TextDisabled("%s", m_Project.IsOpen() ? Tr("No matching textures.")
                                                     : Tr("Open a project and add a texture package first."));
    }
    ImGui::EndChild();
    if (chosen) {
        std::string error;
        if (!ApplyTexturePickerItem(*chosen, &error)) {
            SetStatus(LocalizedErrorText(error), true);
        } else {
            m_OpenTexturePicker = false;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::Separator();
    const float width = Ui::S(112.0f);
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - width);
    if (ImGui::Button(Tr("Cancel"), ImVec2(width, Ui::S(28.0f)))) {
        m_TexturePickerNode = 0;
        m_TexturePickerPin = 0;
        m_OpenTexturePicker = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void Application::OpenTextureViewer(const std::filesystem::path& path) {
    ImageData image;
    std::string error;
    if (!LoadImageRGBA(PathUtil::ToUtf8(path), image, &error)) {
        SetStatus(LocalizedErrorText(error), true);
        return;
    }
    CloseTextureViewerTexture();
    m_TextureViewerPath = path;
    m_TextureViewerPixels = std::move(image.rgba);
    m_TextureViewerWidth = image.width;
    m_TextureViewerHeight = image.height;
    m_TextureViewerChannel = 0;
    m_TextureViewerZoom = 1.0f;
    m_TextureViewerPan = ImVec2(0.0f, 0.0f);
    m_TextureViewerError.clear();
    m_TextureViewerNeedsTexture = true;
    m_OpenTextureViewer = true;
}

void Application::CloseTextureViewerTexture() {
    if (m_TextureViewerTexture != 0) {
        glDeleteTextures(1, &m_TextureViewerTexture);
        m_TextureViewerTexture = 0;
    }
}

void Application::RebuildTextureViewerTexture() {
    if (!m_TextureViewerNeedsTexture) return;
    m_TextureViewerNeedsTexture = false;
    CloseTextureViewerTexture();
    const std::size_t pixelCount = static_cast<std::size_t>(m_TextureViewerWidth) *
                                   static_cast<std::size_t>(m_TextureViewerHeight);
    if (pixelCount == 0 || m_TextureViewerPixels.size() < pixelCount * 4u) return;
    std::vector<unsigned char> preview(pixelCount * 4u);
    for (std::size_t i = 0; i < pixelCount; ++i) {
        const unsigned char r = m_TextureViewerPixels[i * 4u + 0];
        const unsigned char g = m_TextureViewerPixels[i * 4u + 1];
        const unsigned char b = m_TextureViewerPixels[i * 4u + 2];
        const unsigned char a = m_TextureViewerPixels[i * 4u + 3];
        unsigned char outR = r;
        unsigned char outG = g;
        unsigned char outB = b;
        unsigned char outA = a;
        switch (m_TextureViewerChannel) {
        case 1: // RGB, hide alpha
            outA = 255;
            break;
        case 2: // R
            outG = outB = r;
            outA = 255;
            break;
        case 3: // G
            outR = outB = g;
            outA = 255;
            break;
        case 4: // B
            outR = outG = b;
            outA = 255;
            break;
        case 5: // A
            outR = outG = outB = a;
            outA = 255;
            break;
        default: // RGBA
            break;
        }
        preview[i * 4u + 0] = outR;
        preview[i * 4u + 1] = outG;
        preview[i * 4u + 2] = outB;
        preview[i * 4u + 3] = outA;
    }
    glGenTextures(1, &m_TextureViewerTexture);
    glBindTexture(GL_TEXTURE_2D, m_TextureViewerTexture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_TextureViewerWidth, m_TextureViewerHeight, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, preview.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void Application::DrawTextureViewer() {
    if (!m_OpenTextureViewer) return;
    if (m_TextureViewerNeedsTexture) RebuildTextureViewerTexture();
    ImGui::SetNextWindowSize(ImVec2(Ui::S(900.0f), Ui::S(700.0f)), ImGuiCond_FirstUseEver);
    bool open = m_OpenTextureViewer;
    const std::string title =
        PathUtil::ToUtf8(m_TextureViewerPath.filename()) + "###TextureViewer";
    if (!ImGui::Begin(title.c_str(), &open,
                      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        if (!open) {
            m_OpenTextureViewer = false;
            CloseTextureViewerTexture();
        }
        return;
    }
    ImGui::TextDisabled("%s", PathUtil::ToUtf8(m_TextureViewerPath).c_str());
    ImGui::Text("%d x %d  |  %s", m_TextureViewerWidth, m_TextureViewerHeight,
                m_TextureViewerPath.extension().u8string().c_str());
    static const char* const channels[] = {"RGBA", "RGB", "R", "G", "B", "A"};
    ImGui::SetNextItemWidth(Ui::S(120.0f));
    if (ImGui::Combo("##texture_channel", &m_TextureViewerChannel, channels,
                     static_cast<int>(sizeof(channels) / sizeof(channels[0])))) {
        m_TextureViewerNeedsTexture = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Ui::S(180.0f));
    if (ImGui::SliderFloat(Tr("Zoom"), &m_TextureViewerZoom, 0.1f, 16.0f, "%.2fx")) {
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr("Fit"))) {
        m_TextureViewerZoom = 1.0f;
        m_TextureViewerPan = ImVec2(0.0f, 0.0f);
    }
    ImGui::Separator();
    const ImVec2 area = ImGui::GetContentRegionAvail();
    if (area.x < 4.0f || area.y < 4.0f) {
        ImGui::End();
        if (!open) {
            m_OpenTextureViewer = false;
            CloseTextureViewerTexture();
        }
        return;
    }
    ImGui::InvisibleButton("##texture_view", area);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    if (hovered && ImGui::GetIO().MouseWheel != 0.0f) {
        m_TextureViewerZoom = std::clamp(
            m_TextureViewerZoom + ImGui::GetIO().MouseWheel * 0.1f, 0.1f, 16.0f);
    }
    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f)) {
        m_TextureViewerPan.x += ImGui::GetIO().MouseDelta.x;
        m_TextureViewerPan.y += ImGui::GetIO().MouseDelta.y;
    }
    const ImVec2 imageMin = ImGui::GetItemRectMin();
    const ImVec2 imageMax = ImGui::GetItemRectMax();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(imageMin, imageMax, IM_COL32(8, 12, 16, 255), Ui::S(5.0f));
    // Checkerboard behind RGBA/alpha views so transparency is visible.
    if (m_TextureViewerChannel == 0 || m_TextureViewerChannel == 5) {
        const float cell = Ui::S(12.0f);
        for (float y = imageMin.y; y < imageMax.y; y += cell) {
            for (float x = imageMin.x; x < imageMax.x; x += cell) {
                const int ix = static_cast<int>((x - imageMin.x) / cell);
                const int iy = static_cast<int>((y - imageMin.y) / cell);
                drawList->AddRectFilled(
                    ImVec2(x, y),
                    ImVec2(std::min(x + cell, imageMax.x), std::min(y + cell, imageMax.y)),
                    ((ix + iy) & 1) ? IM_COL32(42, 48, 52, 255) : IM_COL32(58, 64, 68, 255));
            }
        }
    }
    if (m_TextureViewerTexture != 0 && m_TextureViewerWidth > 0 &&
        m_TextureViewerHeight > 0) {
        const float fit = std::min((imageMax.x - imageMin.x) / m_TextureViewerWidth,
                                   (imageMax.y - imageMin.y) / m_TextureViewerHeight);
        const float scale = fit * m_TextureViewerZoom;
        const ImVec2 center((imageMin.x + imageMax.x) * 0.5f + m_TextureViewerPan.x,
                            (imageMin.y + imageMax.y) * 0.5f + m_TextureViewerPan.y);
        const ImVec2 half(m_TextureViewerWidth * scale * 0.5f,
                          m_TextureViewerHeight * scale * 0.5f);
        drawList->AddImage(static_cast<ImTextureID>(static_cast<intptr_t>(m_TextureViewerTexture)),
                           ImVec2(center.x - half.x, center.y - half.y),
                           ImVec2(center.x + half.x, center.y + half.y),
                           ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
    }
    drawList->AddRect(imageMin, imageMax, IM_COL32(255, 255, 255, 22), Ui::S(5.0f), 0, 1.0f);
    ImGui::End();
    if (!open) {
        m_OpenTextureViewer = false;
        CloseTextureViewerTexture();
    }
}

void Application::StartNodePreview(NodeId nodeId, PinId pinId) {
    if (m_DocumentKind != DocumentKind::Material) return;
    Node* node = m_Graph.FindNode(nodeId);
    if (!node || node->IsMaterialOutput()) return;
    TypeResolver resolver(m_Graph);
    Pin* output = pinId != 0 ? m_Graph.FindPin(pinId) : nullptr;
    if (!output || output->nodeId != node->id || output->direction != PinDirection::Output) {
        output = nullptr;
        for (Pin& candidate : node->outputs) {
            const PinType type = resolver.GetEffectiveType(candidate);
            if (IsNumericPin(type) && type != PinType::Wildcard) {
                output = &candidate;
                break;
            }
        }
    }
    if (!output) {
        SetStatus(Tr("This node has no numeric output to preview."), true);
        return;
    }
    m_NodePreviewActive = true;
    m_NodePreviewNode = nodeId;
    m_NodePreviewPin = output->id;
    m_NodePreviewRevision = std::numeric_limits<std::uint64_t>::max();
    m_ForceShaderRefresh = true;
    RefreshNodePreview();
}

void Application::StopNodePreview() {
    if (!m_NodePreviewActive) return;
    m_NodePreviewActive = false;
    m_NodePreviewGraph = MaterialGraph{};
    m_NodePreviewNode = 0;
    m_NodePreviewPin = 0;
    m_NodePreviewRevision = 0;
    m_LastContentRevision = 0;
    m_ForceShaderRefresh = true;
    SetStatus(Tr("Node preview closed."));
}

void Application::RefreshNodePreview() {
    if (!m_NodePreviewActive) return;
    Node* node = m_Graph.FindNode(m_NodePreviewNode);
    if (!node) {
        StopNodePreview();
        return;
    }
    TypeResolver resolver(m_Graph);
    Pin* source = m_Graph.FindPin(m_NodePreviewPin);
    if (!source || source->nodeId != node->id || source->direction != PinDirection::Output) {
        source = nullptr;
        for (Pin& candidate : node->outputs) {
            const PinType type = resolver.GetEffectiveType(candidate);
            if (IsNumericPin(type) && type != PinType::Wildcard) {
                source = &candidate;
                break;
            }
        }
    }
    if (!source) {
        StopNodePreview();
        return;
    }
    m_NodePreviewPin = source->id;

    MaterialGraph preview = m_Graph;
    Node* output = preview.FindMaterialOutput();
    Pin* previewSource = preview.FindPin(source->id);
    if (!output || !previewSource) {
        StopNodePreview();
        return;
    }
    for (Pin& input : output->inputs) {
        preview.DisconnectInput(input.id);
    }
    Pin* previewTarget = output->FindInput("Emissive Color");
    if (!previewTarget) previewTarget = output->FindInput("Base Color");
    if (!previewTarget) {
        StopNodePreview();
        return;
    }
    std::string error;
    if (!preview.Connect(previewSource->id, previewTarget->id, &error)) {
        SetStatus(LocalizedErrorText(error), true);
        StopNodePreview();
        return;
    }

    const fs::path baseDirectory =
        m_Project.IsOpen() ? m_Project.RootPath() : m_DocumentPath.parent_path();
    GeneratedShader shader = GlslGenerator().Generate(preview, m_GlslVersion, baseDirectory,
                                                       PreviewBuffer::Lit, PreviewLighting());
    m_ShaderGenerated = shader.success;
    m_ShaderError = shader.diagnostic;
    if (!shader.success) {
        m_ShaderCompiled = false;
        m_PreviewRenderer.SetMaterial(shader, nullptr);
        SetStatus(Tr("Node preview shader generation failed. Open the GLSL tab for details."), true);
        m_NodePreviewGraph = std::move(preview);
        m_NodePreviewRevision = m_Graph.ContentRevision();
        m_LastContentRevision = m_NodePreviewRevision;
        m_ForceShaderRefresh = false;
        m_PreviewNeedsRender = true;
        return;
    }

    GeneratedShader runtimeShader = shader;
    for (ShaderTextureBinding& texture : runtimeShader.textures) {
        if (texture.assetPath.empty()) continue;
        fs::path resolved;
        if (m_Project.IsOpen()) {
            const fs::path projectCandidate = m_Project.ResolveRelative(texture.assetPath);
            std::error_code ec;
            if (!projectCandidate.empty() && fs::is_regular_file(projectCandidate, ec)) {
                resolved = projectCandidate;
            }
        }
        if (resolved.empty()) resolved = PathUtil::ResolveResource(texture.assetPath);
        texture.assetPath = PathUtil::ToUtf8(resolved);
    }
    std::string compileError;
    m_ShaderCompiled = m_PreviewRenderer.SetMaterial(runtimeShader, &compileError);
    m_ShaderError = m_ShaderCompiled ? std::string() : compileError;
    m_GeneratedShader = std::move(shader);
    m_NodePreviewGraph = std::move(preview);
    m_NodePreviewRevision = m_Graph.ContentRevision();
    m_LastContentRevision = m_NodePreviewRevision;
    m_ForceShaderRefresh = false;
    m_PreviewNeedsRender = true;
    if (m_ShaderCompiled) {
        SetStatus(FormatLocalized("Previewing node: %s", NodeDisplayName(*node)));
    } else {
        SetStatus(Tr("Node preview shader compile failed. Open the GLSL tab for details."), true);
    }
}

void Application::DrawNodeEditorPanel(float width, float height) {
    ImGui::BeginChild("##GraphPanel", ImVec2(width, height), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    if (m_DocumentKind == DocumentKind::Instance) {
        Ui::SectionLabel(Tr("MATERIAL INSTANCE"));
        const float saveWidth = Ui::S(112.0f);
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - saveWidth);
        if (Ui::AccentButton(Tr("Save"), ImVec2(saveWidth, Ui::S(30.0f)))) {
            RequestSave(false);
        }
        PushBoldFont();
        ImGui::TextUnformatted(m_Instance.Name().empty()
                                   ? PathUtil::ToUtf8(m_DocumentPath.stem()).c_str()
                                   : m_Instance.Name().c_str());
        PopBoldFont();
        if (m_Graph.IsDirty()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.98f, 0.72f, 0.31f, 1.0f), "*");
        }
        ImGui::TextDisabled("%s", Tr("Parent material"));
        ImGui::SameLine();
        ImGui::TextUnformatted(m_Instance.ParentMaterial().c_str());
        ImGui::TextDisabled(Tr("%zu parameters  |  %zu overrides"),
                            m_InstanceParameters.size(),
                            m_Instance.Overrides().size());
        ImGui::Separator();
        ImGui::BeginChild("##instance_editor_scroll", ImVec2(0.0f, 0.0f),
                          ImGuiChildFlags_None,
                          ImGuiWindowFlags_AlwaysVerticalScrollbar);
        DrawInstancePanel();
        ImGui::EndChild();
        ImGui::EndChild();
        return;
    }

    Ui::SectionLabel(m_DocumentKind == DocumentKind::Function ? Tr("BLUEPRINT FUNCTION")
                                                              : Tr("MATERIAL GRAPH"));
    const float buttonWidth = Ui::S(78.0f);
    const float saveWidth = Ui::S(112.0f);
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float buttonsStart = ImGui::GetWindowContentRegionMax().x - saveWidth -
                               buttonWidth * 2.0f - spacing * 3.0f;
    ImGui::SameLine();
    ImGui::SetCursorPosX(buttonsStart);
    if (Ui::GhostButton(Tr("Frame All"), ImVec2(buttonWidth, Ui::S(24.0f)))) {
        m_GraphView->FrameAll();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(m_GraphView->SelectedNodeId() == 0);
    if (Ui::GhostButton(Tr("Selection"), ImVec2(buttonWidth, Ui::S(24.0f)))) {
        m_GraphView->FrameSelection();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (Ui::AccentButton(Tr("Save"), ImVec2(saveWidth, Ui::S(30.0f)))) {
        RequestSave(false);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", Tr("Save the active document (Ctrl+S)"));
    }

    PushBoldFont();
    if (m_DocumentKind == DocumentKind::Instance) {
        const std::string title = m_Instance.Name().empty()
                                      ? PathUtil::ToUtf8(m_DocumentPath.stem())
                                      : m_Instance.Name();
        ImGui::TextUnformatted(title.c_str());
    } else {
        ImGui::TextUnformatted(m_Graph.Settings().name.c_str());
    }
    PopBoldFont();
    if (m_Graph.IsDirty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.98f, 0.72f, 0.31f, 1.0f), "*");
    }
    ImGui::SameLine();
    const GraphStats stats = m_Graph.ComputeStats();
    ImGui::TextDisabled(Tr("  %zu nodes  |  %zu links  |  %zu params"), stats.nodeCount,
                        stats.linkCount, stats.parameterCount);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", Tr("Right-click or double-click the canvas to add a node. Drag a pin to connect. Del removes selected nodes. Hold 3/4/T/V/S + left-click for quick nodes. Ctrl+C / Ctrl+V copies at the mouse; Ctrl+V also pastes material nodes copied as text from UE4/UE5. Drag a texture from the Content Browser into the canvas to create a TextureSample."));
    ImGui::PopStyleColor();
    ImGui::Spacing();

    const ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    if (canvasSize.x > 2.0f && canvasSize.y > 2.0f) {
        m_GraphView->Draw(canvasSize);
        const std::string graphError = m_GraphView->TakeLastError();
        const std::string graphMessage = m_GraphView->TakeLastMessage();
        if (!graphError.empty()) {
            SetStatus(graphError, true);
        } else if (!graphMessage.empty()) {
            SetStatus(graphMessage);
        }
        const std::string functionRequest = m_GraphView->TakeFunctionOpenRequest();
        if (!functionRequest.empty()) {
            OpenMaterialFunction(functionRequest);
        }
        NodeId texturePickerNode = 0;
        PinId texturePickerPin = 0;
        if (m_GraphView->TakeTexturePickerRequest(&texturePickerNode, &texturePickerPin)) {
            OpenTexturePicker(texturePickerNode, texturePickerPin);
        }
        NodeId nodePreviewNode = 0;
        PinId nodePreviewPin = 0;
        if (m_GraphView->TakeNodePreviewRequest(&nodePreviewNode, &nodePreviewPin)) {
            StartNodePreview(nodePreviewNode, nodePreviewPin);
        }
    }

    ImGui::EndChild();
}

void Application::DrawRightSidebar(float width, float height) {
    const float gap = 8.0f;
    const float minPreview = std::min(280.0f, std::max(120.0f, height * 0.5f));
    const float detailsMin = std::min(140.0f, std::max(80.0f, height * 0.3f));
    const float maxPreview = std::max(minPreview, height - detailsMin - gap);
    float previewHeight = std::clamp(height * m_PreviewSplit, minPreview, maxPreview);
    const float detailsHeight = std::max(detailsMin, height - previewHeight - gap);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("##RightSidebar", ImVec2(width, height), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    DrawPreviewPanel(width, previewHeight);
    ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + previewHeight + gap));
    DrawDetailsPanel(width, detailsHeight);
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void Application::DrawPreviewContents(float width, float height, bool floating) {
    (void)width;
    (void)height;
    if (m_DocumentKind == DocumentKind::Function) {
        ImGui::Dummy(ImVec2(0.0f, 12.0f));
        ImGui::TextWrapped("%s", Tr("Function graphs are edited as reusable subgraphs and do not render a material preview. Add Function Input and Function Output nodes, connect their scalar pins, then save the .mffunction asset."));
        ImGui::Dummy(ImVec2(0.0f, 18.0f));
        ImGui::Separator();
        ImGui::TextDisabled("%s", Tr("Double-click the function asset in the Content Browser to reopen it."));
        return;
    }

    const bool shaderOk = m_ShaderGenerated && m_ShaderCompiled;
    const ImVec2 imageSize = ImGui::GetContentRegionAvail();
    if (imageSize.x < 2.0f || imageSize.y < 2.0f) {
        return;
    }

    if (!m_Graph.FindMaterialOutput()) {
        ImGui::InvisibleButton("##empty_preview", imageSize);
        const ImVec2 imageMin = ImGui::GetItemRectMin();
        const ImVec2 imageMax = ImGui::GetItemRectMax();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(imageMin, imageMax, IM_COL32(8, 12, 16, 255), 6.0f);
        const char* message = Tr("No material open");
        const char* hint = Tr("Create or open a material to see the live preview.");
        const ImVec2 messageSize = ImGui::CalcTextSize(message);
        const ImVec2 hintSize = ImGui::CalcTextSize(hint);
        drawList->AddText(ImVec2((imageMin.x + imageMax.x - messageSize.x) * 0.5f,
                                 (imageMin.y + imageMax.y) * 0.5f - Ui::S(14.0f)),
                          IM_COL32(176, 196, 200, 235), message);
        drawList->AddText(ImVec2((imageMin.x + imageMax.x - hintSize.x) * 0.5f,
                                 (imageMin.y + imageMax.y) * 0.5f + Ui::S(10.0f)),
                          IM_COL32(120, 145, 150, 210), hint);
        drawList->AddRect(imageMin, imageMax, IM_COL32(255, 255, 255, 22), Ui::S(6.0f), 0,
                          1.0f);
        return;
    }

    const float framebufferScale = std::max(1.0f, ImGui::GetIO().DisplayFramebufferScale.x);
    const int previewWidth = std::max(2, static_cast<int>(imageSize.x * framebufferScale));
    const int previewHeight = std::max(2, static_cast<int>(imageSize.y * framebufferScale));
    if (!m_PreviewRenderedThisFrame) {
        const bool sizeChanged = previewWidth != m_PreviewLastWidth ||
                                 previewHeight != m_PreviewLastHeight;
        if (m_PreviewLive || m_PreviewNeedsRender || sizeChanged) {
            const MaterialGraph& renderGraph = m_NodePreviewActive ? m_NodePreviewGraph : m_Graph;
            m_PreviewRenderer.Render(renderGraph, static_cast<float>(m_TimeSeconds), previewWidth,
                                     previewHeight);
            m_PreviewRenderedThisFrame = true;
            m_PreviewNeedsRender = false;
            m_PreviewLastWidth = previewWidth;
            m_PreviewLastHeight = previewHeight;
        }
    }

    ImGui::InvisibleButton("##preview_surface", imageSize);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    if (hovered) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            m_PreviewRenderer.Zoom(wheel);
            m_PreviewNeedsRender = true;
        }
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            m_PreviewRenderer.Camera().Reset();
            m_PreviewNeedsRender = true;
        }
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f)) {
        const ImVec2 delta = ImGui::GetIO().MouseDelta;
        m_PreviewRenderer.Orbit(delta.x, delta.y);
        m_PreviewNeedsRender = true;
    }

    const ImVec2 imageMin = ImGui::GetItemRectMin();
    const ImVec2 imageMax = ImGui::GetItemRectMax();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImTextureID textureId = static_cast<ImTextureID>(
        static_cast<intptr_t>(m_PreviewRenderer.ColorTexture()));
    if (textureId != 0) {
        drawList->AddImage(textureId, imageMin, imageMax, ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
    } else {
        drawList->AddRectFilled(imageMin, imageMax, IM_COL32(8, 12, 16, 255), 6.0f);
    }

    if (m_PixelProbeEnabled) {
        const ImVec2 mouse = hovered
                                 ? ImGui::GetIO().MousePos
                                 : ImVec2((imageMin.x + imageMax.x) * 0.5f,
                                          (imageMin.y + imageMax.y) * 0.5f);
        const float imageWidth = std::max(1.0f, imageMax.x - imageMin.x);
        const float imageHeight = std::max(1.0f, imageMax.y - imageMin.y);
        const float u = std::clamp((mouse.x - imageMin.x) / imageWidth, 0.0f, 1.0f);
        const float v = std::clamp((mouse.y - imageMin.y) / imageHeight, 0.0f, 1.0f);
        float rgba[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        if (m_PreviewRenderer.ProbePixel(u, v, rgba)) {
            drawList->AddCircle(mouse, Ui::S(5.0f), IM_COL32(255, 214, 120, 245), 16,
                                Ui::S(1.5f));
            drawList->AddLine(ImVec2(mouse.x - Ui::S(8.0f), mouse.y),
                              ImVec2(mouse.x + Ui::S(8.0f), mouse.y),
                              IM_COL32(255, 214, 120, 190), 1.0f);
            drawList->AddLine(ImVec2(mouse.x, mouse.y - Ui::S(8.0f)),
                              ImVec2(mouse.x, mouse.y + Ui::S(8.0f)),
                              IM_COL32(255, 214, 120, 190), 1.0f);

            char firstLine[128];
            char secondLine[128];
            if (IsScalarPreviewBuffer(m_PreviewBuffer)) {
                std::snprintf(firstLine, sizeof(firstLine), "%s",
                              Tr(PreviewBufferName(m_PreviewBuffer)));
                std::snprintf(secondLine, sizeof(secondLine), "%.4f", rgba[0]);
            } else {
                std::snprintf(firstLine, sizeof(firstLine), "%s",
                              Tr(PreviewBufferName(m_PreviewBuffer)));
                std::snprintf(secondLine, sizeof(secondLine), "R %.3f  G %.3f  B %.3f  A %.3f",
                              rgba[0], rgba[1], rgba[2], rgba[3]);
            }
            const float lineWidth = std::max(ImGui::CalcTextSize(firstLine).x,
                                             ImGui::CalcTextSize(secondLine).x);
            const ImVec2 boxSize(lineWidth + Ui::S(18.0f), Ui::S(45.0f));
            ImVec2 boxMin(mouse.x + Ui::S(14.0f), mouse.y + Ui::S(14.0f));
            if (boxMin.x + boxSize.x > imageMax.x - Ui::S(6.0f)) {
                boxMin.x = mouse.x - boxSize.x - Ui::S(14.0f);
            }
            if (boxMin.y + boxSize.y > imageMax.y - Ui::S(6.0f)) {
                boxMin.y = mouse.y - boxSize.y - Ui::S(14.0f);
            }
            const ImVec2 boxMax(boxMin.x + boxSize.x, boxMin.y + boxSize.y);
            drawList->AddRectFilled(boxMin, boxMax, IM_COL32(7, 14, 18, 235), Ui::S(7.0f));
            drawList->AddRect(boxMin, boxMax, IM_COL32(255, 214, 120, 150), Ui::S(7.0f));
            drawList->AddText(ImVec2(boxMin.x + Ui::S(9.0f), boxMin.y + Ui::S(5.0f)),
                              IM_COL32(255, 220, 145, 255), firstLine);
            drawList->AddText(ImVec2(boxMin.x + Ui::S(9.0f), boxMin.y + Ui::S(23.0f)),
                              IM_COL32(218, 233, 235, 255), secondLine);
        }
    }

    // Bottom vignette for legible overlay text.
    drawList->AddRectFilledMultiColor(ImVec2(imageMin.x, imageMax.y - 54.0f), imageMax,
                                      IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0),
                                      IM_COL32(0, 0, 0, 155), IM_COL32(0, 0, 0, 155));

    // Preview label chip.
    const char* meshLabel = Tr(m_PreviewRenderer.MeshName());
    const ImVec2 chipSize(ImGui::CalcTextSize(meshLabel).x + Ui::S(24.0f), Ui::S(30.0f));
    const ImVec2 chipPos(imageMin.x + Ui::S(10.0f), imageMin.y + Ui::S(10.0f));
    drawList->AddRectFilled(chipPos, ImVec2(chipPos.x + chipSize.x, chipPos.y + chipSize.y),
                            IM_COL32(6, 13, 16, 190), Ui::S(11.0f));
    drawList->AddCircleFilled(ImVec2(chipPos.x + Ui::S(10.0f), chipPos.y + Ui::S(11.0f)), 3.0f,
                              IM_COL32(88, 221, 196, 255), 10);
    drawList->AddText(ImVec2(chipPos.x + Ui::S(17.0f), chipPos.y + Ui::S(4.0f)),
                      IM_COL32(210, 236, 232, 240), meshLabel);

    const char* footHint = Tr("Drag: orbit  Scroll: zoom  Double-click: reset");
    drawList->AddText(ImVec2(imageMin.x + Ui::S(12.0f), imageMax.y - Ui::S(22.0f)),
                      IM_COL32(214, 230, 232, 165), footHint);
    std::string backendLabel = m_PreviewRenderer.BackendName();
    if (m_DocumentKind != DocumentKind::Function) {
        backendLabel += m_PreviewRenderer.Backend() == RenderApi::OpenGL
                            ? "  GLSL " + std::to_string(m_GlslVersion)
                            : std::string("  GPU");
    }
    drawList->AddText(ImVec2(imageMax.x - ImGui::CalcTextSize(backendLabel.c_str()).x - Ui::S(12.0f),
                             imageMax.y - Ui::S(22.0f)),
                      IM_COL32(205, 225, 226, 135), backendLabel.c_str());
    drawList->AddRect(imageMin, imageMax, IM_COL32(255, 255, 255, 22), Ui::S(6.0f), 0, 1.0f);

    const std::string& textureWarnings = m_PreviewRenderer.TextureDiagnostics();
    if (!textureWarnings.empty()) {
        const char* warning = Tr("TEXTURE FALLBACK");
        const float warningWidth = ImGui::CalcTextSize(warning).x + Ui::S(16.0f);
        drawList->AddRectFilled(ImVec2(imageMax.x - warningWidth - Ui::S(10.0f), imageMin.y + Ui::S(10.0f)),
                                ImVec2(imageMax.x - Ui::S(10.0f), imageMin.y + Ui::S(32.0f)),
                                IM_COL32(74, 53, 16, 205), 11.0f);
        drawList->AddText(ImVec2(imageMax.x - warningWidth - Ui::S(2.0f), imageMin.y + Ui::S(14.0f)),
                          IM_COL32(245, 197, 100, 255), warning);
    }

    if (!shaderOk) {
        const char* title = m_ShaderGenerated ? Tr("SHADER COMPILE ERROR")
                                               : Tr("MATERIAL GRAPH ERROR");
        const char* subtitle = Tr("Open the GLSL tab in the inspector for details.");
        const ImVec2 titleSize = ImGui::CalcTextSize(title);
        const ImVec2 subtitleSize = ImGui::CalcTextSize(subtitle);
        const float boxWidth = std::min(imageSize.x - Ui::S(28.0f), std::max(titleSize.x, subtitleSize.x) + Ui::S(42.0f));
        const float boxHeight = Ui::S(64.0f);
        const ImVec2 boxMin((imageMin.x + imageMax.x - boxWidth) * 0.5f,
                            (imageMin.y + imageMax.y - boxHeight) * 0.5f);
        const ImVec2 boxMax(boxMin.x + boxWidth, boxMin.y + boxHeight);
        drawList->AddRectFilled(boxMin, boxMax, IM_COL32(21, 12, 14, 225), 8.0f);
        drawList->AddRect(boxMin, boxMax, IM_COL32(198, 72, 66, 180), 8.0f);
        drawList->AddText(ImVec2((boxMin.x + boxMax.x - titleSize.x) * 0.5f, boxMin.y + Ui::S(14.0f)),
                          IM_COL32(250, 132, 121, 255), title);
        drawList->AddText(ImVec2((boxMin.x + boxMax.x - subtitleSize.x) * 0.5f, boxMin.y + Ui::S(38.0f)),
                          IM_COL32(205, 172, 170, 220), subtitle);
    }
    (void)floating;
}

void Application::DrawPreviewPanel(float width, float height) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Ui::S(12.0f), Ui::S(10.0f)));
    ImGui::BeginChild("##PreviewPanel", ImVec2(width, height), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    const bool functionDoc = m_DocumentKind == DocumentKind::Function;
    const bool shaderOk = m_ShaderGenerated && m_ShaderCompiled;
    const float toggleWidth = Ui::S(66.0f);
    const float detachWidth = Ui::S(54.0f);
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const bool showDetach = !functionDoc && !m_PreviewDetached;
    const float controlsWidth = toggleWidth * 2.0f + spacing +
                                (showDetach ? detachWidth + spacing : 0.0f);

    ImGui::TextDisabled("%s", Tr("LIVE PREVIEW"));
    ImGui::SameLine();
    const bool noPreviewMaterial =
        m_DocumentKind == DocumentKind::Material && !m_Graph.FindMaterialOutput();
    ImGui::GetWindowDrawList()->AddCircleFilled(
        ImVec2(ImGui::GetCursorScreenPos().x + 3.0f,
               ImGui::GetCursorScreenPos().y + ImGui::GetTextLineHeight() * 0.5f),
        3.0f,
        noPreviewMaterial ? IM_COL32(160, 176, 182, 255)
                          : (shaderOk ? IM_COL32(69, 222, 191, 255)
                                      : IM_COL32(247, 102, 92, 255)),
        10);
    ImGui::Dummy(ImVec2(8.0f, 0.0f));

    if (!functionDoc) {
        ImGui::SameLine(0.0f, 6.0f);
        if (ImGui::Checkbox(Tr("Live"), &m_PreviewLive)) {
            m_Settings.previewLive = m_PreviewLive;
            m_PreviewNeedsRender = true;
            SaveUiSettings();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", Tr("Render the preview every frame. Turn off to keep a static image and refresh it only after orbiting the material ball."));
        }
    }

    if (!functionDoc) {
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - controlsWidth);
        if (Ui::ToggleButton(Tr("Sphere"), m_PreviewRenderer.GetMesh() == PreviewMesh::Sphere,
                             ImVec2(toggleWidth, Ui::S(30.0f)))) {
            m_PreviewRenderer.SetMesh(PreviewMesh::Sphere);
            m_PreviewNeedsRender = true;
        }
        ImGui::SameLine();
        if (Ui::ToggleButton(Tr("Plane"), m_PreviewRenderer.GetMesh() == PreviewMesh::Plane,
                             ImVec2(toggleWidth, Ui::S(30.0f)))) {
            m_PreviewRenderer.SetMesh(PreviewMesh::Plane);
            m_PreviewNeedsRender = true;
        }
        if (showDetach) {
            ImGui::SameLine();
            if (Ui::GhostButton(Tr("Float"), ImVec2(detachWidth, Ui::S(30.0f)))) {
                DetachPreview();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s",
                                  Tr("Detach the live preview into a floating window"));
            }
        }
    }

    if (!functionDoc) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", Tr("Buffer"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(Ui::S(160.0f));
        const char* currentBuffer = Tr(PreviewBufferName(m_PreviewBuffer));
        if (ImGui::BeginCombo("##preview_buffer", currentBuffer)) {
            for (int i = 0; i <= static_cast<int>(PreviewBuffer::Depth); ++i) {
                const PreviewBuffer buffer = static_cast<PreviewBuffer>(i);
                const bool selected = m_PreviewBuffer == buffer;
                if (ImGui::Selectable(Tr(PreviewBufferName(buffer)), selected)) {
                    if (m_PreviewBuffer != buffer) {
                        m_PreviewBuffer = buffer;
                        m_LastContentRevision = 0;
                        m_ForceShaderRefresh = true;
                        m_PreviewNeedsRender = true;
                    }
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::Checkbox(Tr("Pixel Probe"), &m_PixelProbeEnabled);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", Tr("Hover the preview to inspect the selected buffer value."));
        }
        if (m_NodePreviewActive) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.35f, 0.88f, 0.76f, 1.0f), "%s",
                               Tr("Node result preview"));
            ImGui::SameLine();
            if (Ui::GhostButton(Tr("Stop Node Preview"), ImVec2(Ui::S(132.0f), Ui::S(26.0f)))) {
                StopNodePreview();
            }
        }
    }

    ImGui::Spacing();

    if (functionDoc) {
        DrawPreviewContents(width, height, false);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        return;
    }

    if (m_PreviewDetached) {
        const ImVec2 dropMin = ImGui::GetCursorScreenPos();
        const ImVec2 available = ImGui::GetContentRegionAvail();
        const ImVec2 dropMax(dropMin.x + available.x, dropMin.y + available.y);
        m_PreviewDockRectValid = true;
        m_PreviewDockMinX = dropMin.x;
        m_PreviewDockMinY = dropMin.y;
        m_PreviewDockMaxX = dropMax.x;
        m_PreviewDockMaxY = dropMax.y;
        ImGui::InvisibleButton("##preview_dock_target", available);
        const bool hovered = ImGui::IsItemHovered();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImU32 fill = hovered ? IM_COL32(24, 61, 60, 235) : IM_COL32(18, 24, 29, 180);
        drawList->AddRectFilled(dropMin, dropMax, fill, 8.0f);
        drawList->AddRect(dropMin, dropMax,
                          hovered ? IM_COL32(88, 221, 196, 220) : IM_COL32(255, 255, 255, 34),
                          8.0f, 0, hovered ? 2.0f : 1.0f);
        const char* title = Tr("LIVE PREVIEW DETACHED");
        const char* hint = Tr("Drag the floating preview here to dock it");
        const ImVec2 titleSize = ImGui::CalcTextSize(title);
        const ImVec2 hintSize = ImGui::CalcTextSize(hint);
        const float centerX = (dropMin.x + dropMax.x) * 0.5f;
        const float centerY = (dropMin.y + dropMax.y) * 0.5f;
        drawList->AddText(ImVec2(centerX - titleSize.x * 0.5f, centerY - Ui::S(36.0f)),
                          IM_COL32(211, 239, 234, 230), title);
        drawList->AddText(ImVec2(centerX - hintSize.x * 0.5f, centerY - Ui::S(8.0f)),
                          IM_COL32(137, 180, 179, 220), hint);
        const ImVec2 buttonSize(Ui::S(92.0f), Ui::S(30.0f));
        ImGui::SetCursorScreenPos(ImVec2(centerX - buttonSize.x * 0.5f, centerY + Ui::S(28.0f)));
        if (Ui::AccentButton(Tr("Dock"), buttonSize)) {
            DockPreview();
        }
        if (m_DetachedPreviewDragging && hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            DockPreview();
        }
        ImGui::SetCursorScreenPos(ImVec2(dropMin.x, dropMax.y + Ui::S(6.0f)));
        ImGui::TextDisabled("%s", m_PreviewRenderer.BackendName());
        ImGui::EndChild();
        ImGui::PopStyleVar();
        return;
    }

    DrawPreviewContents(width, height, false);
    m_PreviewDockRectValid = false;
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void Application::DetachPreview() {
    if (m_PreviewDetached) {
        return;
    }
    const float scale = std::max(0.75f, m_UiScale);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    m_DetachedPreviewWidth = std::min(viewport->WorkSize.x * 0.6f, 680.0f * scale);
    m_DetachedPreviewHeight = std::min(viewport->WorkSize.y * 0.65f, 500.0f * scale);
    m_DetachedPreviewX = viewport->WorkPos.x + viewport->WorkSize.x - m_DetachedPreviewWidth -
                         Ui::S(24.0f);
    m_DetachedPreviewY = viewport->WorkPos.y + Ui::S(100.0f);
    m_PreviewDetached = true;
    m_DetachedPreviewPlacementPending = true;
    m_DetachedPreviewDragging = false;
    SaveUiSettings();
    LogMessage(LogLevel::Info, "Live preview detached into a floating window.");
}

void Application::DockPreview() {
    if (!m_PreviewDetached) {
        return;
    }
    m_PreviewDetached = false;
    m_DetachedPreviewDragging = false;
    m_PreviewDockRectValid = false;
    SaveUiSettings();
    LogMessage(LogLevel::Info, "Live preview docked back into the inspector sidebar.");
}

void Application::SavePreviewWindowPlacement() {
    m_Settings.previewWindowX = m_DetachedPreviewX;
    m_Settings.previewWindowY = m_DetachedPreviewY;
    m_Settings.previewWindowWidth = m_DetachedPreviewWidth;
    m_Settings.previewWindowHeight = m_DetachedPreviewHeight;
    SaveUiSettings();
}

void Application::DrawDetachedPreviewWindow() {
    if (!m_PreviewDetached) {
        return;
    }
    if (m_DetachedPreviewPlacementPending) {
        ImGui::SetNextWindowPos(ImVec2(m_DetachedPreviewX, m_DetachedPreviewY),
                                ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(m_DetachedPreviewWidth, m_DetachedPreviewHeight),
                                 ImGuiCond_Always);
    }
    ImGui::SetNextWindowSizeConstraints(ImVec2(Ui::S(360.0f), Ui::S(300.0f)),
                                        ImVec2(FLT_MAX, FLT_MAX));

    bool open = true;
    bool dockByDrag = false;
    const std::string title = std::string(Tr("LIVE PREVIEW")) + "###DetachedPreview";
    if (ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoSavedSettings)) {
        const ImVec2 windowPos = ImGui::GetWindowPos();
        const ImVec2 windowSize = ImGui::GetWindowSize();
        if (m_DetachedPreviewPlacementPending) {
            m_DetachedPreviewPlacementPending = false;
            m_LastDetachedWindowX = windowPos.x;
            m_LastDetachedWindowY = windowPos.y;
        } else if (ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
                   (std::abs(windowPos.x - m_LastDetachedWindowX) > 0.5f ||
                    std::abs(windowPos.y - m_LastDetachedWindowY) > 0.5f)) {
            m_DetachedPreviewDragging = true;
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && m_DetachedPreviewDragging) {
            m_DetachedPreviewX = windowPos.x;
            m_DetachedPreviewY = windowPos.y;
            m_DetachedPreviewWidth = windowSize.x;
            m_DetachedPreviewHeight = windowSize.y;
            m_DetachedPreviewDragging = false;
            SavePreviewWindowPlacement();
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            if (m_PreviewDockRectValid && mouse.x >= m_PreviewDockMinX &&
                mouse.x <= m_PreviewDockMaxX && mouse.y >= m_PreviewDockMinY &&
                mouse.y <= m_PreviewDockMaxY) {
                dockByDrag = true;
            }
        }
        m_LastDetachedWindowX = windowPos.x;
        m_LastDetachedWindowY = windowPos.y;

        const float controlsWidth = Ui::S(66.0f) * 2.0f + Ui::S(64.0f) +
                                    ImGui::GetStyle().ItemSpacing.x * 2.0f;
        if (ImGui::GetContentRegionAvail().x > controlsWidth + Ui::S(24.0f)) {
            ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - controlsWidth);
        }
        const bool functionDoc = m_DocumentKind == DocumentKind::Function;
        if (!functionDoc) {
            if (Ui::ToggleButton(Tr("Sphere"), m_PreviewRenderer.GetMesh() == PreviewMesh::Sphere,
                                 ImVec2(Ui::S(66.0f), Ui::S(28.0f)))) {
                m_Settings.previewMeshMode = "Sphere";
                m_PreviewRenderer.SetMesh(PreviewMesh::Sphere);
                m_PreviewNeedsRender = true;
                SaveUiSettings();
            }
            ImGui::SameLine();
            if (Ui::ToggleButton(Tr("Plane"), m_PreviewRenderer.GetMesh() == PreviewMesh::Plane,
                                 ImVec2(Ui::S(66.0f), Ui::S(28.0f)))) {
                m_Settings.previewMeshMode = "Plane";
                m_PreviewRenderer.SetMesh(PreviewMesh::Plane);
                m_PreviewNeedsRender = true;
                SaveUiSettings();
            }
            ImGui::SameLine();
            if (Ui::AccentButton(Tr("Dock"), ImVec2(Ui::S(64.0f), Ui::S(28.0f)))) {
                DockPreview();
            }
        }
        ImGui::Separator();
        DrawPreviewContents(ImGui::GetContentRegionAvail().x,
                            ImGui::GetContentRegionAvail().y, true);
    }
    ImGui::End();
    if (!open || dockByDrag) {
        DockPreview();
    }
}

void Application::DrawDetailsPanel(float width, float height) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Ui::S(12.0f), Ui::S(10.0f)));
    ImGui::BeginChild("##DetailsPanel", ImVec2(width, height), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    Ui::SectionLabel(Tr("INSPECTOR"));
    ImGui::SameLine();
    if (m_DocumentKind == DocumentKind::Function) {
        ImGui::TextDisabled("  %s", Tr("function graph"));
    } else if (m_DocumentKind == DocumentKind::Instance) {
        ImGui::TextDisabled("  %s", Tr("material instance"));
    } else if (!m_Graph.FindMaterialOutput()) {
        ImGui::TextDisabled("  %s", Tr("no material"));
    } else if (!m_ShaderCompiled) {
        ImGui::TextColored(ImVec4(0.98f, 0.45f, 0.39f, 1.0f), "  %s", Tr("shader error"));
        ImGui::SameLine();
        if (Ui::GhostButton(Tr("View GLSL"), ImVec2(Ui::S(92.0f), Ui::S(28.0f)))) {
            m_FocusShaderTab = true;
        }
    } else {
        ImGui::TextDisabled("  %s", Tr("live"));
    }

    const ImGuiTabItemFlags shaderTabFlags =
        m_FocusShaderTab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
    const ImGuiTabItemFlags materialTabFlags =
        m_FocusMaterialSettingsTab ? ImGuiTabItemFlags_SetSelected
                                   : ImGuiTabItemFlags_None;
    const ImGuiTabItemFlags instanceTabFlags =
        m_FocusInstanceTab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
    if (ImGui::BeginTabBar("##InspectorTabs", ImGuiTabBarFlags_None)) {
        if (m_DocumentKind == DocumentKind::Instance &&
            ImGui::BeginTabItem(Tr("INSTANCE"), nullptr, instanceTabFlags)) {
            m_FocusInstanceTab = false;
            ImGui::BeginChild("##instance_scroll", ImVec2(0.0f, 0.0f),
                              ImGuiChildFlags_None,
                              ImGuiWindowFlags_AlwaysVerticalScrollbar);
            DrawInstancePanel();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (m_DocumentKind != DocumentKind::Instance &&
            ImGui::BeginTabItem(Tr("NODE DETAILS"))) {
            ImGui::BeginChild("##details_scroll", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
            DrawNodeDetails();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (m_DocumentKind == DocumentKind::Material &&
            ImGui::BeginTabItem(Tr("MATERIAL SETTINGS"), nullptr, materialTabFlags)) {
            m_FocusMaterialSettingsTab = false;
            ImGui::BeginChild("##material_settings_scroll", ImVec2(0.0f, 0.0f),
                              ImGuiChildFlags_None,
                              ImGuiWindowFlags_AlwaysVerticalScrollbar);
            DrawMaterialSettings();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (m_DocumentKind != DocumentKind::Function &&
            ImGui::BeginTabItem("GLSL", nullptr, shaderTabFlags)) {
            m_FocusShaderTab = false;
            ImGui::BeginChild("##shader_scroll", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
            DrawShaderPanel();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void Application::DrawPreviewSceneModal() {
    ImGui::SetNextWindowSize(ImVec2(Ui::S(620.0f), 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(Tr("Preview Scene"), &m_OpenPreviewSceneModal,
                                ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        m_OpenPreviewSceneModal = false;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    Ui::SectionLabel(Tr("MESH"));
    const char* meshModes[] = {Tr("Sphere"), Tr("Plane"), Tr("Custom OBJ")};
    int meshMode = m_Settings.previewMeshMode == "Plane"
                       ? 1
                       : (m_Settings.previewMeshMode == "Custom" ? 2 : 0);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo("##preview_mesh_mode", &meshMode, meshModes, 3)) {
        m_Settings.previewMeshMode = meshMode == 1 ? "Plane"
                                                    : (meshMode == 2 ? "Custom" : "Sphere");
    }
    if (meshMode == 2) {
        ImGui::InputTextWithHint("##preview_mesh_path", "assets/models/mesh.obj",
                                 &m_Settings.previewMeshPath);
        ImGui::SameLine();
        if (Ui::GhostButton(Tr("Browse..."), ImVec2(Ui::S(92.0f), 0.0f))) {
            m_FileAction = FileAction::SelectPreviewMesh;
            m_ReturnToPreviewSceneAfterBrowse = true;
            m_FileBrowser.Open(Tr("Select Preview Mesh"), FileBrowser::Mode::Open,
                               DefaultSaveDirectory(), m_Settings.previewMeshPath, {".obj"});
            m_OpenPreviewSceneModal = false;
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }
    }

    ImGui::Spacing();
    Ui::SectionLabel(Tr("ENVIRONMENT"));
    ImGui::InputTextWithHint("##preview_hdri_path", "assets/hdri/studio.hdr",
                             &m_Settings.previewHdriPath);
    ImGui::SameLine();
    if (Ui::GhostButton(Tr("Browse..."), ImVec2(Ui::S(92.0f), 0.0f))) {
        m_FileAction = FileAction::SelectPreviewHdri;
        m_ReturnToPreviewSceneAfterBrowse = true;
        m_FileBrowser.Open(Tr("Select HDRI Environment"), FileBrowser::Mode::Open,
                           DefaultSaveDirectory(), m_Settings.previewHdriPath, {".hdr"});
        m_OpenPreviewSceneModal = false;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    if (!m_Settings.previewHdriPath.empty() &&
        Ui::GhostButton(Tr("Clear HDRI"), ImVec2(Ui::S(112.0f), 0.0f))) {
        m_Settings.previewHdriPath.clear();
    }

    ImGui::Spacing();
    Ui::SectionLabel(Tr("GROUND"));
    ImGui::Checkbox(Tr("Enable Ground Plane"), &m_Settings.previewGroundEnabled);
    ImGui::SetNextItemWidth(-Ui::S(170.0f));
    ImGui::SliderFloat(Tr("Ground Height"), &m_Settings.previewGroundHeight,
                       -3.0f, 0.0f, "%.2f");

    ImGui::Spacing();
    Ui::SectionLabel(Tr("STUDIO LIGHTING"));
    ImGui::SetNextItemWidth(-Ui::S(170.0f));
    ImGui::SliderFloat(Tr("Key Light"), &m_Settings.previewKeyIntensity, 0.0f, 4.0f,
                       "%.2f");
    ImGui::SetNextItemWidth(-Ui::S(170.0f));
    ImGui::SliderFloat(Tr("Fill Light"), &m_Settings.previewFillIntensity, 0.0f, 4.0f,
                       "%.2f");
    ImGui::SetNextItemWidth(-Ui::S(170.0f));
    ImGui::SliderFloat(Tr("Rim Light"), &m_Settings.previewRimIntensity, 0.0f, 4.0f,
                       "%.2f");
    ImGui::SetNextItemWidth(-Ui::S(170.0f));
    ImGui::SliderFloat(Tr("Ambient"), &m_Settings.previewAmbientIntensity, 0.0f, 4.0f,
                       "%.2f");
    ImGui::SetNextItemWidth(-Ui::S(170.0f));
    ImGui::SliderFloat(Tr("Preview Exposure"), &m_Settings.previewExposure, 0.1f, 4.0f,
                       "%.2f");

    if (!m_PreviewSceneError.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.98f, 0.45f, 0.39f, 1.0f), "%s",
                           m_PreviewSceneError.c_str());
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s",
                       Tr("Custom mesh, HDRI and ground are rendered by the OpenGL "
                          "preview backend. Studio light intensities apply to all "
                          "backends after the shader is rebuilt."));
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::Separator();
    const float buttonWidth = Ui::S(108.0f);
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - buttonWidth * 2.0f -
                         ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::Button(Tr("Cancel"), ImVec2(buttonWidth, Ui::S(30.0f)))) {
        m_OpenPreviewSceneModal = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (Ui::AccentButton(Tr("Apply"), ImVec2(buttonWidth, Ui::S(30.0f)))) {
        ApplyPreviewSceneSettings();
        SaveUiSettings();
        m_OpenPreviewSceneModal = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

PreviewLightingSettings Application::PreviewLighting() const {
    PreviewLightingSettings lighting;
    lighting.keyIntensity = m_Settings.previewKeyIntensity;
    lighting.fillIntensity = m_Settings.previewFillIntensity;
    lighting.rimIntensity = m_Settings.previewRimIntensity;
    lighting.ambientIntensity = m_Settings.previewAmbientIntensity;
    lighting.exposure = m_Settings.previewExposure;
    return lighting;
}

PreviewEnvironmentSettings Application::PreviewEnvironment() const {
    PreviewEnvironmentSettings environment;
    environment.hdriPath = m_Settings.previewHdriPath;
    environment.groundEnabled = m_Settings.previewGroundEnabled;
    environment.groundHeight = m_Settings.previewGroundHeight;
    environment.exposure = m_Settings.previewExposure;
    environment.environmentRotation = 0.0f;
    return environment;
}

void Application::ApplyPreviewSceneSettings() {
    m_PreviewSceneError.clear();
    if (m_Settings.previewMeshMode == "Custom") {
        if (m_Settings.previewMeshPath.empty()) {
            m_PreviewSceneError = "Custom mesh path is empty; using the sphere.";
            m_PreviewRenderer.SetMesh(PreviewMesh::Sphere);
        } else {
            std::string meshError;
            std::string meshPath = m_Settings.previewMeshPath;
            if (!PathUtil::Exists(PathUtil::FromUtf8(meshPath))) {
                meshPath = PathUtil::ToUtf8(PathUtil::ResolveResource(meshPath));
            }
            if (!m_PreviewRenderer.LoadCustomMesh(meshPath, &meshError)) {
                m_PreviewSceneError = meshError;
                m_PreviewRenderer.SetMesh(PreviewMesh::Sphere);
            }
        }
    } else if (m_Settings.previewMeshMode == "Plane") {
        m_PreviewRenderer.SetMesh(PreviewMesh::Plane);
    } else {
        m_PreviewRenderer.SetMesh(PreviewMesh::Sphere);
    }

    std::string environmentError;
    PreviewEnvironmentSettings environment = PreviewEnvironment();
    if (!environment.hdriPath.empty() &&
        !PathUtil::Exists(PathUtil::FromUtf8(environment.hdriPath))) {
        environment.hdriPath =
            PathUtil::ToUtf8(PathUtil::ResolveResource(environment.hdriPath));
    }
    if (m_PreviewRenderer.Backend() == RenderApi::OpenGL ||
        !environment.hdriPath.empty() || environment.groundEnabled) {
        if (!m_PreviewRenderer.SetEnvironment(environment, &environmentError)) {
            if (!environmentError.empty()) {
                m_PreviewSceneError = environmentError;
                LogMessage(LogLevel::Warning, environmentError);
            }
        }
    }
    m_LastContentRevision = 0;
    m_ForceShaderRefresh = true;
    m_PreviewNeedsRender = true;
}

void Application::RequestUeBatchExport() {
    if (!m_Project.IsOpen()) {
        SetStatus(Tr("Open a project before exporting a UE batch."), true);
        return;
    }
    m_FileAction = FileAction::ExportUeBatch;
    m_FileBrowser.Open(Tr("Export UE Batch Manifest"), FileBrowser::Mode::Save,
                       m_Project.RootPath(), "ue_batch.json", {".json"});
}

bool Application::WriteUeBatchManifest(const std::filesystem::path& path,
                                       std::string* error) {
    if (!m_Project.IsOpen()) {
        if (error) *error = "No project is open.";
        return false;
    }
    nlohmann::json document;
    document["schema"] = "materialforge.ue5.batch";
    document["version"] = 1;
    document["destPath"] = m_UeDestPath;
    document["materials"] = nlohmann::json::array();
    std::vector<fs::path> materials;
    const auto addMaterial = [&](const std::string& relativePath) {
        if (relativePath.empty()) return;
        const fs::path absolute = m_Project.ResolveRelative(relativePath);
        if (!PathUtil::Exists(absolute)) return;
        if (std::find(materials.begin(), materials.end(), absolute) != materials.end()) return;
        materials.push_back(absolute);
    };
    addMaterial(m_Project.MainMaterialPath());
    for (const ProjectResource& resource : m_Project.Resources()) {
        if (resource.kind == "material") addMaterial(resource.path);
    }
    for (const fs::path& material : materials) {
        document["materials"].push_back(PathUtil::ToUtf8(material));
        const std::string relative = m_Project.RelativePathOf(material);
        const std::string stem = PathUtil::ToUtf8(material.stem());
        m_UeSync.Set(relative, m_UeDestPath + "/" + stem, "exported");
    }
    if (document["materials"].empty()) {
        if (error) *error = "The project has no material assets to export.";
        return false;
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        if (error) *error = "Could not write batch manifest: " + PathUtil::ToUtf8(path);
        return false;
    }
    stream << document.dump(2) << "\n";
    if (!stream) {
        if (error) *error = "Failed while writing batch manifest.";
        return false;
    }
    std::string syncError;
    if (!m_UeSync.Save(&syncError) && !syncError.empty()) {
        LogMessage(LogLevel::Warning, syncError);
    }
    if (error) error->clear();
    return true;
}

void Application::DrawUeBridgeModal() {
    ImGui::SetNextWindowSize(ImVec2(Ui::S(760.0f), 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(Tr("UE5 Push / Pull"), &m_OpenUeBridgeModal,
                                ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }
    Ui::SectionLabel(Tr("FILE BRIDGE"));
    ImGui::TextWrapped("%s",
                       Tr("Export the current material as UE5 JSON, run the push script "
                          "inside Unreal Editor, or pull a UE material back into "
                          "MaterialForge. The scripts live in tools/ue5."));

    ImGui::Spacing();
    ImGui::TextDisabled("%s", Tr("UE destination folder"));
    ImGui::SetNextItemWidth(-Ui::S(210.0f));
    ImGui::InputText("##ue_dest", &m_UeDestPath);
    ImGui::SameLine();
    if (Ui::GhostButton(Tr("Export JSON for Push"), ImVec2(Ui::S(190.0f), 0.0f))) {
        m_OpenUeBridgeModal = false;
        ImGui::CloseCurrentPopup();
        RequestExportDialog();
        ImGui::EndPopup();
        return;
    }
    ImGui::BeginDisabled(!m_Project.IsOpen());
    if (Ui::GhostButton(Tr("Export Project Batch..."), ImVec2(Ui::S(210.0f), 0.0f))) {
        m_OpenUeBridgeModal = false;
        ImGui::CloseCurrentPopup();
        RequestUeBatchExport();
        ImGui::EndPopup();
        return;
    }
    ImGui::EndDisabled();
    if (!m_UeLastMessage.empty()) {
        ImGui::TextDisabled("%s", m_UeLastMessage.c_str());
    }

    ImGui::Spacing();
    ImGui::TextDisabled("%s", Tr("UE material asset"));
    ImGui::SetNextItemWidth(-Ui::S(210.0f));
    ImGui::InputText("##ue_material", &m_UeMaterialPath);
    ImGui::SameLine();
    if (Ui::GhostButton(Tr("Open Pulled JSON..."), ImVec2(Ui::S(190.0f), 0.0f))) {
        m_OpenUeBridgeModal = false;
        ImGui::CloseCurrentPopup();
        m_FileAction = FileAction::OpenGraph;
        m_FileBrowser.Open(Tr("Open Pulled Material JSON"), FileBrowser::Mode::Open,
                           DefaultSaveDirectory(), {}, {".json"});
        ImGui::EndPopup();
        return;
    }

    ImGui::Spacing();
    Ui::SectionLabel(Tr("UE SYNC"));
    std::string currentRelative;
    if (m_Project.IsOpen() && m_DocumentKind == DocumentKind::Material &&
        !m_DocumentPath.empty() && m_Project.Contains(m_DocumentPath)) {
        currentRelative = m_Project.RelativePathOf(m_DocumentPath);
    }
    const UeSyncEntry* syncEntry =
        currentRelative.empty() ? nullptr : m_UeSync.Find(currentRelative);
    if (currentRelative.empty()) {
        ImGui::TextDisabled("%s", Tr("Open a project material to track UE sync state."));
    } else if (!syncEntry) {
        ImGui::TextDisabled("%s", Tr("Not linked to UE yet."));
    } else {
        ImGui::Text("%s  %s", Tr("State:"), syncEntry->state.c_str());
        ImGui::TextDisabled("%s", syncEntry->uePath.c_str());
        ImGui::TextDisabled("%s  %s", Tr("Updated:"), syncEntry->updatedAt.c_str());
    }
    ImGui::BeginDisabled(currentRelative.empty());
    if (Ui::GhostButton(Tr("Mark Pushed"), ImVec2(Ui::S(120.0f), 0.0f))) {
        const std::string stem = PathUtil::ToUtf8(PathUtil::FromUtf8(currentRelative).stem());
        m_UeSync.Set(currentRelative, m_UeDestPath + "/" + stem, "pushed");
        m_UeSync.Save(nullptr);
        m_UeLastMessage = Tr("Marked current material as pushed.");
    }
    ImGui::SameLine();
    if (Ui::GhostButton(Tr("Mark Pulled"), ImVec2(Ui::S(120.0f), 0.0f))) {
        const std::string stem = PathUtil::ToUtf8(PathUtil::FromUtf8(currentRelative).stem());
        m_UeSync.Set(currentRelative, m_UeMaterialPath, "pulled");
        m_UeSync.Save(nullptr);
        m_UeLastMessage = Tr("Marked current material as pulled.");
    }
    ImGui::EndDisabled();

    const std::string toolsDir =
        PathUtil::ToUtf8(m_ContentRoot / "tools" / "ue5");
    const std::string pushCommand =
        "py \"" + toolsDir + "/push_material.py\" \"<exported.json>\" --dest-path \"" +
        m_UeDestPath + "\"";
    const std::string pullCommand =
        "py \"" + toolsDir + "/pull_material_graph.py\" --material \"" +
        m_UeMaterialPath + "\" --out \"<pulled.json>\" --catalog \"" + toolsDir +
        "/node_catalog.json\"";
    ImGui::Spacing();
    Ui::SectionLabel(Tr("COMMANDS"));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", pushCommand.c_str());
    ImGui::TextWrapped("%s", pullCommand.c_str());
    ImGui::PopStyleColor();
    const std::string batchCommand =
        "py \"" + toolsDir + "/push_batch.py\" \"<project_batch.json>\"";
    const std::string batchPullCommand =
        "py \"" + toolsDir + "/pull_batch.py\" --folder \"" + m_UeDestPath +
        "\" --out-dir \"<pulled_folder>\"";
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", batchCommand.c_str());
    ImGui::TextWrapped("%s", batchPullCommand.c_str());
    ImGui::PopStyleColor();
    if (Ui::GhostButton(Tr("Copy Push Command"), ImVec2(Ui::S(170.0f), 0.0f))) {
        ImGui::SetClipboardText(pushCommand.c_str());
    }
    ImGui::SameLine();
    if (Ui::GhostButton(Tr("Copy Pull Command"), ImVec2(Ui::S(170.0f), 0.0f))) {
        ImGui::SetClipboardText(pullCommand.c_str());
    }
    if (Ui::GhostButton(Tr("Copy Batch Commands"), ImVec2(Ui::S(180.0f), 0.0f))) {
        ImGui::SetClipboardText((batchCommand + "\n" + batchPullCommand).c_str());
    }

    ImGui::Spacing();
    ImGui::TextDisabled("%s",
                        Tr("Pushing requires the Unreal Editor Python environment. "
                           "Pull exports supported expressions and warns about "
                           "unsupported UE classes."));
    ImGui::Spacing();
    ImGui::Separator();
    const float width = Ui::S(110.0f);
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - width);
    if (ImGui::Button(Tr("Close"), ImVec2(width, Ui::S(30.0f)))) {
        m_OpenUeBridgeModal = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void Application::DrawInstancePanel() {
    if (m_DocumentKind != DocumentKind::Instance) return;

    const fs::path parentPath = ResolveInstanceParent(m_Instance, m_DocumentPath);
    ImGui::TextDisabled("%s", Tr("Parent material"));
    ImGui::TextWrapped("%s", m_Instance.ParentMaterial().c_str());
    if (!parentPath.empty()) {
        if (Ui::GhostButton(Tr("Open Parent Material"),
                            ImVec2(Ui::S(170.0f), Ui::S(28.0f)))) {
            std::string error;
            if (!LoadDocument(PathUtil::ToUtf8(parentPath), &error)) {
                SetStatus(LocalizedErrorText(error), true);
            }
        }
    }
    ImGui::TextDisabled(Tr("%zu parameters  |  %zu overrides"),
                        m_InstanceParameters.size(),
                        m_Instance.Overrides().size());
    ImGui::Separator();

    if (m_InstanceParameters.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s",
                           Tr("The parent material does not expose any Scalar or Vector "
                              "parameters. Add parameter nodes there to create instance "
                              "controls."));
        return;
    }

    const auto writeNodeValue = [&](const MaterialParameterInfo& parameter,
                                    const PropertyValue& value) {
        Node* node = m_Graph.FindNode(parameter.nodeId);
        if (!node || node->type != parameter.nodeType) {
            node = nullptr;
            for (const auto& candidate : m_Graph.Nodes()) {
                if (candidate->type != parameter.nodeType) continue;
                const auto nameIt = candidate->properties.find("parameterName");
                if (nameIt != candidate->properties.end() &&
                    IsStringProperty(nameIt->second.kind) &&
                    nameIt->second.text == parameter.name) {
                    node = candidate.get();
                    break;
                }
            }
        }
        if (!node) return;
        node->properties["value"] = value;
        m_Graph.MarkContentChanged();
        SyncActiveDocument();
    };

    std::string currentGroup;
    for (const MaterialParameterInfo& parameter : m_InstanceParameters) {
        if (parameter.group != currentGroup) {
            currentGroup = parameter.group;
            ImGui::Spacing();
            Ui::SectionLabel(currentGroup.c_str());
        }
        ImGui::PushID(static_cast<int>(parameter.nodeId));
        const PropertyValue* overrideValue = m_Instance.FindOverride(parameter.name);
        const bool overridden = overrideValue != nullptr;
        ImGui::TextUnformatted(parameter.name.c_str());
        ImGui::SameLine();
        Ui::Badge(PinTypeToString(parameter.type), Ui::PinTypeColor(parameter.type, true));
        if (overridden) {
            ImGui::SameLine();
            Ui::Badge(Tr("OVERRIDE"), ImVec4(0.98f, 0.72f, 0.31f, 1.0f));
        }
        if (!parameter.description.empty()) {
            ImGui::SameLine();
            Ui::HelpMarker(parameter.description.c_str());
        }
        if (overridden) {
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - Ui::S(62.0f));
            if (ImGui::SmallButton(Tr("Reset"))) {
                m_Instance.ClearOverride(parameter.name);
                writeNodeValue(parameter, parameter.defaultValue);
                ImGui::PopID();
                continue;
            }
        }

        if (parameter.type == PinType::Float) {
            float value = overridden ? overrideValue->number : parameter.defaultValue.number;
            const float minValue = std::min(parameter.sliderMin, parameter.sliderMax);
            const float maxValue = std::max(parameter.sliderMin, parameter.sliderMax);
            ImGui::SetNextItemWidth(-1.0f);
            const bool changed =
                maxValue > minValue + 1e-4f
                    ? ImGui::SliderFloat("##instance_value", &value, minValue, maxValue,
                                         "%.4f", ImGuiSliderFlags_AlwaysClamp)
                    : ImGui::DragFloat("##instance_value", &value, 0.005f);
            if (changed && value != (overridden ? overrideValue->number
                                                : parameter.defaultValue.number)) {
                const PropertyValue property = PropertyValue::Float(value);
                m_Instance.SetOverride(parameter.name, parameter.type, property);
                writeNodeValue(parameter, property);
            }
        } else if (parameter.type == PinType::Vector4) {
            const PropertyValue baseValue =
                overridden ? *overrideValue : parameter.defaultValue;
            float values[4] = {baseValue.vec4.x, baseValue.vec4.y, baseValue.vec4.z,
                               baseValue.vec4.w};
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::ColorEdit4("##instance_value", values,
                                  ImGuiColorEditFlags_AlphaBar |
                                      ImGuiColorEditFlags_AlphaPreviewHalf)) {
                const PropertyValue property =
                    PropertyValue::Color4Value(values[0], values[1], values[2], values[3]);
                m_Instance.SetOverride(parameter.name, parameter.type, property);
                writeNodeValue(parameter, property);
            }
        } else {
            if (IsStringProperty(parameter.defaultValue.kind)) {
                ImGui::TextDisabled("%s", parameter.defaultValue.text.c_str());
            } else {
                ImGui::TextDisabled("%s", PinTypeToString(parameter.type));
            }
        }
        ImGui::PopID();
    }

    ImGui::Spacing();
    if (Ui::GhostButton(Tr("Reset All Overrides"), ImVec2(Ui::S(190.0f), Ui::S(28.0f)))) {
        m_Instance.ClearOverrides();
        for (const MaterialParameterInfo& parameter : m_InstanceParameters) {
            writeNodeValue(parameter, parameter.defaultValue);
        }
        SyncActiveDocument();
    }
}

void Application::DrawMaterialSettings() {
    MaterialSettings& settings = m_Graph.Settings();

    Ui::SectionLabel(Tr("MATERIAL"));
    ImGui::Spacing();
    std::string materialName = settings.name;
    ImGui::TextDisabled("%s", Tr("Name"));
    ImGui::SameLine(Ui::S(152.0f));
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##material_name", &materialName)) {
        settings.name = materialName;
        m_Graph.MarkLayoutChanged();
    }

    const auto combo = [&](const char* label, std::string& value,
                           const char* const* items, int count) {
        int index = 0;
        for (int i = 0; i < count; ++i) {
            if (value == items[i]) {
                index = i;
                break;
            }
        }
        bool changed = false;
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo(label, Tr(items[index]))) {
            for (int i = 0; i < count; ++i) {
                const bool selected = i == index;
                if (ImGui::Selectable(Tr(items[i]), selected)) {
                    value = items[i];
                    changed = true;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        return changed;
    };

    static const char* const domains[] = {"Surface", "Deferred Decal", "Light Function",
                                          "Volume", "Post Process"};
    static const char* const blendModes[] = {"Opaque", "Masked", "Translucent",
                                             "Additive", "Modulate", "AlphaComposite"};
    static const char* const shadingModels[] = {
        "DefaultLit", "Unlit", "Subsurface", "SubsurfaceProfile",
        "TwoSidedFoliage", "Hair", "Cloth", "Eye", "ClearCoat"};
    static const char* const translucencyModes[] = {
        "SurfaceForwardShading", "SurfaceTranslucencyVolume",
        "VolumetricNonDirectional", "VolumetricDirectional",
        "VolumetricPerVertexNonDirectional", "VolumetricPerVertexDirectional"};

    ImGui::Spacing();
    ImGui::TextDisabled("%s", Tr("Domain"));
    ImGui::SameLine(Ui::S(152.0f));
    if (combo("##material_domain", settings.domain, domains,
              static_cast<int>(std::size(domains)))) {
        m_Graph.MarkLayoutChanged();
    }
    ImGui::TextDisabled("%s", Tr("Blend Mode"));
    ImGui::SameLine(Ui::S(152.0f));
    if (combo("##material_blend_mode", settings.blendMode, blendModes,
              static_cast<int>(std::size(blendModes)))) {
        m_Graph.MarkContentChanged();
    }
    ImGui::TextDisabled("%s", Tr("Shading Model"));
    ImGui::SameLine(Ui::S(152.0f));
    if (combo("##material_shading_model", settings.shadingModel, shadingModels,
              static_cast<int>(std::size(shadingModels)))) {
        m_Graph.MarkContentChanged();
    }

    ImGui::Spacing();
    bool twoSided = settings.twoSided;
    if (ImGui::Checkbox(Tr("Two Sided"), &twoSided)) {
        settings.twoSided = twoSided;
        m_Graph.MarkContentChanged();
    }
    if (settings.blendMode == "Masked") {
        float clip = settings.opacityMaskClip;
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::SliderFloat(Tr("Opacity Mask Clip"), &clip, 0.0f, 1.0f, "%.3f")) {
            settings.opacityMaskClip = clip;
            m_Graph.MarkContentChanged();
        }
    }
    if (settings.blendMode != "Opaque" && settings.blendMode != "Masked") {
        ImGui::TextDisabled("%s", Tr("Translucency Lighting Mode"));
        ImGui::SameLine(Ui::S(152.0f));
        if (combo("##translucency_mode", settings.translucencyLightingMode,
                  translucencyModes, static_cast<int>(std::size(translucencyModes)))) {
            m_Graph.MarkLayoutChanged();
        }
    }

    ImGui::Spacing();
    if (ImGui::Checkbox(Tr("Dithered LOD Transition"),
                        &settings.ditheredLodTransition)) {
        m_Graph.MarkLayoutChanged();
    }
    if (ImGui::Checkbox(Tr("Cast Dynamic Shadow as Masked"),
                        &settings.castDynamicShadowAsMasked)) {
        m_Graph.MarkLayoutChanged();
    }
    if (ImGui::Checkbox(Tr("Use Emissive for Static Lighting"),
                        &settings.useEmissiveForStaticLighting)) {
        m_Graph.MarkLayoutChanged();
    }

    ImGui::Spacing();
    ImGui::TextDisabled("%s",
                        Tr("The preview uses the same settings for culling, blend mode "
                           "and shading. Output pins are exported to the UE5 bridge."));
}

void Application::DrawNodeDetails() {
    const NodeId selectedId = m_GraphView ? m_GraphView->SelectedNodeId() : 0;
    Node* node = m_Graph.FindNode(selectedId);

    if (!node) {
        DrawMaterialSettings();

        ImGui::Spacing();
        Ui::SectionLabel(Tr("STATISTICS"));
        const GraphStats stats = m_Graph.ComputeStats();
        if (ImGui::BeginTable("##graph_stats", 2, ImGuiTableFlags_SizingStretchProp |
                                                    ImGuiTableFlags_RowBg |
                                                    ImGuiTableFlags_NoBordersInBody)) {
            ImGui::TableSetupColumn(Tr("Key"), ImGuiTableColumnFlags_WidthFixed, 140.0f);
            ImGui::TableSetupColumn(Tr("Value"), ImGuiTableColumnFlags_WidthStretch);
            const auto row = [](const char* key, const std::string& value) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("%s", key);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(value.c_str());
            };
            row(Tr("Nodes"), std::to_string(stats.nodeCount));
            row(Tr("Links"), std::to_string(stats.linkCount));
            row(Tr("Parameters"), std::to_string(stats.parameterCount));
            row(Tr("Texture Objects"), std::to_string(stats.textureCount));
            row(Tr("Shading"), Tr("Studio PBR preview"));
            ImGui::EndTable();
        }

        ImGui::Spacing();
        Ui::SectionLabel(Tr("GETTING STARTED"));
        ImGui::TextWrapped("%s", Tr("Select a node to edit its properties. Right-click the graph canvas to search the UE5 expression library, then drag from an output pin to an input pin to build the material."));
        ImGui::Spacing();
        ImGui::TextWrapped("%s", Tr("The export JSON can be imported into UE5 with tools/ue5/import_material_graph.py."));
        return;
    }

    const NodeDefinition* definition = NodeRegistry::Get().Find(node->type);
    const ImVec4 categoryColor = Ui::CategoryColor(definition ? definition->category : "");

    ImGui::PushID(static_cast<int>(node->id));
    ImGui::Dummy(ImVec2(Ui::S(30.0f), Ui::S(30.0f)));
    Ui::DrawNodeThumbnail(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(),
                          Ui::S(30.0f), node->type,
                          definition ? definition->category : "");
    ImGui::SameLine();
    ImGui::TextColored(categoryColor, "%s", definition ? definition->displayName.c_str() : node->type.c_str());
    if (definition) {
        ImGui::SameLine();
        Ui::Badge(Tr(definition->category.c_str()), categoryColor);
        ImGui::TextDisabled(Tr("UE: %s"), definition->ueClass.c_str());
    }
    ImGui::TextDisabled(Tr("Node #%llu"), static_cast<unsigned long long>(node->id));
    if (definition && definition->isFunctionCall) {
        const auto readPropertyText = [&](const char* key, const char* fallback) {
            const auto it = node->properties.find(key);
            return it != node->properties.end() && IsStringProperty(it->second.kind) &&
                           !it->second.text.empty()
                       ? it->second.text
                       : std::string(fallback);
        };
        const std::string functionName = readPropertyText("functionName", "Material Function");
        const std::string functionPath = readPropertyText("functionPath", "");
        ImGui::Spacing();
        Ui::SectionLabel(Tr("MATERIAL FUNCTION"));
        ImGui::TextColored(ImVec4(0.56f, 0.72f, 1.0f, 1.0f), "%s", functionName.c_str());
        ImGui::Spacing();
        if (Ui::GhostButton(Tr("Open & Reveal Material Function"),
                            ImVec2(std::min(ImGui::GetContentRegionAvail().x, Ui::S(250.0f)),
                                   Ui::S(28.0f)))) {
            RevealMaterialFunction(functionPath);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", Tr("Open the function graph tab and select the asset in the Content Browser."));
        }
        if (!functionPath.empty()) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", functionPath.c_str());
            ImGui::PopTextWrapPos();
        } else {
            ImGui::TextDisabled("%s", Tr("No function asset is assigned."));
        }
        ImGui::Spacing();
    }

    if (definition) {
        ImGui::Indent(8.0f);
        ImGui::TextWrapped("%s", Tr(definition->description.c_str()));
        ImGui::Unindent(8.0f);
    }

    if (definition && definition->isComment) {
        ImGui::Spacing();
        Ui::SectionLabel(Tr("COMMENT BOX"));
        float size[2] = {node->size.x, node->size.y};
        if (ImGui::DragFloat2(Tr("Size"), size, 1.0f, 120.0f, 2000.0f, "%.0f")) {
            node->size.x = std::max(120.0f, size[0]);
            node->size.y = std::max(70.0f, size[1]);
            m_Graph.MarkLayoutChanged();
        }
    }

    ImGui::Spacing();
    Ui::SectionLabel(Tr("PROPERTIES"));
    if (definition && !definition->properties.empty()) {
        if (ImGui::BeginTable("##node_properties", 2, ImGuiTableFlags_SizingStretchProp |
                                                         ImGuiTableFlags_PadOuterX |
                                                         ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn(Tr("Property"), ImGuiTableColumnFlags_WidthFixed, 126.0f);
            ImGui::TableSetupColumn(Tr("Value"), ImGuiTableColumnFlags_WidthStretch);
            bool changed = false;

            for (const PropertySpec& spec : definition->properties) {
                auto valueIt = node->properties.find(spec.key);
                if (valueIt == node->properties.end()) {
                    node->properties.emplace(spec.key, spec.defaultValue);
                    valueIt = node->properties.find(spec.key);
                }
                PropertyValue& value = valueIt->second;

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(Tr(spec.label.c_str()));
                Ui::HelpMarker(Tr(spec.tooltip.c_str()));
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::PushID(spec.key.c_str());
                bool pinTypeHandled = false;
                const bool textureSettingsNode =
                    definition &&
                    (definition->type == "Sampler" ||
                     definition->type == "TextureObject" ||
                     definition->type == "TextureObjectParameter" ||
                     definition->type == "TextureSampleParameter2D");
                if (textureSettingsNode && value.kind == PropertyKind::String &&
                    (spec.key == "filter" || spec.key == "addressU" ||
                     spec.key == "addressV")) {
                    const char* const* items = nullptr;
                    int itemCount = 0;
                    if (spec.key == "filter") {
                        static const char* const filters[] = {
                            "Point", "Linear", "Trilinear", "Anisotropic"};
                        items = filters;
                        itemCount = 4;
                    } else {
                        static const char* const addresses[] = {
                            "Repeat", "Clamp", "Mirror"};
                        items = addresses;
                        itemCount = 3;
                    }
                    int index = 0;
                    for (int i = 0; i < itemCount; ++i) {
                        if (value.text == items[i]) {
                            index = i;
                            break;
                        }
                    }
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::BeginCombo("##texture_setting", items[index])) {
                        for (int i = 0; i < itemCount; ++i) {
                            if (ImGui::Selectable(items[i], i == index)) {
                                value.text = items[i];
                                changed = true;
                            }
                            if (i == index) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    pinTypeHandled = true;
                }
                if (definition &&
                    (definition->type == "FunctionInput" ||
                     definition->type == "FunctionOutput") &&
                    spec.key == "pinType" && value.kind == PropertyKind::String) {
                    static const char* const typeLabels[] = {
                        "Float", "Vector2", "Vector3", "Vector4"};
                    bool typeOk = false;
                    PinType currentType = PinTypeFromString(value.text, &typeOk);
                    if (!typeOk) currentType = PinType::Float;
                    int currentIndex = static_cast<int>(currentType);
                    if (currentIndex < 0 || currentIndex > 3) currentIndex = 0;
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::BeginCombo("##pin_type", typeLabels[currentIndex])) {
                        for (int i = 0; i < 4; ++i) {
                            if (ImGui::Selectable(typeLabels[i], i == currentIndex)) {
                                value.text = typeLabels[i];
                                const PinType newType = static_cast<PinType>(i);
                                auto applyType = [&](Pin& pin) {
                                    pin.type = newType;
                                    switch (newType) {
                                    case PinType::Vector2:
                                        pin.defaultValue = PropertyValue::Vec2Value(0.0f, 0.0f);
                                        break;
                                    case PinType::Vector3:
                                        pin.defaultValue =
                                            PropertyValue::Vec3Value(0.0f, 0.0f, 0.0f);
                                        break;
                                    case PinType::Vector4:
                                        pin.defaultValue = PropertyValue::Vec4Value(
                                            0.0f, 0.0f, 0.0f, 0.0f);
                                        break;
                                    default:
                                        pin.defaultValue = PropertyValue::Float(0.0f);
                                        break;
                                    }
                                };
                                for (Pin& pin : node->outputs) applyType(pin);
                                for (Pin& pin : node->inputs) applyType(pin);
                                changed = true;
                            }
                        }
                        ImGui::EndCombo();
                    }
                    pinTypeHandled = true;
                }
                if (!pinTypeHandled) {
                    switch (value.kind) {
                case PropertyKind::Float: {
                    const ImGuiSliderFlags sliderFlags = spec.hasRange
                        ? ImGuiSliderFlags_AlwaysClamp
                        : static_cast<ImGuiSliderFlags>(0);
                    float minV = spec.hasRange ? spec.minValue : 0.0f;
                    float maxV = spec.hasRange ? spec.maxValue : 0.0f;
                    changed |= ImGui::DragFloat("##value", &value.number, spec.dragSpeed,
                                                minV, maxV, "%.4f", sliderFlags);
                    break;
                }
                case PropertyKind::Vector2: {
                    const ImGuiSliderFlags sliderFlags = spec.hasRange
                        ? ImGuiSliderFlags_AlwaysClamp
                        : static_cast<ImGuiSliderFlags>(0);
                    changed |= ImGui::DragFloat2("##value", value.vec2.data(), spec.dragSpeed,
                                                 spec.hasRange ? spec.minValue : 0.0f,
                                                 spec.hasRange ? spec.maxValue : 0.0f,
                                                 "%.3f", sliderFlags);
                    break;
                }
                case PropertyKind::Vector3: {
                    const ImGuiSliderFlags sliderFlags = spec.hasRange
                        ? ImGuiSliderFlags_AlwaysClamp
                        : static_cast<ImGuiSliderFlags>(0);
                    changed |= ImGui::DragFloat3("##value", value.vec3.data(), spec.dragSpeed,
                                                 spec.hasRange ? spec.minValue : 0.0f,
                                                 spec.hasRange ? spec.maxValue : 0.0f,
                                                 "%.3f", sliderFlags);
                    break;
                }
                case PropertyKind::Vector4: {
                    const ImGuiSliderFlags sliderFlags = spec.hasRange
                        ? ImGuiSliderFlags_AlwaysClamp
                        : static_cast<ImGuiSliderFlags>(0);
                    changed |= ImGui::DragFloat4("##value", value.vec4.data(), spec.dragSpeed,
                                                 spec.hasRange ? spec.minValue : 0.0f,
                                                 spec.hasRange ? spec.maxValue : 0.0f,
                                                 "%.3f", sliderFlags);
                    break;
                }
                case PropertyKind::Color3:
                    changed |= ImGui::ColorEdit3("##value", value.vec3.data(),
                                                 ImGuiColorEditFlags_Float);
                    break;
                case PropertyKind::Color4:
                    changed |= ImGui::ColorEdit4("##value", value.vec4.data(),
                                                 ImGuiColorEditFlags_Float);
                    break;
                case PropertyKind::Bool:
                    changed |= ImGui::Checkbox("##value", &value.boolean);
                    break;
                case PropertyKind::String:
                    changed |= ImGui::InputText("##value", &value.text);
                    break;
                case PropertyKind::AssetPath: {
                    const float browseWidth = 62.0f;
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - browseWidth -
                                            ImGui::GetStyle().ItemSpacing.x);
                    changed |= ImGui::InputText("##value", &value.text);
                    ImGui::SameLine();
                    if (ImGui::Button(Tr("Browse"), ImVec2(browseWidth, 0.0f))) {
                        RequestTextureDialog(node->id, spec.key);
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", Tr("Choose an image file. Paths inside the project are stored relative to the project root."));
                    }
                    break;
                }
                }
                }
                ImGui::PopID();
            }
            ImGui::EndTable();

            if (changed) {
                m_Graph.MarkContentChanged();
            }
        }
    } else {
        ImGui::TextDisabled("%s", Tr("This node has no editable properties."));
    }

    ImGui::Spacing();
    TypeResolver resolver(m_Graph);
    bool hasEditableDefaults = false;
    for (const Pin& pin : node->inputs) {
        if (pin.direction != PinDirection::Input || m_Graph.IsPinConnected(pin.id)) continue;
        const PinType effective = resolver.GetEffectiveType(pin);
        if (!IsNumericPin(effective) && pin.defaultValue.kind != PropertyKind::Float &&
            pin.defaultValue.kind != PropertyKind::Vector2 &&
            pin.defaultValue.kind != PropertyKind::Vector3 &&
            pin.defaultValue.kind != PropertyKind::Vector4 &&
            pin.defaultValue.kind != PropertyKind::Color3 &&
            pin.defaultValue.kind != PropertyKind::Color4 &&
            pin.defaultValue.kind != PropertyKind::Bool) {
            continue;
        }
        hasEditableDefaults = true;
        break;
    }
    if (hasEditableDefaults) {
        Ui::SectionLabel(Tr("INPUT DEFAULTS"));
        ImGui::TextDisabled("%s", Tr("Used when an input is not connected."));
        bool changed = false;
        if (ImGui::BeginTable("##node_input_defaults", 2,
                              ImGuiTableFlags_SizingStretchProp |
                                  ImGuiTableFlags_PadOuterX | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn(Tr("Input"), ImGuiTableColumnFlags_WidthFixed, 126.0f);
            ImGui::TableSetupColumn(Tr("Constant"), ImGuiTableColumnFlags_WidthStretch);
            for (Pin& pin : node->inputs) {
                if (pin.direction != PinDirection::Input || m_Graph.IsPinConnected(pin.id)) {
                    continue;
                }
                const PinType effective = resolver.GetEffectiveType(pin);
                const bool numericDefault =
                    pin.defaultValue.kind == PropertyKind::Float ||
                    pin.defaultValue.kind == PropertyKind::Vector2 ||
                    pin.defaultValue.kind == PropertyKind::Vector3 ||
                    pin.defaultValue.kind == PropertyKind::Vector4 ||
                    pin.defaultValue.kind == PropertyKind::Color3 ||
                    pin.defaultValue.kind == PropertyKind::Color4 ||
                    pin.defaultValue.kind == PropertyKind::Bool;
                if (!IsNumericPin(effective) && !numericDefault) {
                    continue;
                }
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(pin.name.c_str());
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s  ·  %s", Tr("Input default"),
                                      Ui::PinTypeShortName(effective));
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::PushID(static_cast<int>(pin.id));
                switch (pin.defaultValue.kind) {
                case PropertyKind::Vector2:
                    changed |= ImGui::DragFloat2("##pin_default", pin.defaultValue.vec2.data(),
                                                 0.005f);
                    break;
                case PropertyKind::Vector3:
                case PropertyKind::Color3:
                    changed |= ImGui::DragFloat3("##pin_default", pin.defaultValue.vec3.data(),
                                                 0.005f);
                    break;
                case PropertyKind::Vector4:
                case PropertyKind::Color4:
                    changed |= ImGui::DragFloat4("##pin_default", pin.defaultValue.vec4.data(),
                                                 0.005f);
                    break;
                case PropertyKind::Bool:
                    changed |= ImGui::Checkbox("##pin_default", &pin.defaultValue.boolean);
                    break;
                case PropertyKind::Float:
                case PropertyKind::String:
                case PropertyKind::AssetPath:
                default:
                    changed |= ImGui::DragFloat("##pin_default", &pin.defaultValue.number,
                                                0.005f);
                    break;
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (changed) {
            m_Graph.MarkContentChanged();
        }
        ImGui::Spacing();
    }

    Ui::SectionLabel(Tr("PINS"));
    if (ImGui::BeginTable("##node_pins", 5, ImGuiTableFlags_RowBg |
                                               ImGuiTableFlags_SizingStretchProp |
                                               ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupColumn(Tr("IO"), ImGuiTableColumnFlags_WidthFixed, 34.0f);
        ImGui::TableSetupColumn(Tr("Pin"), ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableSetupColumn(Tr("Type"), ImGuiTableColumnFlags_WidthFixed, 74.0f);
        ImGui::TableSetupColumn(Tr("Preview"), ImGuiTableColumnFlags_WidthFixed, 54.0f);
        ImGui::TableSetupColumn(Tr("Link"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();

        auto drawPinRow = [&](const Pin& pin) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%s", pin.direction == PinDirection::Input ? "IN" : "OUT");
            ImGui::TableSetColumnIndex(1);
            const ImU32 dotColor = Ui::PinTypeU32(resolver.GetEffectiveType(pin), true);
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            const ImVec2 cursor = ImGui::GetCursorScreenPos();
            drawList->AddCircleFilled(ImVec2(cursor.x + 3.0f, cursor.y + ImGui::GetTextLineHeight() * 0.5f),
                                      3.0f, dotColor, 10);
            ImGui::Dummy(ImVec2(10.0f, 0.0f));
            ImGui::SameLine();
            ImGui::TextUnformatted(pin.name.c_str());
            const auto acceptTextureDrop = [&](const Pin& targetPin) {
                if (!ImGui::BeginDragDropTarget()) return;
                if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload("MF_TEXTURE_ASSET")) {
                    if (payload->Data && payload->DataSize > 1 && m_GraphView) {
                        const std::string path(static_cast<const char*>(payload->Data));
                        std::string error;
                        if (!m_GraphView->ApplyTextureToInput(node->id, targetPin.id, path,
                                                              &error)) {
                            SetStatus(LocalizedErrorText(error), true);
                        }
                    }
                }
                ImGui::EndDragDropTarget();
            };
            if (pin.direction == PinDirection::Input && pin.type == PinType::Texture) {
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                    OpenTexturePicker(node->id, pin.id);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", Tr("Click to choose a texture, or drag a texture asset here."));
                }
                acceptTextureDrop(pin);
            }
            ImGui::TableSetColumnIndex(2);
            ImGui::TextColored(Ui::PinTypeColor(resolver.GetEffectiveType(pin), true), "%s",
                               Ui::PinTypeShortName(resolver.GetEffectiveType(pin)));
            ImGui::TableSetColumnIndex(3);
            ImGui::Dummy(ImVec2(44.0f, 18.0f));
            Ui::DrawPinThumbnail(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(),
                                 44.0f, 18.0f, resolver.GetEffectiveType(pin),
                                 pin.defaultValue);
            ImGui::TableSetColumnIndex(4);
            const Link* link = m_Graph.FindInputLink(pin.id);
            if (pin.direction == PinDirection::Input && pin.type == PinType::Texture) {
                const Node* source = link ? m_Graph.FindSourceNode(pin.id) : nullptr;
                std::string texturePath;
                if (source) {
                    const auto textureIt = source->properties.find("texture");
                    if (textureIt != source->properties.end() &&
                        IsStringProperty(textureIt->second.kind)) {
                        texturePath = textureIt->second.text;
                    }
                }
                if (texturePath.empty()) {
                    const auto ownTexture = node->properties.find("texture");
                    if (ownTexture != node->properties.end() &&
                        IsStringProperty(ownTexture->second.kind)) {
                        texturePath = ownTexture->second.text;
                    }
                }
                const std::string textureName =
                    texturePath.empty()
                        ? (source ? std::string("TextureObject") : Tr("Select Texture..."))
                        : fs::path(texturePath).filename().u8string();
                if (ImGui::Button(textureName.c_str(), ImVec2(-1.0f, 0.0f))) {
                    OpenTexturePicker(node->id, pin.id);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", Tr("Click to choose a texture, or drag a texture asset here."));
                }
                acceptTextureDrop(pin);
            } else if (link && pin.direction == PinDirection::Input) {
                const Node* source = m_Graph.FindSourceNode(pin.id);
                ImGui::TextDisabled("%s", source ? NodeDisplayName(*source) : Tr("Connected"));
            } else if (pin.direction == PinDirection::Output && m_Graph.IsPinConnected(pin.id)) {
                ImGui::TextDisabled("%s", Tr("Connected"));
            } else if (pin.direction == PinDirection::Input) {
                ImGui::TextDisabled("%s", Tr("default"));
            } else {
                ImGui::TextDisabled("-");
            }
        };

        for (const Pin& pin : node->inputs) drawPinRow(pin);
        for (const Pin& pin : node->outputs) drawPinRow(pin);
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::TextDisabled("%s", Tr("Tip: remove a connection by selecting the link and pressing Del."));
    ImGui::PopID();
}

void Application::DrawShaderPanel() {
    const bool ok = m_ShaderGenerated && m_ShaderCompiled;
    if (m_DocumentKind == DocumentKind::Material && !m_Graph.FindMaterialOutput()) {
        ImGui::TextDisabled("%s", Tr("No material open"));
    } else if (ok) {
        ImGui::TextColored(ImVec4(0.35f, 0.88f, 0.76f, 1.0f), "%s", Tr("GLSL compiled successfully"));
    } else if (!m_ShaderGenerated) {
        ImGui::TextColored(ImVec4(0.98f, 0.45f, 0.39f, 1.0f), "%s", Tr("Material graph error"));
    } else {
        ImGui::TextColored(ImVec4(0.98f, 0.45f, 0.39f, 1.0f), "%s", Tr("GLSL compile error"));
    }
    ImGui::TextDisabled("%s", m_GeneratedShader.diagnostic.c_str());

    if (!m_ShaderError.empty()) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.09f, 0.04f, 0.045f, 1.0f));
        ImGui::BeginChild("##shader_error_log", ImVec2(0.0f, 128.0f), ImGuiChildFlags_Borders);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.55f, 0.48f, 1.0f));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(m_ShaderError.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    for (const std::string& warning : m_GeneratedShader.warnings) {
        ImGui::TextColored(ImVec4(0.96f, 0.76f, 0.34f, 1.0f), Tr("Warning: %s"),
                           warning.c_str());
    }
    const std::string& textureDiagnostics = m_PreviewRenderer.TextureDiagnostics();
    if (!textureDiagnostics.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f, 0.70f, 0.35f, 1.0f));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(textureDiagnostics.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    if (Ui::GhostButton(Tr("Copy GLSL"), ImVec2(90.0f, 24.0f))) {
        ImGui::SetClipboardText(m_GeneratedShader.fragmentSource.c_str());
        SetStatus(Tr("GLSL source copied to clipboard."));
    }
    ImGui::SameLine();
    if (Ui::GhostButton(Tr("Copy Log"), ImVec2(82.0f, 24.0f))) {
        std::string log = m_GeneratedShader.diagnostic;
        if (!m_ShaderError.empty()) {
            log += "\n\n";
            log += m_ShaderError;
        }
        ImGui::SetClipboardText(log.c_str());
        SetStatus(Tr("Shader diagnostics copied to clipboard."));
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (m_GeneratedShader.fragmentSource.empty()) {
        ImGui::TextDisabled("%s", Tr("No shader source was generated."));
        return;
    }

    const ImVec2 codeSize = ImGui::GetContentRegionAvail();
    PushMonoFont();
    std::string& source = m_GeneratedShader.fragmentSource;
    ImGui::InputTextMultiline("##fragment_source", source.data(), source.size() + 1,
                              ImVec2(codeSize.x, std::max(120.0f, codeSize.y)),
                              ImGuiInputTextFlags_ReadOnly);
    PopMonoFont();
}

void Application::DrawStatusBar() {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float width = ImGui::GetContentRegionAvail().x;
    drawList->AddLine(ImVec2(pos.x, pos.y), ImVec2(pos.x + width, pos.y),
                      IM_COL32(255, 255, 255, 18), 1.0f);
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + 5.0f));

    int errorCount = 0;
    int warningCount = 0;
    for (const ConsoleEntry& entry : m_ConsoleEntries) {
        if (entry.level == LogLevel::Error) ++errorCount;
        if (entry.level == LogLevel::Warning) ++warningCount;
    }

    char consoleLabel[128];
    if (errorCount > 0) {
        std::snprintf(consoleLabel, sizeof(consoleLabel), ">_ %s  !%d", Tr("CMD Console"),
                      errorCount);
    } else if (warningCount > 0) {
        std::snprintf(consoleLabel, sizeof(consoleLabel), ">_ %s  %d!", Tr("CMD Console"),
                      warningCount);
    } else {
        std::snprintf(consoleLabel, sizeof(consoleLabel), ">_ %s", Tr("CMD Console"));
    }
    const float consoleWidth = ImGui::CalcTextSize(consoleLabel).x + Ui::S(18.0f);

    const GraphStats stats = m_Graph.ComputeStats();
    char metrics[320];
    const char* backendName = m_PreviewRenderer.BackendName();
    if (m_PreviewRenderer.Backend() == RenderApi::OpenGL) {
        if (IsChineseUi()) {
            std::snprintf(metrics, sizeof(metrics),
                          "%zu 个节点  |  %zu 条连线  |  %.0f FPS  |  OpenGL %d.%d",
                          stats.nodeCount, stats.linkCount, ImGui::GetIO().Framerate,
                          m_GlMajor, m_GlMinor);
        } else {
            std::snprintf(metrics, sizeof(metrics),
                          "%zu nodes  |  %zu links  |  %.0f FPS  |  OpenGL %d.%d",
                          stats.nodeCount, stats.linkCount, ImGui::GetIO().Framerate,
                          m_GlMajor, m_GlMinor);
        }
    } else {
        if (IsChineseUi()) {
            std::snprintf(metrics, sizeof(metrics),
                          "%zu 个节点  |  %zu 条连线  |  %.0f FPS  |  %s 预览",
                          stats.nodeCount, stats.linkCount, ImGui::GetIO().Framerate,
                          backendName);
        } else {
            std::snprintf(metrics, sizeof(metrics),
                          "%zu nodes  |  %zu links  |  %.0f FPS  |  %s preview",
                          stats.nodeCount, stats.linkCount, ImGui::GetIO().Framerate,
                          backendName);
        }
    }
    const float metricsWidth = ImGui::CalcTextSize(metrics).x;

    const bool statusExpired = glfwGetTime() > m_StatusExpiresAt;
    const char* message = statusExpired ? Tr("Ready") : m_StatusMessage.c_str();
    const ImVec4 statusColor = m_StatusError && !statusExpired
                                   ? ImVec4(0.98f, 0.52f, 0.45f, 1.0f)
                                   : ImVec4(0.54f, 0.67f, 0.70f, 1.0f);
    const float maxMessageWidth =
        std::max(Ui::S(60.0f), width - consoleWidth - metricsWidth - Ui::S(42.0f));
    const std::string displayMessage = EllipsizeToWidth(message, maxMessageWidth);
    ImGui::TextColored(statusColor, "%s", displayMessage.c_str());
    if (ImGui::IsItemHovered() && displayMessage != message) {
        ImGui::SetTooltip("%s", message);
    }

    ImGui::SameLine(0.0f, 10.0f);
    const ImVec4 consoleText = errorCount > 0
        ? ImVec4(0.98f, 0.55f, 0.48f, 1.0f)
        : warningCount > 0 ? ImVec4(0.96f, 0.76f, 0.34f, 1.0f)
                           : ImVec4(0.60f, 0.76f, 0.78f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, consoleText);
    if (Ui::GhostButton(consoleLabel, ImVec2(consoleWidth, Ui::S(20.0f)))) {
        SetConsoleVisible(!m_ConsoleVisible);
    }
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s\n%s", Tr("Show or hide the CMD console (Ctrl+`)"),
                          Tr("Errors, warnings and command output are shown here."));
    }

    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - metricsWidth);
    ImGui::TextDisabled("%s", metrics);
}

void Application::DrawConsoleEntries() {
    ImFont* mono = m_MonoFont ? m_MonoFont : ImGui::GetFont();
    ImGui::PushFont(mono);
    const ImVec4 infoColor(0.64f, 0.76f, 0.78f, 1.0f);
    const ImVec4 warningColor(0.98f, 0.78f, 0.34f, 1.0f);
    const ImVec4 errorColor(0.98f, 0.55f, 0.48f, 1.0f);
    for (const ConsoleEntry& entry : m_ConsoleEntries) {
        if (m_ConsoleFilter == 1 && entry.level != LogLevel::Info) continue;
        if (m_ConsoleFilter == 2 && entry.level != LogLevel::Warning) continue;
        if (m_ConsoleFilter == 3 && entry.level != LogLevel::Error) continue;
        const char* tag = entry.level == LogLevel::Error
                              ? "ERR"
                              : entry.level == LogLevel::Warning ? "WRN" : "INF";
        const ImVec4 color = entry.level == LogLevel::Error
                                 ? errorColor
                                 : entry.level == LogLevel::Warning ? warningColor : infoColor;
        ImGui::PushStyleColor(ImGuiCol_Text, entry.level == LogLevel::Info
                                                 ? ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled)
                                                 : color);
        ImGui::Text("[%7.2f] %s  ", entry.time, tag);
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(entry.message.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    ImGui::PopFont();
    if (m_ConsoleScrollToBottom) {
        ImGui::SetScrollHereY(1.0f);
        m_ConsoleScrollToBottom = false;
    }
}

void Application::DrawConsolePanel(float height) {
    const float splitterHeight = Ui::S(5.0f);
    ImGui::InvisibleButton("##ConsoleSplitter", ImVec2(ImGui::GetContentRegionAvail().x, splitterHeight));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }
    if (ImGui::IsItemActive()) {
        m_ConsoleHeight = std::clamp(
            m_ConsoleHeight - ImGui::GetIO().MouseDelta.y / std::max(0.1f, m_UiScale),
            110.0f, 520.0f);
    }
    if (ImGui::IsItemDeactivated()) {
        SaveUiSettings();
    }
    const ImVec2 splitterMin = ImGui::GetItemRectMin();
    const ImVec2 splitterMax = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(splitterMin.x + Ui::S(10.0f), splitterMin.y + 2.0f),
        ImVec2(splitterMax.x - Ui::S(10.0f), splitterMin.y + 2.0f),
        ImGui::IsItemHovered() ? IM_COL32(89, 211, 191, 170) : IM_COL32(255, 255, 255, 32),
        1.0f);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Ui::S(9.0f), Ui::S(6.0f)));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.035f, 0.046f, 0.054f, 1.0f));
    ImGui::BeginChild("##CMDConsole",
                      ImVec2(0.0f, std::max(Ui::S(80.0f), height - splitterHeight)),
                      ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    Ui::SectionLabel(Tr("CMD CONSOLE"));
    ImGui::SameLine();
    int infoCount = 0;
    int warningCount = 0;
    int errorCount = 0;
    for (const ConsoleEntry& entry : m_ConsoleEntries) {
        if (entry.level == LogLevel::Info) ++infoCount;
        else if (entry.level == LogLevel::Warning) ++warningCount;
        else ++errorCount;
    }
    ImGui::TextDisabled("  %d / %d / %d", errorCount, warningCount, infoCount);
    const char* copyLabel = Tr("Copy");
    const char* clearLabel = Tr("Clear");
    const float closeWidth = Ui::S(24.0f);
    const float clearWidth = ImGui::CalcTextSize(clearLabel).x + Ui::S(14.0f);
    const float copyWidth = ImGui::CalcTextSize(copyLabel).x + Ui::S(14.0f);
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float actionsWidth = closeWidth + clearWidth + copyWidth + spacing * 2.0f;
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - actionsWidth);
    if (Ui::GhostButton(copyLabel, ImVec2(copyWidth, Ui::S(20.0f)))) {
        std::string text;
        for (const ConsoleEntry& entry : m_ConsoleEntries) {
            char line[16];
            std::snprintf(line, sizeof(line), "[%7.2f] ", entry.time);
            text += line;
            text += entry.level == LogLevel::Error ? "ERR " :
                    entry.level == LogLevel::Warning ? "WRN " : "INF ";
            text += entry.message;
            text += "\n";
        }
        ImGui::SetClipboardText(text.c_str());
    }
    ImGui::SameLine();
    if (Ui::GhostButton(clearLabel, ImVec2(clearWidth, Ui::S(20.0f)))) {
        ClearConsole();
    }
    ImGui::SameLine();
    if (Ui::GhostButton("x", ImVec2(closeWidth, Ui::S(20.0f)))) {
        SetConsoleVisible(false);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", Tr("Hide the console"));
    }
    ImGui::NewLine();

    const char* filters[] = {"All", "Info", "Warnings", "Errors"};
    for (int i = 0; i < 4; ++i) {
        if (i > 0) ImGui::SameLine();
        const bool active = m_ConsoleFilter == i;
        if (Ui::ToggleButton(Tr(filters[i]), active, ImVec2(Ui::S(74.0f), Ui::S(20.0f)))) {
            m_ConsoleFilter = i;
        }
    }

    ImGui::Separator();
    const float inputHeight = Ui::S(24.0f);
    ImGui::BeginChild("##console_log", ImVec2(0.0f, -inputHeight - Ui::S(4.0f)),
                      ImGuiChildFlags_None);
    DrawConsoleEntries();
    ImGui::EndChild();

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.92f, 0.78f, 1.0f));
    ImGui::TextUnformatted(">");
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, 6.0f);
    ImGui::SetNextItemWidth(-1.0f);
    if (m_ConsoleFocusInput) {
        ImGui::SetKeyboardFocusHere();
        m_ConsoleFocusInput = false;
    }
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.06f, 0.08f, 0.09f, 1.0f));
    const bool submitted = ImGui::InputTextWithHint(
        "##console_input", Tr("Type a command, e.g. help, renderer vulkan, stats"),
        m_ConsoleInput, sizeof(m_ConsoleInput),
        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopStyleColor();
    if (ImGui::IsItemFocused()) {
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
            if (!m_ConsoleHistory.empty()) {
                m_ConsoleHistoryCursor = std::min(
                    static_cast<int>(m_ConsoleHistory.size()) - 1,
                    m_ConsoleHistoryCursor < 0 ? static_cast<int>(m_ConsoleHistory.size()) - 1
                                               : m_ConsoleHistoryCursor - 1);
                std::snprintf(m_ConsoleInput, sizeof(m_ConsoleInput), "%s",
                              m_ConsoleHistory[static_cast<std::size_t>(
                                  std::max(0, m_ConsoleHistoryCursor))].c_str());
            }
        } else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
            if (!m_ConsoleHistory.empty() && m_ConsoleHistoryCursor >= 0) {
                ++m_ConsoleHistoryCursor;
                if (m_ConsoleHistoryCursor >= static_cast<int>(m_ConsoleHistory.size())) {
                    m_ConsoleHistoryCursor = -1;
                    m_ConsoleInput[0] = '\0';
                } else {
                    std::snprintf(m_ConsoleInput, sizeof(m_ConsoleInput), "%s",
                                  m_ConsoleHistory[static_cast<std::size_t>(
                                      m_ConsoleHistoryCursor)].c_str());
                }
            }
        }
    }
    if (submitted) {
        const std::string command = Trim(m_ConsoleInput);
        m_ConsoleInput[0] = '\0';
        if (!command.empty()) {
            m_ConsoleHistory.push_back(command);
            if (m_ConsoleHistory.size() > 64) {
                m_ConsoleHistory.erase(m_ConsoleHistory.begin());
            }
            m_ConsoleHistoryCursor = -1;
            ExecuteConsoleCommand(command);
        }
        m_ConsoleFocusInput = true;
    }
    if (ImGui::IsItemHovered() && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        SetConsoleVisible(false);
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

void Application::ExecuteConsoleCommand(const std::string& commandLine) {
    const std::size_t separator = commandLine.find(';');
    if (separator != std::string::npos) {
        std::size_t start = 0;
        while (start <= commandLine.size()) {
            const std::size_t end = commandLine.find(';', start);
            const std::string part = Trim(commandLine.substr(
                start, end == std::string::npos ? std::string::npos : end - start));
            if (!part.empty()) {
                ExecuteConsoleCommand(part);
            }
            if (end == std::string::npos) break;
            start = end + 1;
        }
        return;
    }

    LogMessage(LogLevel::Info, "> " + commandLine);
    std::istringstream stream(commandLine);
    std::string command;
    stream >> command;
    std::transform(command.begin(), command.end(), command.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    if (command == "help" || command == "?") {
        LogMessage(LogLevel::Info,
                   "Commands: help, clear, stats, gpu, renderer <opengl|vulkan|dx12>, "
                   "preview <detach|dock|toggle>, library <collapse|expand|toggle>, "
                   "console <show|hide|toggle>, theme <graphite|midnight>, "
                   "select <node id>, call <function.mffunction>, "
                   "undo, redo, autosave, recover, discard-autosave, "
                   "buffer <name>, probe <on|off|toggle>, comment, group, selectall, "
                   "align <mode>, distribute <h|v>, bookmark, "
                   "settings <blend|shading|clip|twosided> <value>, collapse, delete <path>, instance, param <name> <value>, previewscene, uebridge, "
                   "packages, addallpackages, pluginmanager, texturepicker, viewtexture, previewnode, stopnodepreview, convertconstant, close, open <graph.json>, save, saveas, quit. Plugin commands appear here after a plugin is loaded.");
        return;
    }
    if (command == "clear" || command == "cls") {
        ClearConsole();
        return;
    }
    if (command == "stats") {
        const GraphStats stats = m_Graph.ComputeStats();
        char text[256];
        std::snprintf(text, sizeof(text),
                      "nodes=%zu links=%zu params=%zu textures=%zu fps=%.0f backend=%s",
                      stats.nodeCount, stats.linkCount, stats.parameterCount,
                      stats.textureCount, ImGui::GetIO().Framerate,
                      m_PreviewRenderer.BackendName());
        LogMessage(LogLevel::Info, text);
        return;
    }
    if (command == "select" || command == "node") {
        unsigned long long id = 0;
        stream >> id;
        Node* node = m_Graph.FindNode(static_cast<NodeId>(id));
        if (!node) {
            LogMessage(LogLevel::Error, "No node with that id.");
            return;
        }
        if (m_GraphView) {
            m_GraphView->SelectNode(node->id);
        }
        LogMessage(LogLevel::Info, "Selected node #" + std::to_string(node->id) + " (" +
                                       node->type + ").");
        return;
    }
    if (command == "call" || command == "function") {
        std::string path;
        std::getline(stream >> std::ws, path);
        if (path.size() >= 2 &&
            ((path.front() == '"' && path.back() == '"') ||
             (path.front() == '\'' && path.back() == '\''))) {
            path = path.substr(1, path.size() - 2);
        }
        if (path.empty()) {
            LogMessage(LogLevel::Error, "Usage: call <function.mffunction>");
            return;
        }
        const fs::path resolved = ResolveFunctionPath(path);
        if (resolved.empty()) {
            LogMessage(LogLevel::Error, "Material function not found: " + path);
            return;
        }
        std::string callError;
        Node* node = m_GraphView
                         ? m_GraphView->AddFunctionCallAtViewCenter(
                               PathUtil::ToUtf8(resolved), &callError)
                         : nullptr;
        if (!node) {
            LogMessage(LogLevel::Error, "Could not create function call: " + callError);
            return;
        }
        LogMessage(LogLevel::Info, "Created function call #" + std::to_string(node->id) +
                                       " for " + resolved.filename().string() + ".");
        return;
    }
    if (command == "undo") {
        Undo();
        return;
    }
    if (command == "redo") {
        Redo();
        return;
    }
    if (command == "buffer") {
        std::string name;
        stream >> name;
        name = ToLower(name);
        name.erase(std::remove_if(name.begin(), name.end(),
                                  [](unsigned char ch) {
                                      return ch == '_' || ch == '-' || ch == ' ';
                                  }),
                   name.end());
        PreviewBuffer buffer = m_PreviewBuffer;
        if (name == "lit") buffer = PreviewBuffer::Lit;
        else if (name == "basecolor" || name == "base") buffer = PreviewBuffer::BaseColor;
        else if (name == "metallic") buffer = PreviewBuffer::Metallic;
        else if (name == "specular") buffer = PreviewBuffer::Specular;
        else if (name == "roughness") buffer = PreviewBuffer::Roughness;
        else if (name == "emissive") buffer = PreviewBuffer::Emissive;
        else if (name == "normal") buffer = PreviewBuffer::Normal;
        else if (name == "ao" || name == "ambientocclusion") buffer = PreviewBuffer::AmbientOcclusion;
        else if (name == "opacity" || name == "op") buffer = PreviewBuffer::Opacity;
        else if (name == "worldnormal") buffer = PreviewBuffer::WorldNormal;
        else if (name == "depth") buffer = PreviewBuffer::Depth;
        else {
            LogMessage(LogLevel::Error,
                       "Usage: buffer <lit|basecolor|metallic|specular|roughness|"
                       "emissive|normal|ao|opacity|worldnormal|depth>");
            return;
        }
        m_PreviewBuffer = buffer;
        m_LastContentRevision = 0;
        m_ForceShaderRefresh = true;
        m_PreviewNeedsRender = true;
        LogMessage(LogLevel::Info,
                   std::string("Preview buffer: ") + PreviewBufferName(buffer));
        return;
    }
    if (command == "comment") {
        if (m_GraphView) {
            m_GraphView->AddCommentAtViewCenter();
        }
        return;
    }
    if (command == "group") {
        if (m_GraphView) {
            LogMessage(LogLevel::Info, FormatLocalized("Grouping %d selected node(s).",
                                                       m_GraphView->SelectedNodeCount()));
            m_GraphView->GroupSelectedNodes();
        }
        return;
    }
    if (command == "selectall") {
        if (m_GraphView) {
            m_GraphView->SelectAllNodes();
            LogMessage(LogLevel::Info, FormatLocalized("Selected %d node(s).",
                                                       m_GraphView->SelectedNodeCount()));
        }
        return;
    }
    if (command == "align") {
        std::string mode;
        stream >> mode;
        mode = ToLower(mode);
        if (!m_GraphView) return;
        if (mode == "left") m_GraphView->AlignSelected(NodeGraphView::AlignMode::Left);
        else if (mode == "center" || mode == "hcenter")
            m_GraphView->AlignSelected(NodeGraphView::AlignMode::CenterHorizontal);
        else if (mode == "right") m_GraphView->AlignSelected(NodeGraphView::AlignMode::Right);
        else if (mode == "top") m_GraphView->AlignSelected(NodeGraphView::AlignMode::Top);
        else if (mode == "middle" || mode == "vcenter")
            m_GraphView->AlignSelected(NodeGraphView::AlignMode::CenterVertical);
        else if (mode == "bottom") m_GraphView->AlignSelected(NodeGraphView::AlignMode::Bottom);
        else LogMessage(LogLevel::Error, "Usage: align <left|center|right|top|middle|bottom>");
        return;
    }
    if (command == "distribute") {
        std::string axis;
        stream >> axis;
        axis = ToLower(axis);
        if (!m_GraphView) return;
        if (axis == "h" || axis == "horizontal") m_GraphView->DistributeSelected(true);
        else if (axis == "v" || axis == "vertical") m_GraphView->DistributeSelected(false);
        else LogMessage(LogLevel::Error, "Usage: distribute <h|v>");
        return;
    }
    if (command == "bookmark") {
        if (m_GraphView) {
            m_GraphView->AddBookmarkFromSelection();
            LogMessage(LogLevel::Info,
                       FormatLocalized("Bookmarks: %d",
                                       static_cast<int>(m_GraphView->Bookmarks().size())));
        }
        return;
    }
    if (command == "settings") {
        std::string key;
        std::string value;
        stream >> key >> value;
        key = ToLower(key);
        if (key == "blend") {
            m_Graph.Settings().blendMode = value;
            m_Graph.MarkContentChanged();
        } else if (key == "shading") {
            m_Graph.Settings().shadingModel = value;
            m_Graph.MarkContentChanged();
        } else if (key == "clip") {
            try {
                m_Graph.Settings().opacityMaskClip = std::clamp(std::stof(value), 0.0f, 1.0f);
                m_Graph.MarkContentChanged();
            } catch (...) {
                LogMessage(LogLevel::Error, "Usage: settings clip <0..1>");
                return;
            }
        } else if (key == "twosided") {
            m_Graph.Settings().twoSided =
                value == "on" || value == "true" || value == "1";
            m_Graph.MarkContentChanged();
        } else {
            LogMessage(LogLevel::Error,
                       "Usage: settings <blend|shading|clip|twosided> <value>");
            return;
        }
        LogMessage(LogLevel::Info, "Material settings updated.");
        m_FocusMaterialSettingsTab = true;
        return;
    }
    if (command == "collapse") {
        CollapseSelectionToFunction();
        return;
    }
    if (command == "instance") {
        if (!m_Project.IsOpen()) {
            LogMessage(LogLevel::Error, "No project is open.");
            return;
        }
        std::string instanceError;
        if (!CreateMaterialInstanceFromMaterial(m_Project.MainMaterialPath(),
                                                &instanceError)) {
            LogMessage(LogLevel::Error, instanceError);
        }
        return;
    }
    if (command == "previewscene") {
        m_PreviewSceneModalPending = true;
        return;
    }
    if (command == "uebridge") {
        m_UeBridgePending = true;
        return;
    }
    if (command == "cd") {
        std::string folder;
        stream >> folder;
        NavigateProjectFolder(folder);
        return;
    }
    if (command == "uebatch") {
        std::string path;
        stream >> path;
        if (path.empty()) {
            LogMessage(LogLevel::Error, "Usage: uebatch <manifest.json>");
            return;
        }
        std::string batchError;
        if (!WriteUeBatchManifest(PathUtil::FromUtf8(path), &batchError)) {
            LogMessage(LogLevel::Error, batchError);
        } else {
            LogMessage(LogLevel::Info, "UE batch manifest written: " + path);
        }
        return;
    }
    if (command == "param") {
        if (m_DocumentKind != DocumentKind::Instance) {
            LogMessage(LogLevel::Error, "The active document is not a material instance.");
            return;
        }
        std::string name;
        stream >> name;
        MaterialParameterInfo* parameter = FindMaterialParameter(m_InstanceParameters, name);
        if (!parameter) {
            LogMessage(LogLevel::Error, "Unknown material parameter: " + name);
            return;
        }
        if (parameter->type == PinType::Float) {
            float value = 0.0f;
            if (!(stream >> value)) {
                LogMessage(LogLevel::Error, "Usage: param <name> <scalar>");
                return;
            }
            const PropertyValue property = PropertyValue::Float(value);
            m_Instance.SetOverride(parameter->name, parameter->type, property);
            Node* node = m_Graph.FindNode(parameter->nodeId);
            if (node) node->properties["value"] = property;
            m_Graph.MarkContentChanged();
            SyncActiveDocument();
            m_LastContentRevision = 0;
            m_ForceShaderRefresh = true;
            LogMessage(LogLevel::Info, "Instance parameter updated: " + name);
        } else {
            LogMessage(LogLevel::Error,
                       "Console overrides currently support scalar parameters.");
        }
        return;
    }
    if (command == "delete") {
        std::string relative;
        stream >> relative;
        if (!m_Project.IsOpen()) {
            LogMessage(LogLevel::Error, "No project is open.");
            return;
        }
        std::string deleteError;
        if (m_Project.DeleteEntry(relative, true, &deleteError)) {
            m_FunctionAssetsDirty = true;
            RefreshProjectIntegrity();
            RefreshProjectEntries(false);
            LogMessage(LogLevel::Info, "Deleted project entry: " + relative);
        } else {
            LogMessage(LogLevel::Error, deleteError);
        }
        return;
    }
    if (command == "probe") {
        std::string state;
        stream >> state;
        state = ToLower(state);
        if (state == "on" || state == "show" || state == "1") {
            m_PixelProbeEnabled = true;
        } else if (state == "off" || state == "hide" || state == "0") {
            m_PixelProbeEnabled = false;
        } else if (state == "toggle") {
            m_PixelProbeEnabled = !m_PixelProbeEnabled;
        } else {
            LogMessage(LogLevel::Error, "Usage: probe <on|off|toggle>");
            return;
        }
        LogMessage(LogLevel::Info,
                   m_PixelProbeEnabled ? "Pixel probe enabled." : "Pixel probe disabled.");
        return;
    }
    if (command == "autosave") {
        WriteAutosave(true);
        LogMessage(LogLevel::Info, "Autosave written to " + PathUtil::ToUtf8(AutosaveDataPath()));
        return;
    }
    if (command == "recover" || command == "recover-autosave") {
        RecoverAutosave();
        return;
    }
    if (command == "discard-autosave") {
        DiscardAutosave();
        return;
    }
    if (command == "gpu") {
        LogMessage(LogLevel::Info, "OpenGL: " + m_GlVersionString);
        LogMessage(LogLevel::Info, "Preview: " + m_PreviewRenderer.BackendDetails());
        if (!m_PreviewRenderer.LastBackendError().empty()) {
            LogMessage(LogLevel::Warning, m_PreviewRenderer.LastBackendError());
        }
        return;
    }
    if (command == "renderer" || command == "gpu-api" || command == "api") {
        std::string argument;
        stream >> argument;
        if (argument.empty()) {
            LogMessage(LogLevel::Info, std::string("Active preview renderer: ") +
                                           m_PreviewRenderer.BackendName() + " (" +
                                           RenderApiName(m_PreviewRenderer.Backend()) + ")");
            return;
        }
        const RenderApi api = ParseRenderApi(argument, RenderApi::OpenGL);
        SetRenderApi(api);
        return;
    }
    if (command == "preview") {
        std::string argument;
        stream >> argument;
        if (argument == "detach" || argument == "float") {
            DetachPreview();
        } else if (argument == "dock") {
            DockPreview();
        } else if (argument == "toggle") {
            m_PreviewDetached ? DockPreview() : DetachPreview();
        } else {
            LogMessage(LogLevel::Info, m_PreviewDetached ? "Preview is detached."
                                                         : "Preview is docked.");
        }
        return;
    }
    if (command == "library") {
        std::string argument;
        stream >> argument;
        if (argument == "collapse" || argument == "hide") {
            SetNodeLibraryCollapsed(true);
        } else if (argument == "expand" || argument == "show") {
            SetNodeLibraryCollapsed(false);
        } else if (argument == "toggle") {
            SetNodeLibraryCollapsed(!m_NodeLibraryCollapsed);
        } else {
            LogMessage(LogLevel::Info, m_NodeLibraryCollapsed ? "Node library is collapsed."
                                                              : "Node library is expanded.");
        }
        return;
    }
    if (command == "console") {
        std::string argument;
        stream >> argument;
        if (argument == "show") SetConsoleVisible(true);
        else if (argument == "hide") SetConsoleVisible(false);
        else if (argument == "toggle") SetConsoleVisible(!m_ConsoleVisible);
        else LogMessage(LogLevel::Info, m_ConsoleVisible ? "Console is visible."
                                                         : "Console is hidden.");
        return;
    }
    if (command == "theme") {
        std::string argument;
        stream >> argument;
        if (argument == "graphite" || argument == "midnight") {
            SetTheme(argument);
        } else {
            LogMessage(LogLevel::Info, "Themes: graphite, midnight");
        }
        return;
    }
    if (command == "open") {
        std::string path;
        std::getline(stream, path);
        path = Trim(path);
        if (path.empty()) {
            LogMessage(LogLevel::Error, "Usage: open <path-to-graph.json>");
            return;
        }
        std::string error;
        if (!LoadDocument(path, &error)) {
            LogMessage(LogLevel::Error, "Open failed: " + error);
        } else {
            LogMessage(LogLevel::Info, "Opened " + path);
        }
        return;
    }
    if (command == "save") {
        RequestSave(false);
        return;
    }
    if (command == "saveas") {
        RequestSave(true);
        return;
    }
    if (command == "quit" || command == "exit") {
        m_WantsExit = true;
        return;
    }
    if (command == "close") {
        if (m_ActiveDocument >= 0) RequestCloseDocument(m_ActiveDocument);
        return;
    }
    if (command == "version") {
        LogMessage(LogLevel::Info, "MaterialForge " MF_VERSION_STRING);
        return;
    }
    if (command == "packages" || command == "texturepackages") {
        RequestTexturePackages();
        return;
    }
    if (command == "addallpackages") {
        const std::vector<TexturePackage> packages = BuildTexturePackages();
        if (packages.empty()) {
            LogMessage(LogLevel::Warning, "No built-in texture package was found.");
            return;
        }
        for (const TexturePackage& package : packages) {
            if (package.id != "all") continue;
            int imported = 0;
            int skipped = 0;
            std::string error;
            if (!ImportTexturePackage(package, &imported, &skipped, &error)) {
                SetStatus(LocalizedErrorText(error), true);
            } else {
                SetStatus(FormatLocalized("Imported %d texture(s); %d already existed.",
                                          imported, skipped));
            }
            return;
        }
        return;
    }
    if (command == "pluginmanager" || command == "plugins") {
        m_OpenPluginManager = true;
        m_PluginManagerPending = true;
        return;
    }
    if (command == "texturepicker") {
        for (const auto& node : m_Graph.Nodes()) {
            if (node->type != "TextureSample" && node->type != "TextureSampleParameter2D") {
                continue;
            }
            if (Pin* texturePin = node->FindInput("Texture")) {
                OpenTexturePicker(node->id, texturePin->id);
                return;
            }
        }
        LogMessage(LogLevel::Warning, "The current material has no TextureSample texture input.");
        return;
    }
    if (command == "previewnode") {
        NodeId target = m_GraphView ? m_GraphView->SelectedNodeId() : 0;
        if (target == 0) {
            for (const auto& node : m_Graph.Nodes()) {
                if (node->IsMaterialOutput()) continue;
                if (!node->outputs.empty()) {
                    target = node->id;
                    break;
                }
            }
        }
        if (target != 0) {
            StartNodePreview(target, 0);
        } else {
            LogMessage(LogLevel::Warning, "Select a node with an output before previewing it.");
        }
        return;
    }
    if (command == "stopnodepreview") {
        StopNodePreview();
        return;
    }
    if (command == "viewtexture") {
        const std::vector<TexturePickerItem> textures = CollectTexturePickerItems();
        if (!textures.empty()) {
            OpenTextureViewer(textures.front().path);
        } else {
            LogMessage(LogLevel::Warning, "No project texture is available to view.");
        }
        return;
    }
    if (command == "convertconstant") {
        Node* constant = nullptr;
        for (const auto& node : m_Graph.Nodes()) {
            if (node->type == "Constant" || node->type == "Constant2Vector" ||
                node->type == "Constant3Vector" || node->type == "Constant4Vector") {
                constant = node.get();
                break;
            }
        }
        if (!constant || !m_GraphView) {
            LogMessage(LogLevel::Warning, "No constant node was found in this material.");
            return;
        }
        std::string convertError;
        if (!m_GraphView->ConvertConstantNodeToParameter(constant->id, &convertError)) {
            SetStatus(LocalizedErrorText(convertError), true);
            return;
        }
        m_Graph.MarkContentChanged();
        SyncActiveDocument();
        if (!m_DocumentPath.empty()) {
            std::string saveError;
            SaveDocument(PathUtil::ToUtf8(m_DocumentPath), true, &saveError);
        }
        SetStatus(m_GraphView->TakeLastMessage());
        return;
    }
    std::string pluginArguments;
    std::getline(stream, pluginArguments);
    if (m_PluginManager.ExecuteCommand(command, Trim(pluginArguments))) {
        return;
    }
    LogMessage(LogLevel::Error, "Unknown command: " + command + ". Type 'help' for a list.");
}

void Application::ProbeRenderBackends() {
    const RenderApi apis[] = {RenderApi::OpenGL, RenderApi::Vulkan, RenderApi::DirectX12};
    for (int i = 0; i < 3; ++i) {
        std::string details;
        std::string error;
        m_BackendProbeOk[i] = PreviewRenderer::ProbeBackend(apis[i], &details, &error);
        m_BackendProbeText[i] = details;
        m_BackendProbeError[i] = error;
    }
    m_BackendProbeDone = true;
}

void Application::SetRenderApi(RenderApi api) {
    if (m_PreviewRenderer.Backend() == api && m_PreviewRenderer.IsInitialized()) {
        m_Settings.renderApi = RenderApiName(api);
        SaveUiSettings();
        SetStatus(FormatLocalized("Preview renderer is already %s.",
                                  RenderApiDisplayName(api)));
        return;
    }
    std::string error;
    if (!m_PreviewRenderer.SetBackend(api, &error)) {
        LogMessage(LogLevel::Error, error);
        SetStatus(error, true);
        m_BackendProbeDone = false;
        return;
    }
    m_Settings.renderApi = RenderApiName(api);
    SaveUiSettings();
    m_ForceShaderRefresh = true;
    m_LastContentRevision = 0;
    m_BackendProbeDone = false;
    if (api == RenderApi::OpenGL) {
        ApplyPreviewSceneSettings();
    }
    const std::string message = FormatLocalized("Preview renderer switched to %s.",
                                                RenderApiDisplayName(api));
    LogMessage(LogLevel::Info, message);
    SetStatus(message);
}

void Application::DrawPreferences() {
    if (!m_OpenPreferences) {
        return;
    }
    if (!m_BackendProbeDone) {
        ProbeRenderBackends();
    }
    ImGui::SetNextWindowSize(ImVec2(Ui::S(700.0f), Ui::S(620.0f)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin(std::string(Tr("Preferences")).append("###Preferences").c_str(),
                      &m_OpenPreferences,
                      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    ImGui::SetNextItemWidth(-Ui::S(70.0f));
    ImGui::InputTextWithHint("##preferences_search", Tr("Search settings..."),
                             m_PreferencesSearch, sizeof(m_PreferencesSearch));
    const std::string query = ToLower(Trim(m_PreferencesSearch));
    if (!query.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton(Tr("Clear"))) {
            m_PreferencesSearch[0] = '\0';
        }
    }
    const auto matches = [&](std::initializer_list<const char*> keywords) {
        if (query.empty()) return true;
        for (const char* keyword : keywords) {
            if (ToLower(Tr(keyword)).find(query) != std::string::npos) return true;
            if (ToLower(keyword).find(query) != std::string::npos) return true;
        }
        return false;
    };

    const auto drawRendering = [&]() {
        Ui::SectionLabel(Tr("PREVIEW RENDERING PIPELINE"));
        ImGui::TextDisabled("%s", Tr("Choose the GPU API used to shade the live material preview. The editor shell itself stays on OpenGL."));
        ImGui::Spacing();
        const RenderApi apis[] = {RenderApi::OpenGL, RenderApi::Vulkan, RenderApi::DirectX12};
        for (int i = 0; i < 3; ++i) {
            const RenderApi api = apis[i];
            ImGui::PushID(i);
            const bool active = m_PreviewRenderer.Backend() == api;
            const bool available = m_BackendProbeOk[i];
            ImGui::BeginDisabled(!available);
            if (ImGui::RadioButton(RenderApiDisplayName(api), active)) {
                SetRenderApi(api);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (active) {
                Ui::Badge(Tr("ACTIVE"), ImVec4(0.31f, 0.84f, 0.78f, 1.0f));
            } else if (available) {
                Ui::Badge(Tr("AVAILABLE"), ImVec4(0.55f, 0.70f, 0.45f, 1.0f));
            } else {
                Ui::Badge(Tr("UNAVAILABLE"), ImVec4(0.70f, 0.42f, 0.38f, 1.0f));
            }
            const std::string& details = m_BackendProbeText[i];
            const std::string& error = m_BackendProbeError[i];
            if (!error.empty()) {
                ImGui::TextWrapped("%s", error.c_str());
            } else if (!details.empty()) {
                ImGui::TextDisabled("%s", details.c_str());
            }
            ImGui::Spacing();
            ImGui::PopID();
        }
        if (Ui::GhostButton(Tr("Refresh detection"), ImVec2(Ui::S(150.0f), Ui::S(26.0f)))) {
            m_BackendProbeDone = false;
            ProbeRenderBackends();
        }
    };
    const auto drawActiveBackend = [&]() {
        Ui::SectionLabel(Tr("ACTIVE BACKEND"));
        ImGui::TextDisabled("%s", m_PreviewRenderer.BackendDetails().c_str());
        if (m_PreviewRenderer.FellBackToOpenGL()) {
            ImGui::TextColored(ImVec4(0.96f, 0.76f, 0.34f, 1.0f), "%s",
                               Tr("The requested backend could not start; OpenGL is active."));
        }
        ImGui::Spacing();
        ImGui::TextDisabled(Tr("OpenGL context: %s"), m_GlVersionString.c_str());
        if (m_PreviewRenderer.Backend() != RenderApi::OpenGL) {
            ImGui::TextWrapped("%s",
                               Tr("Vulkan and DirectX 12 shade the same generated material shader through a compute pipeline and present the result in the preview panel."));
        }
    };
    const auto drawTheme = [&]() {
        Ui::SectionLabel(Tr("THEME"));
        if (Ui::ToggleButton(Tr("Graphite Gray"), m_Theme != "midnight",
                             ImVec2(Ui::S(150.0f), Ui::S(28.0f)))) {
            SetTheme("graphite");
        }
        ImGui::SameLine();
        if (Ui::ToggleButton(Tr("Midnight Blue"), m_Theme == "midnight",
                             ImVec2(Ui::S(150.0f), Ui::S(28.0f)))) {
            SetTheme("midnight");
        }
    };
    const auto drawLanguage = [&]() {
        Ui::SectionLabel(Tr("LANGUAGE"));
        if (Ui::ToggleButton(Tr("English"),
                             Localization::Get().CurrentLanguage() == Language::English,
                             ImVec2(Ui::S(120.0f), Ui::S(28.0f)))) {
            SetLanguage(Language::English);
        }
        ImGui::SameLine();
        if (Ui::ToggleButton(Tr("Chinese"),
                             Localization::Get().CurrentLanguage() == Language::Chinese,
                             ImVec2(Ui::S(120.0f), Ui::S(28.0f)))) {
            SetLanguage(Language::Chinese);
        }
    };
    const auto drawWorkspace = [&]() {
        Ui::SectionLabel(Tr("WORKSPACE"));
        bool libraryCollapsed = m_NodeLibraryCollapsed;
        if (ImGui::Checkbox(Tr("Collapse the node library"), &libraryCollapsed)) {
            SetNodeLibraryCollapsed(libraryCollapsed);
        }
        bool consoleVisible = m_ConsoleVisible;
        if (ImGui::Checkbox(Tr("Show the CMD console"), &consoleVisible)) {
            SetConsoleVisible(consoleVisible);
        }
        bool previewDetached = m_PreviewDetached;
        if (ImGui::Checkbox(Tr("Float the live preview"), &previewDetached)) {
            if (previewDetached) DetachPreview();
            else DockPreview();
        }
    };
    const auto drawAutosave = [&]() {
        Ui::SectionLabel(Tr("AUTOSAVE"));
        bool autosaveEnabled = m_Settings.autosaveEnabled;
        if (ImGui::Checkbox(Tr("Enable autosave"), &autosaveEnabled)) {
            m_Settings.autosaveEnabled = autosaveEnabled;
            if (!autosaveEnabled) {
                DeleteAutosave();
            }
            SaveUiSettings();
        }
        ImGui::SetNextItemWidth(Ui::S(220.0f));
        int autosaveInterval = m_Settings.autosaveIntervalSeconds;
        if (ImGui::SliderInt(Tr("Autosave interval"), &autosaveInterval, 2, 120, "%d s")) {
            m_Settings.autosaveIntervalSeconds = std::clamp(autosaveInterval, 2, 120);
            SaveUiSettings();
        }
        ImGui::TextDisabled("%s", Tr("Saves dirty documents periodically and offers recovery after an unexpected exit."));
    };
    const auto drawConsole = [&]() {
        Ui::SectionLabel(Tr("CMD CONSOLE"));
        ImGui::TextWrapped("%s", Tr("The console records errors and warnings from the shader compiler, GPU backends, project loader and file browser. Type 'help' in the command line for available commands."));
        ImGui::Spacing();
        bool consoleVisible = m_ConsoleVisible;
        if (ImGui::Checkbox(Tr("Show the CMD console"), &consoleVisible)) {
            SetConsoleVisible(consoleVisible);
        }
        ImGui::SetNextItemWidth(Ui::S(220.0f));
        float consoleHeight = m_ConsoleHeight;
        if (ImGui::SliderFloat(Tr("Console height"), &consoleHeight, Ui::S(110.0f),
                               Ui::S(520.0f), "%.0f px")) {
            m_ConsoleHeight = consoleHeight;
        }
        ImGui::Spacing();
        if (Ui::GhostButton(Tr("Clear console"), ImVec2(Ui::S(130.0f), Ui::S(26.0f)))) {
            ClearConsole();
        }
        ImGui::SameLine();
        if (Ui::GhostButton(Tr("Copy console"), ImVec2(Ui::S(130.0f), Ui::S(26.0f)))) {
            std::string text;
            for (const ConsoleEntry& entry : m_ConsoleEntries) {
                text += entry.message + "\n";
            }
            ImGui::SetClipboardText(text.c_str());
        }
    };
    const auto drawMcp = [&]() {
        Ui::SectionLabel(Tr("MCP SERVER"));
        ImGui::TextWrapped("%s", Tr("Expose MaterialForge tools to external MCP clients on localhost. Disabled by default."));
        bool enabled = m_Settings.mcpEnabled;
        if (ImGui::Checkbox(Tr("Enable MCP server"), &enabled)) {
            m_Settings.mcpEnabled = enabled;
            std::string mcpError;
            if (enabled) {
                if (!StartMcpServer(&mcpError)) {
                    m_Settings.mcpEnabled = false;
                    SetStatus(LocalizedErrorText(mcpError), true);
                } else {
                    SetStatus(FormatLocalized("MCP server listening on %s",
                                              m_McpServer->Endpoint().c_str()));
                }
            } else {
                StopMcpServer();
                SetStatus(Tr("MCP server disabled."));
            }
            SaveMcpSettings();
        }
        ImGui::SetNextItemWidth(Ui::S(150.0f));
        int port = m_Settings.mcpPort;
        if (ImGui::InputInt(Tr("Port"), &port)) {
            m_Settings.mcpPort = std::clamp(port, 1024, 65535);
        }
        ImGui::SameLine();
        if (Ui::GhostButton(Tr("Apply Port"), ImVec2(Ui::S(96.0f), 0.0f))) {
            SaveUiSettings();
            if (m_Settings.mcpEnabled) {
                std::string mcpError;
                if (!StartMcpServer(&mcpError)) {
                    SetStatus(LocalizedErrorText(mcpError), true);
                }
            }
        }
        ImGui::TextDisabled("%s", m_McpStatus.c_str());
        if (m_McpServer && m_McpServer->IsRunning()) {
            const std::string endpoint = m_McpServer->Endpoint();
            ImGui::TextColored(ImVec4(0.31f, 0.84f, 0.78f, 1.0f), "%s", endpoint.c_str());
            ImGui::SameLine();
            if (Ui::GhostButton(Tr("Copy Endpoint"), ImVec2(Ui::S(112.0f), 0.0f))) {
                ImGui::SetClipboardText(endpoint.c_str());
            }
        }
    };

    const float footerHeight = Ui::S(48.0f);
    bool anyMatch = false;
    const auto section = [&](std::initializer_list<const char*> keywords, auto&& draw) {
        if (!matches(keywords)) return;
        draw();
        anyMatch = true;
        ImGui::Separator();
    };
    if (!query.empty()) {
        ImGui::BeginChild("##preferences_search_results", ImVec2(0.0f, -footerHeight),
                          ImGuiChildFlags_None);
        section({"preview", "rendering", "pipeline", "gpu", "opengl", "vulkan", "directx", "渲染", "管线"},
                drawRendering);
        section({"backend", "msaa", "glsl", "opengl", "预览", "后端"}, drawActiveBackend);
        section({"theme", "graphite", "midnight", "主题", "外观"}, drawTheme);
        section({"language", "english", "chinese", "语言", "中文"}, drawLanguage);
        section({"workspace", "node library", "console", "float", "preview", "界面", "工作区", "节点库"},
                drawWorkspace);
        section({"autosave", "recovery", "interval", "自动保存", "恢复", "间隔"}, drawAutosave);
        section({"console", "cmd", "command", "控制台", "命令"}, drawConsole);
        section({"mcp", "model context protocol", "server", "port", "automation", "外部", "自动化"},
                drawMcp);
        if (!anyMatch) {
            ImGui::TextDisabled("%s", Tr("No matching settings."));
        }
        ImGui::EndChild();
    } else {
        if (ImGui::BeginTabBar("##PreferencesTabs")) {
            if (ImGui::BeginTabItem(Tr("Rendering"))) {
                drawRendering();
                ImGui::Separator();
                drawActiveBackend();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(Tr("Interface"))) {
                drawTheme();
                ImGui::Spacing();
                drawLanguage();
                ImGui::Spacing();
                drawWorkspace();
                ImGui::Spacing();
                drawAutosave();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(Tr("Console"))) {
                drawConsole();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(Tr("MCP"))) {
                drawMcp();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }

    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - Ui::S(38.0f));
    if (ImGui::Button(Tr("Close"), ImVec2(Ui::S(100.0f), Ui::S(28.0f)))) {
        m_OpenPreferences = false;
        SaveUiSettings();
    }
    ImGui::End();
}

void Application::RequestInstallPlugin() {
    m_FileAction = FileAction::InstallPlugin;
    const fs::path start = m_PluginManager.RootPath();
    m_FileBrowser.Open(Tr("Select Plugin Folder"), FileBrowser::Mode::SelectFolder,
                       start, "", {});
}

void Application::RequestCreatePlugin() {
    m_PluginCreateName = "MyPlugin";
    m_PluginCreateId = "com.example.myplugin";
    m_PluginCreateAuthor.clear();
    m_PluginCreateDescription.clear();
    m_PluginCreateLocation =
        PathUtil::ToUtf8(PathUtil::DefaultProjectsDirectory() / "MaterialForge Plugins");
    m_PluginCreateError.clear();
    m_OpenCreatePlugin = true;
    m_PluginCreateModalPending = true;
}

void Application::CreatePluginTemplate(const std::string& parentDirectory,
                                       const std::string& pluginName,
                                       const std::string& pluginId,
                                       const std::string& author,
                                       const std::string& description,
                                       std::string* error) {
    if (error) error->clear();
    std::string cleanedName;
    std::string reason;
    if (!MaterialProject::IsValidFolderName(pluginName, &reason)) {
        if (error) *error = reason;
        return;
    }
    cleanedName = Trim(pluginName);
    if (!PluginManager::IsValidPluginId(pluginId, &reason)) {
        if (error) *error = reason;
        return;
    }
    if (parentDirectory.empty()) {
        if (error) *error = "Choose a folder for the plugin project.";
        return;
    }
    const fs::path parent = PathUtil::FromUtf8(parentDirectory);
    const fs::path target = parent / PathUtil::FromUtf8(cleanedName);
    std::error_code ec;
    if (fs::exists(target, ec)) {
        if (error) *error = "The plugin folder already exists: " + PathUtil::ToUtf8(target);
        return;
    }
    fs::create_directories(target / "src", ec);
    if (ec) {
        if (error) *error = "Could not create the plugin folder: " + ec.message();
        return;
    }
    fs::create_directories(target / "include", ec);
    fs::create_directories(target / "docs", ec);

    const auto writeFile = [&](const fs::path& path, const std::string& contents) {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream) return false;
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        return static_cast<bool>(stream);
    };
    const auto escapeCpp = [](std::string value) {
        std::string out;
        for (char ch : value) {
            if (ch == '\\' || ch == '"') out.push_back('\\');
            out.push_back(ch);
        }
        return out;
    };
    std::string targetName = "MFPlugin_";
    for (unsigned char ch : pluginId) {
        targetName.push_back(std::isalnum(ch) ? static_cast<char>(ch) : '_');
    }
    const std::string commandName = pluginId + ".hello";
    const std::string manifest = nlohmann::json({
        {"schema", "materialforge.plugin"},
        {"schemaVersion", 1},
        {"id", pluginId},
        {"name", cleanedName},
        {"version", "1.0.0"},
        {"author", author},
        {"description", description},
        {"entry", cleanedName + ".dll"},
        {"enabled", true},
    }).dump(2) + "\n";

    const std::string header = R"(#pragma once

// MaterialForge plugin ABI. Keep this file in sync with the host SDK.
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
)";

    const std::string source =
        "#include \"MaterialForgePlugin.h\"\n\n"
        "static const MfPluginApi* g_api = nullptr;\n\n"
        "static void OnHello(const char* args, void* user) {\n"
        "    (void)args;\n"
        "    (void)user;\n"
        "    if (g_api && g_api->log) {\n"
        "        g_api->log(g_api->host, 0, \"Hello from " + escapeCpp(cleanedName) +
        ".\");\n"
        "    }\n"
        "}\n\n"
        "MF_PLUGIN_EXPORT int MaterialForgePluginInit(const MfPluginApi* api) {\n"
        "    if (!api || api->api_version != MF_PLUGIN_API_VERSION) return 0;\n"
        "    g_api = api;\n"
        "    if (api->register_command) {\n"
        "        api->register_command(api->host, \"" + escapeCpp(commandName) +
        "\", \"Print a hello message from " + escapeCpp(cleanedName) + ".\", OnHello, nullptr);\n"
        "    }\n"
        "    if (api->log) api->log(api->host, 0, \"" + escapeCpp(cleanedName) +
        " initialized.\");\n"
        "    return 1;\n"
        "}\n\n"
        "MF_PLUGIN_EXPORT void MaterialForgePluginShutdown(void) {\n"
        "    if (g_api && g_api->log) g_api->log(g_api->host, 0, \"" +
        escapeCpp(cleanedName) + " shutting down.\");\n"
        "    g_api = nullptr;\n"
        "}\n";

    const std::string cmake =
        "cmake_minimum_required(VERSION 3.21)\n"
        "project(" + targetName + " LANGUAGES CXX)\n\n"
        "set(CMAKE_CXX_STANDARD 17)\n"
        "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n\n"
        "add_library(" + targetName + " SHARED src/plugin.cpp)\n"
        "target_include_directories(" + targetName + " PRIVATE include)\n"
        "if(MSVC)\n    target_compile_options(" + targetName + " PRIVATE /utf-8)\nendif()\n"
        "set_target_properties(" + targetName + " PROPERTIES OUTPUT_NAME \"" +
        cleanedName + "\")\n\n"
        "# Copy the built DLL next to plugin.json so Edit -> Plugin Manager ->\n"
        "# Add Plugin... can load this folder directly.\n"
        "add_custom_command(TARGET " + targetName + " POST_BUILD\n"
        "    COMMAND ${CMAKE_COMMAND} -E copy_if_different\n"
        "            $<TARGET_FILE:" + targetName + ">\n"
        "            \"${CMAKE_SOURCE_DIR}/" + cleanedName + ".dll\")\n";

    const std::string readme =
        "# " + cleanedName + "\n\n" + description + "\n\n"
        "MaterialForge plugin project generated by the built-in plugin wizard.\n\n"
        "## Build\n\n"
        "```powershell\ncmake -S . -B build\ncmake --build build --config Release\n```\n\n"
        "The DLL is copied next to `plugin.json`. In MaterialForge choose\n"
        "`Edit -> Plugin Manager... -> Add Plugin...` and select this folder.\n\n"
        "## Run\n\n"
        "Open the MaterialForge CMD console and run:\n\n```text\n" + commandName + "\n```\n";

    const std::string docs =
        "# MaterialForge Plugin API\n\n"
        "Plugins are Windows DLLs exporting `MaterialForgePluginInit` and optionally\n"
        "`MaterialForgePluginShutdown`.\n\n"
        "## Manifest\n\n"
        "`plugin.json` uses `schema: materialforge.plugin`, a unique `id`, display\n"
        "`name`/`version`, and an `entry` path to the DLL relative to the plugin folder.\n\n"
        "## Host API\n\n"
        "- `log(host, level, message)` writes to the MaterialForge console.\n"
        "- `runtime_version(host)` returns the host version string.\n"
        "- `register_command(host, name, description, callback, user)` adds a CMD console command.\n\n"
        "Commands receive the text after the command name as `args`.\n\n"
        "## Lifecycle\n\n"
        "Initialize once when the plugin is loaded. Release resources in the shutdown export.\n"
        "Do not block the UI thread for long periods inside command callbacks.\n";

    if (!writeFile(target / "plugin.json", manifest) ||
        !writeFile(target / "include" / "MaterialForgePlugin.h", header) ||
        !writeFile(target / "src" / "plugin.cpp", source) ||
        !writeFile(target / "CMakeLists.txt", cmake) ||
        !writeFile(target / "README.md", readme) ||
        !writeFile(target / "docs" / "PLUGIN_API.md", docs)) {
        if (error) *error = "Could not write one or more plugin template files.";
        return;
    }
    if (error) error->clear();
}

void Application::DrawPluginManager() {
    ImGui::SetNextWindowSize(ImVec2(Ui::S(760.0f), Ui::S(560.0f)), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(Tr("Plugin Manager"), &m_OpenPluginManager,
                                ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }
    ImGui::TextWrapped("%s", Tr("Plugins are loaded from the user plugin folder. Plugins can register CMD console commands. Create a plugin to generate a documented C++ project with a working DLL template."));
    ImGui::TextDisabled("%s", PathUtil::ToUtf8(m_PluginManager.RootPath()).c_str());
    ImGui::Spacing();
    if (Ui::AccentButton(Tr("Add Plugin..."), ImVec2(Ui::S(118.0f), Ui::S(28.0f)))) {
        RequestInstallPlugin();
    }
    ImGui::SameLine();
    if (Ui::GhostButton(Tr("Create Plugin..."), ImVec2(Ui::S(126.0f), Ui::S(28.0f)))) {
        RequestCreatePlugin();
    }
    ImGui::SameLine();
    if (Ui::GhostButton(Tr("Reload All"), ImVec2(Ui::S(96.0f), Ui::S(28.0f)))) {
        m_PluginManager.Refresh();
    }
    ImGui::Separator();
    const float footerHeight = Ui::S(40.0f);
    ImGui::BeginChild("##plugin_list", ImVec2(0.0f, -footerHeight), ImGuiChildFlags_Borders);
    if (m_PluginManager.Plugins().empty()) {
        ImGui::TextDisabled("%s", Tr("No plugins installed."));
    }
    std::string actionId;
    int action = 0; // 1 enable, 2 disable, 3 uninstall, 4 reload
    for (const PluginInfo& plugin : m_PluginManager.Plugins()) {
        ImGui::PushID(plugin.id.c_str());
        PushBoldFont();
        ImGui::TextUnformatted(plugin.name.c_str());
        PopBoldFont();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", plugin.version.c_str());
        ImGui::SameLine();
        if (plugin.loaded) {
            Ui::Badge(Tr("LOADED"), ImVec4(0.31f, 0.84f, 0.78f, 1.0f));
        } else if (!plugin.enabled) {
            Ui::Badge(Tr("DISABLED"), ImVec4(0.65f, 0.62f, 0.55f, 1.0f));
        } else {
            Ui::Badge(Tr("ERROR"), ImVec4(0.98f, 0.40f, 0.34f, 1.0f));
        }
        ImGui::TextDisabled("%s  |  %s", plugin.id.c_str(), plugin.folder.generic_u8string().c_str());
        if (!plugin.author.empty()) ImGui::TextDisabled("%s", plugin.author.c_str());
        if (!plugin.description.empty()) ImGui::TextWrapped("%s", plugin.description.c_str());
        if (!plugin.error.empty()) {
            ImGui::TextColored(ImVec4(0.98f, 0.45f, 0.39f, 1.0f), "%s", plugin.error.c_str());
        }
        if (!plugin.commands.empty()) {
            std::string commands;
            for (const std::string& command : plugin.commands) {
                if (!commands.empty()) commands += ", ";
                commands += command;
            }
            ImGui::TextDisabled("%s%s", Tr("Commands: "), commands.c_str());
        }
        bool enabled = plugin.enabled;
        if (ImGui::Checkbox(Tr("Enabled"), &enabled)) {
            actionId = plugin.id;
            action = enabled ? 1 : 2;
        }
        ImGui::SameLine();
        if (ImGui::Button(Tr("Reload"), ImVec2(Ui::S(78.0f), 0.0f))) {
            actionId = plugin.id;
            action = 4;
        }
        ImGui::SameLine();
        if (ImGui::Button(Tr("Uninstall"), ImVec2(Ui::S(88.0f), 0.0f))) {
            actionId = plugin.id;
            action = 3;
        }
        ImGui::Separator();
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (!actionId.empty()) {
        std::string pluginError;
        bool ok = true;
        if (action == 1) ok = m_PluginManager.SetEnabled(actionId, true, &pluginError);
        else if (action == 2) ok = m_PluginManager.SetEnabled(actionId, false, &pluginError);
        else if (action == 3) ok = m_PluginManager.Uninstall(actionId, &pluginError);
        else if (action == 4) ok = m_PluginManager.Reload(actionId, &pluginError);
        if (!ok) SetStatus(LocalizedErrorText(pluginError), true);
    }
    const float width = Ui::S(100.0f);
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - width);
    if (ImGui::Button(Tr("Close"), ImVec2(width, Ui::S(28.0f)))) {
        m_OpenPluginManager = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void Application::DrawCreatePluginModal() {
    ImGui::SetNextWindowSize(ImVec2(Ui::S(620.0f), 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(Tr("Create Plugin"), &m_OpenCreatePlugin,
                                ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }
    ImGui::TextWrapped("%s", Tr("Generate a plugin folder with plugin.json, C++ source, CMakeLists, README and API documentation."));
    ImGui::Spacing();
    ImGui::TextUnformatted(Tr("Plugin name"));
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##plugin_name", &m_PluginCreateName);
    ImGui::TextUnformatted(Tr("Plugin id"));
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##plugin_id", "com.example.myplugin", &m_PluginCreateId);
    ImGui::TextUnformatted(Tr("Author"));
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##plugin_author", &m_PluginCreateAuthor);
    ImGui::TextUnformatted(Tr("Description"));
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##plugin_description", &m_PluginCreateDescription);
    ImGui::TextUnformatted(Tr("Location"));
    ImGui::SetNextItemWidth(-Ui::S(96.0f));
    ImGui::InputText("##plugin_location", &m_PluginCreateLocation);
    ImGui::SameLine();
    bool useCustomPluginBrowser = false;
    if (ImGui::Button(Tr("Browse..."), ImVec2(Ui::S(88.0f), 0.0f))) {
#if defined(_WIN32)
        fs::path start = PathUtil::FromUtf8(m_PluginCreateLocation);
        std::error_code ec;
        if (!fs::is_directory(start, ec)) {
            start = PathUtil::DefaultProjectsDirectory();
        }
        fs::path selected;
        if (PickFolderDialog(start, &selected,
                             m_Window ? glfwGetWin32Window(m_Window) : nullptr,
                             Utf8ToWide(Tr("Select Plugin Location"))) ==
            NativeFolderResult::Selected) {
            m_PluginCreateLocation = PathUtil::ToUtf8(selected);
            m_PluginCreateError.clear();
        }
#else
        useCustomPluginBrowser = true;
#endif
    }
    if (useCustomPluginBrowser) {
        m_FileAction = FileAction::SelectPluginCreateLocation;
        m_ReturnToPluginCreateAfterBrowse = true;
        m_FileBrowser.Open(Tr("Select Plugin Location"), FileBrowser::Mode::SelectFolder,
                           PathUtil::FromUtf8(m_PluginCreateLocation), "", {});
    }
    ImGui::TextDisabled("%s", Tr("A subfolder named after the plugin is created here."));
    if (!m_PluginCreateError.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.45f, 0.39f, 1.0f));
        ImGui::TextWrapped("%s", LocalizedErrorText(m_PluginCreateError).c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
    ImGui::Separator();
    const float width = Ui::S(108.0f);
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - width * 2.0f -
                         ImGui::GetStyle().ItemSpacing.x);
    bool cancel = ImGui::Button(Tr("Cancel"), ImVec2(width, Ui::S(30.0f)));
    ImGui::SameLine();
    bool create = Ui::AccentButton(Tr("Create"), ImVec2(width, Ui::S(30.0f)));
    if (cancel) {
        m_OpenCreatePlugin = false;
        m_PluginCreateError.clear();
        ImGui::CloseCurrentPopup();
    } else if (create) {
        std::string error;
        CreatePluginTemplate(m_PluginCreateLocation, m_PluginCreateName, m_PluginCreateId,
                             m_PluginCreateAuthor, m_PluginCreateDescription, &error);
        if (error.empty()) {
            m_PluginCreateError.clear();
            m_OpenCreatePlugin = false;
            SetStatus(FormatLocalized("Plugin project created at %s.",
                                      (fs::path(PathUtil::FromUtf8(m_PluginCreateLocation)) /
                                       PathUtil::FromUtf8(m_PluginCreateName))
                                          .u8string()
                                          .c_str()));
            ImGui::CloseCurrentPopup();
        } else {
            m_PluginCreateError = error;
        }
    }
    ImGui::EndPopup();
}

void Application::StartMcpServerIfEnabled() {
    if (!m_Settings.mcpEnabled) {
        m_McpStatus = Tr("MCP server is disabled.");
        return;
    }
    std::string error;
    if (!StartMcpServer(&error)) {
        m_McpStatus = error;
        m_Settings.mcpEnabled = false;
        SaveUiSettings();
        LogMessage(LogLevel::Warning, "Could not start MCP server: " + error);
    }
}

bool Application::StartMcpServer(std::string* error) {
    StopMcpServer();
    m_McpServer = std::make_unique<McpServer>();
    std::string startError;
    const bool started = m_McpServer->Start(
        static_cast<std::uint16_t>(std::clamp(m_Settings.mcpPort, 1024, 65535)),
        [this](const std::string& tool, const nlohmann::json& arguments) {
            return HandleMcpToolCall(tool, arguments);
        },
        [this](int level, const std::string& message) {
            LogMessage(level >= 2 ? LogLevel::Warning : LogLevel::Info, message);
        },
        &startError);
    if (!started) {
        m_McpServer.reset();
        m_McpStatus = startError;
        if (error) *error = startError;
        return false;
    }
    m_McpStatus = FormatLocalized("MCP server listening on %s",
                                  m_McpServer->Endpoint().c_str());
    if (error) error->clear();
    return true;
}

void Application::StopMcpServer() {
    if (m_McpServer) {
        m_McpServer->Stop();
        m_McpServer.reset();
        m_McpStatus = Tr("MCP server is disabled.");
    }
}

void Application::ProcessMcpPending() {
    if (m_McpServer) m_McpServer->ProcessPending();
}

void Application::SaveMcpSettings() {
    m_Settings.mcpEnabled = m_McpServer && m_McpServer->IsRunning();
    m_Settings.mcpPort = std::clamp(m_Settings.mcpPort, 1024, 65535);
    SaveUiSettings();
}

nlohmann::json Application::HandleMcpToolCall(const std::string& tool,
                                               const nlohmann::json& args) {
    using nlohmann::json;
    const auto ok = [](json data) { return json{{"ok", true}, {"data", std::move(data)}}; };
    const auto fail = [](const std::string& message) {
        return json{{"ok", false}, {"error", message}};
    };
    const auto requireProject = [&]() -> std::string {
        return m_Project.IsOpen() ? std::string() : std::string("No project is open.");
    };
    const auto resolveProjectPath = [&](std::string path) -> std::pair<fs::path, std::string> {
        path = Trim(path);
        std::replace(path.begin(), path.end(), '\\', '/');
        if (path.empty()) return {{}, "A project-relative path is required."};
        fs::path input = PathUtil::FromUtf8(path);
        fs::path absolute = input.is_absolute() ? input : m_Project.RootPath() / input;
        absolute = absolute.lexically_normal();
        if (!m_Project.Contains(absolute)) {
            return {{}, "The path is outside the open project: " + path};
        }
        return {absolute, {}};
    };
    const auto relativePath = [&](const fs::path& path) {
        return m_Project.RelativePathOf(path);
    };
    const auto refreshProject = [&]() {
        m_ProjectEntriesDirty = true;
        RefreshProjectIntegrity();
        RefreshProjectEntries();
    };
    const auto maybeUpdateActiveDocument = [&](const fs::path& path, MaterialGraph& graph) {
        if (NormalizedPathKey(m_DocumentPath) != NormalizedPathKey(path)) return true;
        if (m_Graph.IsDirty()) {
            return false;
        }
        m_Graph = graph;
        m_Graph.MarkSaved();
        m_DocumentPath = path;
        m_ForceShaderRefresh = true;
        m_LastContentRevision = 0;
        if (m_GraphView) m_GraphView->OnGraphReplaced();
        return true;
    };

    try {
        if (tool == "mf_status") {
            const std::string projectError = requireProject();
            const GraphStats stats = m_Graph.ComputeStats();
            json data = {
                {"version", MF_VERSION_STRING},
                {"projectOpen", m_Project.IsOpen()},
                {"projectName", m_Project.IsOpen() ? m_Project.Name() : std::string()},
                {"projectRoot", m_Project.IsOpen() ? PathUtil::ToUtf8(m_Project.RootPath())
                                                   : std::string()},
                {"activeDocument", m_DocumentPath.empty()
                                       ? std::string()
                                       : PathUtil::ToUtf8(m_DocumentPath)},
                {"activeDocumentRelative",
                 m_Project.IsOpen() && !m_DocumentPath.empty()
                     ? m_Project.RelativePathOf(m_DocumentPath)
                     : std::string()},
                {"documentKind", m_DocumentKind == DocumentKind::Function
                                     ? "function"
                                     : (m_DocumentKind == DocumentKind::Instance ? "instance"
                                                                                 : "material")},
                {"documentDirty", m_Graph.IsDirty()},
                {"nodes", stats.nodeCount},
                {"links", stats.linkCount},
                {"parameters", stats.parameterCount},
                {"textures", stats.textureCount},
                {"previewBackend", m_PreviewRenderer.BackendName()},
                {"mcpEndpoint", m_McpServer ? m_McpServer->Endpoint() : std::string()},
            };
            if (!projectError.empty()) data["projectError"] = projectError;
            return ok(std::move(data));
        }

        if (tool == "mf_list_materials") {
            const std::string projectError = requireProject();
            if (!projectError.empty()) return fail(projectError);
            std::string folder;
            if (args.contains("folder") && args["folder"].is_string()) {
                folder = args["folder"].get<std::string>();
            }
            std::replace(folder.begin(), folder.end(), '\\', '/');
            while (!folder.empty() && folder.front() == '/') folder.erase(folder.begin());
            while (!folder.empty() && folder.back() == '/') folder.pop_back();
            const fs::path base = folder.empty() ? m_Project.RootPath()
                                                 : m_Project.ResolveRelative(folder);
            std::error_code ec;
            if (base.empty() || !fs::is_directory(base, ec)) {
                return fail("Project folder does not exist: " + folder);
            }
            json materials = json::array();
            for (fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied,
                                                     ec),
                 end;
                 !ec && it != end; it.increment(ec)) {
                if (it->is_directory(ec)) {
                    if (it->path().filename() == ".materialforge") it.disable_recursion_pending();
                    continue;
                }
                if (!it->is_regular_file(ec)) continue;
                const std::string extension = ToLower(PathUtil::ToUtf8(it->path().extension()));
                if (extension != ".json" && extension != ".mffunction") continue;
                const std::string relative = m_Project.RelativePathOf(it->path());
                if (relative.empty()) continue;
                materials.push_back({
                    {"path", relative},
                    {"kind", MaterialProject::KindForPath(relative)},
                    {"size", static_cast<std::uintmax_t>(it->file_size(ec))},
                });
            }
            return ok({{"materials", materials}, {"count", materials.size()}});
        }

        if (tool == "mf_create_material") {
            const std::string projectError = requireProject();
            if (!projectError.empty()) return fail(projectError);
            if (!args.contains("name") || !args["name"].is_string()) {
                return fail("'name' is required.");
            }
            std::string name = Trim(args["name"].get<std::string>());
            if (name.size() > 5 && ToLower(name.substr(name.size() - 5)) == ".json") {
                name.resize(name.size() - 5);
            }
            std::string reason;
            if (!MaterialProject::IsValidFolderName(name, &reason)) {
                return fail(reason);
            }
            std::string folder;
            if (args.contains("folder") && args["folder"].is_string()) {
                folder = Trim(args["folder"].get<std::string>());
            }
            std::replace(folder.begin(), folder.end(), '\\', '/');
            while (!folder.empty() && folder.front() == '/') folder.erase(folder.begin());
            while (!folder.empty() && folder.back() == '/') folder.pop_back();
            if (!folder.empty() && !MaterialProject::IsSafeRelativePath(folder)) {
                return fail("'folder' must be a safe project-relative path.");
            }
            const fs::path directory = folder.empty() ? m_Project.RootPath()
                                                      : m_Project.RootPath() / PathUtil::FromUtf8(folder);
            std::error_code ec;
            fs::create_directories(directory, ec);
            if (ec) return fail("Could not create the destination folder: " + ec.message());
            const fs::path path = (directory / PathUtil::FromUtf8(name + ".json")).lexically_normal();
            if (!m_Project.Contains(path)) return fail("The destination is outside the project.");
            if (fs::exists(path, ec)) return fail("A material with that name already exists.");
            MaterialGraph graph;
            std::string buildError;
            if (args.contains("graph") && args["graph"].is_object()) {
                if (!GraphSerializer::FromJson(args["graph"], graph, &buildError)) {
                    return fail("Invalid graph document: " + buildError);
                }
            } else {
                const std::string templateName =
                    args.contains("template") && args["template"].is_string()
                        ? args["template"].get<std::string>()
                        : "default";
                if (templateName == "empty") {
                    if (!graph.AddNodeByType("MaterialOutput", Vec2(500.0f, 0.0f), &buildError)) {
                        return fail("Could not create MaterialOutput: " + buildError);
                    }
                } else if (!BuildStarterMaterialGraph(graph, name, &buildError)) {
                    return fail("Could not build the starter material: " + buildError);
                }
            }
            graph.Settings().name = name;
            if (!GraphSerializer::Save(graph, PathUtil::ToUtf8(path), &buildError)) {
                return fail("Could not save the material: " + buildError);
            }
            const std::string relative = m_Project.RelativePathOf(path);
            m_Project.AddResource(relative, "material", true);
            m_Project.Save(&buildError);
            refreshProject();
            m_ProjectSelectedEntry = relative;
            m_ProjectSearch.clear();
            m_ScrollProjectSelection = true;
            RefreshProjectEntries();
            return ok({{"path", relative},
                       {"nodes", graph.Nodes().size()},
                       {"links", graph.Links().size()}});
        }

        if (tool == "mf_get_graph") {
            const std::string projectError = requireProject();
            if (!projectError.empty()) return fail(projectError);
            if (!args.contains("path") || !args["path"].is_string()) {
                return fail("'path' is required.");
            }
            const auto [path, pathError] = resolveProjectPath(args["path"].get<std::string>());
            if (!pathError.empty()) return fail(pathError);
            MaterialGraph graph;
            std::string loadError;
            if (!GraphSerializer::Load(PathUtil::ToUtf8(path), graph, &loadError)) {
                return fail("Could not load the material: " + loadError);
            }
            return ok({{"path", relativePath(path)}, {"graph", GraphSerializer::ToJson(graph)}});
        }

        if (tool == "mf_set_graph") {
            const std::string projectError = requireProject();
            if (!projectError.empty()) return fail(projectError);
            if (!args.contains("path") || !args["path"].is_string() ||
                !args.contains("graph") || !args["graph"].is_object()) {
                return fail("'path' and 'graph' are required.");
            }
            const auto [path, pathError] = resolveProjectPath(args["path"].get<std::string>());
            if (!pathError.empty()) return fail(pathError);
            MaterialGraph graph;
            std::string buildError;
            if (!GraphSerializer::FromJson(args["graph"], graph, &buildError)) {
                return fail("Invalid graph document: " + buildError);
            }
            if (!GraphSerializer::Save(graph, PathUtil::ToUtf8(path), &buildError)) {
                return fail("Could not save the material: " + buildError);
            }
            const std::string relative = relativePath(path);
            m_Project.AddResource(relative, MaterialProject::KindForPath(relative), true);
            m_Project.Save(&buildError);
            if (!maybeUpdateActiveDocument(path, graph)) {
                return fail("The material is open with unsaved changes; save it first.");
            }
            refreshProject();
            return ok({{"path", relative},
                       {"nodes", graph.Nodes().size()},
                       {"links", graph.Links().size()}});
        }

        if (tool == "mf_add_node") {
            const std::string projectError = requireProject();
            if (!projectError.empty()) return fail(projectError);
            if (!args.contains("path") || !args.contains("type")) {
                return fail("'path' and 'type' are required.");
            }
            const auto [path, pathError] = resolveProjectPath(args["path"].get<std::string>());
            if (!pathError.empty()) return fail(pathError);
            MaterialGraph graph;
            std::string fileError;
            if (!GraphSerializer::Load(PathUtil::ToUtf8(path), graph, &fileError)) {
                return fail("Could not load the material: " + fileError);
            }
            const std::string type = args["type"].get<std::string>();
            const NodeDefinition* definition = NodeRegistry::Get().Find(type);
            if (!definition) return fail("Unknown node type: " + type);
            const float x = args.value("x", 0.0f);
            const float y = args.value("y", 0.0f);
            Node* node = graph.AddNodeByType(type, Vec2(x, y), &fileError);
            if (!node) return fail("Could not add the node: " + fileError);
            if (args.contains("properties") && args["properties"].is_object()) {
                for (auto it = args["properties"].begin(); it != args["properties"].end(); ++it) {
                    auto property = node->properties.find(it.key());
                    if (property == node->properties.end()) continue;
                    const json& value = it.value();
                    switch (property->second.kind) {
                    case PropertyKind::Float:
                        if (value.is_number()) property->second = PropertyValue::Float(value.get<float>());
                        break;
                    case PropertyKind::Bool:
                        if (value.is_boolean()) property->second = PropertyValue::BoolValue(value.get<bool>());
                        break;
                    case PropertyKind::Vector2:
                        if (value.is_array() && value.size() >= 2) {
                            property->second = PropertyValue::Vec2Value(value[0].get<float>(),
                                                                       value[1].get<float>());
                        }
                        break;
                    case PropertyKind::Vector3:
                        if (value.is_array() && value.size() >= 3) {
                            property->second = PropertyValue::Vec3Value(
                                value[0].get<float>(), value[1].get<float>(), value[2].get<float>());
                        }
                        break;
                    case PropertyKind::Vector4:
                    case PropertyKind::Color4:
                        if (value.is_array() && value.size() >= 4) {
                            const Vec4 vec(value[0].get<float>(), value[1].get<float>(),
                                           value[2].get<float>(), value[3].get<float>());
                            property->second = property->second.kind == PropertyKind::Color4
                                                   ? PropertyValue::Color4Value(vec.x, vec.y, vec.z, vec.w)
                                                   : PropertyValue::Vec4Value(vec.x, vec.y, vec.z, vec.w);
                        }
                        break;
                    case PropertyKind::Color3:
                        if (value.is_array() && value.size() >= 3) {
                            property->second = PropertyValue::Color3Value(
                                value[0].get<float>(), value[1].get<float>(), value[2].get<float>());
                        }
                        break;
                    case PropertyKind::String:
                    case PropertyKind::AssetPath:
                        if (value.is_string()) {
                            property->second = property->second.kind == PropertyKind::AssetPath
                                                   ? PropertyValue::AssetPathValue(value.get<std::string>())
                                                   : PropertyValue::StringValue(value.get<std::string>());
                        }
                        break;
                    }
                }
            }
            if (!GraphSerializer::Save(graph, PathUtil::ToUtf8(path), &fileError)) {
                return fail("Could not save the material: " + fileError);
            }
            if (!maybeUpdateActiveDocument(path, graph)) {
                return fail("The material is open with unsaved changes; save it first.");
            }
            refreshProject();
            json outputs = json::array();
            for (const Pin& pin : node->outputs) outputs.push_back(pin.name);
            return ok({{"path", relativePath(path)},
                       {"nodeId", node->id},
                       {"type", node->type},
                       {"outputs", outputs}});
        }

        if (tool == "mf_connect") {
            const std::string projectError = requireProject();
            if (!projectError.empty()) return fail(projectError);
            if (!args.contains("path")) return fail("'path' is required.");
            const auto [path, pathError] = resolveProjectPath(args["path"].get<std::string>());
            if (!pathError.empty()) return fail(pathError);
            MaterialGraph graph;
            std::string fileError;
            if (!GraphSerializer::Load(PathUtil::ToUtf8(path), graph, &fileError)) {
                return fail("Could not load the material: " + fileError);
            }
            Node* from = graph.FindNode(args.value("fromNode", NodeId{0}));
            Node* to = graph.FindNode(args.value("toNode", NodeId{0}));
            if (!from || !to) return fail("Source or target node was not found.");
            const std::string fromPinName = args.value("fromPin", std::string());
            const std::string toPinName = args.value("toPin", std::string());
            Pin* fromPin = from->FindOutput(fromPinName);
            Pin* toPin = to->FindInput(toPinName);
            if (!fromPin || !toPin) return fail("Source or target pin was not found.");
            Link* link = graph.Connect(fromPin->id, toPin->id, &fileError);
            if (!link) return fail("Could not connect the pins: " + fileError);
            if (!GraphSerializer::Save(graph, PathUtil::ToUtf8(path), &fileError)) {
                return fail("Could not save the material: " + fileError);
            }
            if (!maybeUpdateActiveDocument(path, graph)) {
                return fail("The material is open with unsaved changes; save it first.");
            }
            refreshProject();
            return ok({{"path", relativePath(path)}, {"linkId", link->id}});
        }

        if (tool == "mf_import_ue5_text") {
            const std::string projectError = requireProject();
            if (!projectError.empty()) return fail(projectError);
            if (!args.contains("path") || !args.contains("text")) {
                return fail("'path' and 'text' are required.");
            }
            const auto [path, pathError] = resolveProjectPath(args["path"].get<std::string>());
            if (!pathError.empty()) return fail(pathError);
            MaterialGraph graph;
            std::string fileError;
            if (!GraphSerializer::Load(PathUtil::ToUtf8(path), graph, &fileError)) {
                return fail("Could not load the material: " + fileError);
            }
            Ue5ClipboardImportResult import;
            if (!ImportUe5MaterialClipboard(args["text"].get<std::string>(), graph,
                                            Vec2(0.0f, 0.0f), &import)) {
                return fail(import.error);
            }
            if (!GraphSerializer::Save(graph, PathUtil::ToUtf8(path), &fileError)) {
                return fail("Could not save the material: " + fileError);
            }
            if (!maybeUpdateActiveDocument(path, graph)) {
                return fail("The material is open with unsaved changes; save it first.");
            }
            refreshProject();
            return ok({{"path", relativePath(path)},
                       {"nodes", import.nodeCount},
                       {"links", import.linkCount},
                       {"skipped", import.skippedNodeCount},
                       {"droppedOutputLinks", import.droppedMaterialOutputLinks}});
        }
    } catch (const std::exception& exception) {
        return fail(std::string("MCP tool failed: ") + exception.what());
    }
    return fail("Unknown MCP tool: " + tool);
}

void Application::DrawProjectDialogs() {
    if (m_NewProjectModalPending) {
        ImGui::OpenPopup(Tr("New Project"));
        m_NewProjectModalPending = false;
    }
    if (m_OpenNewProjectModal) {
        DrawNewProjectModal();
    }

    if (m_NewFolderModalPending) {
        ImGui::OpenPopup(Tr("New Folder"));
        m_NewFolderModalPending = false;
    }
    if (m_OpenNewFolderModal) {
        DrawNewFolderModal();
    }

    if (m_NewMaterialModalPending) {
        ImGui::OpenPopup(Tr("New Material"));
        m_NewMaterialModalPending = false;
    }
    if (m_OpenNewMaterialModal) {
        DrawNewMaterialModal();
    }

    if (m_NewFunctionModalPending) {
        ImGui::OpenPopup(Tr("New Blueprint Function"));
        m_NewFunctionModalPending = false;
    }
    if (m_OpenNewFunctionModal) {
        DrawNewFunctionModal();
    }

    if (m_TexturePackagesPending) {
        ImGui::OpenPopup(Tr("Add Texture Package"));
        m_TexturePackagesPending = false;
    }
    if (m_OpenTexturePackages) {
        DrawTexturePackagesModal();
    }

    if (m_TexturePickerPending) {
        ImGui::OpenPopup(Tr("Select Texture"));
        m_TexturePickerPending = false;
    }
    if (m_OpenTexturePicker) {
        DrawTexturePickerModal();
    }

    if (m_PluginManagerPending) {
        ImGui::OpenPopup(Tr("Plugin Manager"));
        m_PluginManagerPending = false;
    }
    if (m_OpenPluginManager) {
        DrawPluginManager();
    }
    if (m_PluginCreateModalPending) {
        ImGui::OpenPopup(Tr("Create Plugin"));
        m_PluginCreateModalPending = false;
    }
    if (m_OpenCreatePlugin) {
        DrawCreatePluginModal();
    }

    if (m_RenameFolderModalPending) {
        ImGui::OpenPopup(Tr("Rename Folder"));
        m_RenameFolderModalPending = false;
    }
    if (m_OpenRenameFolderModal) {
        DrawRenameFolderModal();
    }

    if (m_RenameFileModalPending) {
        ImGui::OpenPopup(Tr("Rename File"));
        m_RenameFileModalPending = false;
    }
    if (m_OpenRenameFileModal) {
        DrawRenameFileModal();
    }

    if (m_DeleteEntryModalPending) {
        ImGui::OpenPopup(Tr("Delete Entry"));
        m_OpenDeleteEntryModal = true;
        m_DeleteEntryModalPending = false;
    }
    if (m_OpenDeleteEntryModal) {
        DrawDeleteEntryModal();
    }
    if (m_DeleteMultiplePending) {
        ImGui::OpenPopup(Tr("Delete Selected Files"));
        m_OpenDeleteMultipleModal = true;
        m_DeleteMultiplePending = false;
    }
    if (m_OpenDeleteMultipleModal) {
        DrawDeleteMultipleModal();
    }

    if (m_PreviewSceneModalPending) {
        ImGui::OpenPopup(Tr("Preview Scene"));
        m_OpenPreviewSceneModal = true;
        m_PreviewSceneModalPending = false;
    }
    if (m_OpenPreviewSceneModal) {
        DrawPreviewSceneModal();
    }

    if (m_UeBridgePending) {
        ImGui::OpenPopup(Tr("UE5 Push / Pull"));
        m_OpenUeBridgeModal = true;
        m_UeBridgePending = false;
    }
    if (m_OpenUeBridgeModal) {
        DrawUeBridgeModal();
    }
}

void Application::DrawNewProjectModal() {
    ImGui::SetNextWindowSize(ImVec2(Ui::S(580.0f), 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(Tr("New Project"), &m_OpenNewProjectModal,
                                ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        m_OpenNewProjectModal = false;
        m_NewProjectError.clear();
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    ImGui::TextWrapped("%s", Tr("Create a project folder with its own material, assets and a portable .mfproject load file."));
    ImGui::Spacing();
    ImGui::TextUnformatted(Tr("Project name"));
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::InputTextWithHint("##new_project_name", "My Material Project", &m_NewProjectName,
                             ImGuiInputTextFlags_AutoSelectAll);

    ImGui::TextUnformatted(Tr("Location"));
    ImGui::TextDisabled("%s", Tr("Parent folder; the project folder is created inside it."));
    ImGui::SetNextItemWidth(-96.0f);
    ImGui::InputTextWithHint("##new_project_location", "C:/Users/you/Documents/MaterialForge Projects",
                             &m_NewProjectLocation);
    ImGui::SameLine();
    const bool browseClicked = ImGui::Button(Tr("Browse..."), ImVec2(Ui::S(88.0f), 0.0f));
    bool useCustomFolderBrowser = false;
#if defined(_WIN32)
    if (browseClicked) {
        fs::path start = PathUtil::FromUtf8(m_NewProjectLocation);
        std::error_code ec;
        if (!fs::is_directory(start, ec)) {
            start = PathUtil::DefaultProjectsDirectory();
        }
        fs::path selected;
        if (PickFolderDialog(start, &selected,
                             m_Window ? glfwGetWin32Window(m_Window) : nullptr,
                             Utf8ToWide(Tr("Select Project Location"))) ==
            NativeFolderResult::Selected) {
            m_NewProjectLocation = PathUtil::ToUtf8(selected);
            m_NewProjectError.clear();
        }
    }
#else
    useCustomFolderBrowser = browseClicked;
#endif

    const std::string safeName = MaterialProject::SanitizeProjectName(m_NewProjectName);
    fs::path preview = PathUtil::FromUtf8(m_NewProjectLocation) / PathUtil::FromUtf8(safeName);
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.64f, 0.78f, 0.82f, 1.0f));
    ImGui::TextWrapped(Tr("Folder: %s"), PathUtil::ToUtf8(preview).c_str());
    if (!safeName.empty()) {
        ImGui::TextDisabled(Tr("Project file: %s.mfproject"), safeName.c_str());
    }
    ImGui::PopStyleColor();
    ImGui::TextDisabled("%s", Tr("The initial material will be saved into materials/."));
    if (!m_NewProjectError.empty()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.45f, 0.39f, 1.0f));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(LocalizedErrorText(m_NewProjectError).c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::Separator();
    const float width = Ui::S(116.0f);
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - width * 2.0f -
                         ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::Button(Tr("Cancel"), ImVec2(width, Ui::S(32.0f)))) {
        m_OpenNewProjectModal = false;
        m_NewProjectError.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (Ui::AccentButton(Tr("Create Project"), ImVec2(width, Ui::S(32.0f)))) {
        std::string error;
        if (CreateProject(m_NewProjectName, m_NewProjectLocation, &error)) {
            m_OpenNewProjectModal = false;
            m_NewProjectError.clear();
            ImGui::CloseCurrentPopup();
        } else {
            m_NewProjectError = error;
        }
    }

    if (useCustomFolderBrowser) {
        m_OpenNewProjectModal = false;
        m_NewProjectModalPending = false;
        m_NewProjectError.clear();
        m_ReturnToNewProjectAfterBrowse = true;
        m_FileAction = FileAction::SelectProjectLocation;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();

    if (useCustomFolderBrowser) {
        fs::path start = PathUtil::FromUtf8(m_NewProjectLocation);
        std::error_code ec;
        if (!fs::is_directory(start, ec)) {
            start = PathUtil::DefaultProjectsDirectory();
        }
        m_FileBrowser.Open(Tr("Select Project Location"), FileBrowser::Mode::SelectFolder, start, "",
                           {});
    }
}

void Application::DrawNewFolderModal() {
    ImGui::SetNextWindowSize(ImVec2(Ui::S(440.0f), 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(Tr("New Folder"), &m_OpenNewFolderModal,
                                ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        m_OpenNewFolderModal = false;
        m_NewFolderError.clear();
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    if (!m_Project.IsOpen()) {
        ImGui::TextDisabled("%s", Tr("No project is open."));
        if (ImGui::Button(Tr("Close"))) {
            m_OpenNewFolderModal = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        return;
    }

    ImGui::Text(Tr("Create a folder inside /%s"), m_ProjectCurrentDirectory.c_str());
    ImGui::Spacing();
    ImGui::TextUnformatted(Tr("Folder name"));
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(-Ui::S(70.0f));
    ImGui::InputTextWithHint("##new_folder_name", "textures", &m_NewFolderName);
    ImGui::SameLine();
    if (ImGui::Button(Tr("Paste"), ImVec2(Ui::S(64.0f), 0.0f))) {
        if (const char* clipboard = ImGui::GetClipboardText(); clipboard && clipboard[0]) {
            m_NewFolderName = clipboard;
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", Tr("Paste from clipboard"));
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.52f, 0.66f, 0.69f, 1.0f));
    ImGui::TextWrapped("%s", Tr("Supports Chinese and spaces. Forbidden: \\ / : * ? \" < > |"));
    ImGui::PopStyleColor();
    if (!m_NewFolderError.empty()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.45f, 0.39f, 1.0f));
        ImGui::TextWrapped("%s", LocalizedErrorText(m_NewFolderError).c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
    ImGui::Separator();
    const float width = 100.0f;
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - width * 2.0f -
                         ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::Button(Tr("Cancel"), ImVec2(width, Ui::S(30.0f)))) {
        m_OpenNewFolderModal = false;
        m_NewFolderError.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (Ui::AccentButton(Tr("Create"), ImVec2(width, Ui::S(30.0f)))) {
        std::string error;
        if (m_Project.CreateFolder(m_ProjectCurrentDirectory, m_NewFolderName, &error)) {
            m_OpenNewFolderModal = false;
            m_NewFolderError.clear();
            m_ProjectSearch.clear();
            m_ProjectEntriesDirty = true;
            RefreshProjectEntries();
            SetStatus(Tr("Folder created."));
            ImGui::CloseCurrentPopup();
        } else {
            m_NewFolderError = error;
        }
    }
    ImGui::EndPopup();
}

void Application::DrawNewMaterialModal() {
    ImGui::SetNextWindowSize(ImVec2(Ui::S(480.0f), 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(Tr("New Material"), &m_OpenNewMaterialModal,
                                ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        m_OpenNewMaterialModal = false;
        m_NewMaterialError.clear();
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    if (!m_Project.IsOpen()) {
        ImGui::TextDisabled("%s", Tr("No project is open."));
        if (ImGui::Button(Tr("Close"))) {
            m_OpenNewMaterialModal = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        return;
    }

    ImGui::Text(Tr("Create a material inside /%s"), m_ProjectCurrentDirectory.c_str());
    ImGui::Spacing();
    ImGui::TextUnformatted(Tr("Material name"));
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(-Ui::S(70.0f));
    ImGui::InputTextWithHint("##new_material_name", "M_NewMaterial", &m_NewMaterialName,
                             ImGuiInputTextFlags_AutoSelectAll);
    ImGui::SameLine();
    if (ImGui::Button(Tr("Paste"), ImVec2(Ui::S(64.0f), 0.0f))) {
        if (const char* clipboard = ImGui::GetClipboardText(); clipboard && clipboard[0]) {
            m_NewMaterialName = clipboard;
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", Tr("Paste from clipboard"));
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.52f, 0.66f, 0.69f, 1.0f));
    ImGui::TextWrapped("%s", Tr("Supports Chinese and spaces. Forbidden: \\ / : * ? \" < > |"));
    ImGui::PopStyleColor();

    std::string previewName = Trim(m_NewMaterialName);
    if (previewName.size() > 5 && ToLower(previewName.substr(previewName.size() - 5)) == ".json") {
        previewName.resize(previewName.size() - 5);
    }
    if (!previewName.empty()) {
        ImGui::TextDisabled(Tr("File: %s.json"), previewName.c_str());
    }
    ImGui::TextDisabled("%s", Tr("The new material becomes the project's main material and opens immediately."));
    if (!m_NewMaterialError.empty()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.45f, 0.39f, 1.0f));
        ImGui::TextWrapped("%s", LocalizedErrorText(m_NewMaterialError).c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
    ImGui::Separator();
    const float width = Ui::S(104.0f);
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - width * 2.0f -
                         ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::Button(Tr("Cancel"), ImVec2(width, Ui::S(30.0f)))) {
        m_OpenNewMaterialModal = false;
        m_NewMaterialError.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (Ui::AccentButton(Tr("Create"), ImVec2(width, Ui::S(30.0f)))) {
        std::string error;
        if (CreateProjectMaterial(m_NewMaterialName, &error)) {
            m_OpenNewMaterialModal = false;
            m_NewMaterialError.clear();
            ImGui::CloseCurrentPopup();
        } else {
            m_NewMaterialError = error;
        }
    }
    ImGui::EndPopup();
}

void Application::DrawNewFunctionModal() {
    ImGui::SetNextWindowSize(ImVec2(Ui::S(500.0f), 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(Tr("New Blueprint Function"), &m_OpenNewFunctionModal,
                                ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        m_OpenNewFunctionModal = false;
        m_NewFunctionError.clear();
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    if (!m_Project.IsOpen()) {
        ImGui::TextDisabled("%s", Tr("No project is open."));
        if (ImGui::Button(Tr("Close"))) {
            m_OpenNewFunctionModal = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        return;
    }

    ImGui::TextWrapped("%s", Tr("Create a reusable blueprint function graph inside the current project folder."));
    ImGui::Spacing();
    ImGui::TextUnformatted(Tr("Function name"));
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(-Ui::S(70.0f));
    ImGui::InputTextWithHint("##new_function_name", "MF_NewFunction", &m_NewFunctionName,
                             ImGuiInputTextFlags_AutoSelectAll);
    ImGui::SameLine();
    if (ImGui::Button(Tr("Paste"), ImVec2(Ui::S(64.0f), 0.0f))) {
        if (const char* clipboard = ImGui::GetClipboardText(); clipboard && clipboard[0]) {
            m_NewFunctionName = clipboard;
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", Tr("Paste from clipboard"));
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.52f, 0.66f, 0.69f, 1.0f));
    ImGui::TextWrapped("%s", Tr("Supports Chinese and spaces. Forbidden: \\ / : * ? \" < > |"));
    ImGui::PopStyleColor();

    std::string previewName = Trim(m_NewFunctionName);
    const std::string suffix = ".mffunction";
    if (previewName.size() > suffix.size() &&
        ToLower(previewName.substr(previewName.size() - suffix.size())) == suffix) {
        previewName.resize(previewName.size() - suffix.size());
    }
    if (!previewName.empty()) {
        ImGui::TextDisabled(Tr("File: %s.mffunction"), previewName.c_str());
    }
    ImGui::TextDisabled("%s", Tr("The function graph is saved as a reusable .mffunction asset and opens in a new tab."));
    if (!m_NewFunctionError.empty()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.45f, 0.39f, 1.0f));
        ImGui::TextWrapped("%s", LocalizedErrorText(m_NewFunctionError).c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::Separator();
    const float width = Ui::S(116.0f);
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - width * 2.0f -
                         ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::Button(Tr("Cancel"), ImVec2(width, Ui::S(32.0f)))) {
        m_OpenNewFunctionModal = false;
        m_NewFunctionError.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (Ui::AccentButton(Tr("Create"), ImVec2(width, Ui::S(32.0f)))) {
        std::string error;
        if (CreateBlueprintFunction(m_NewFunctionName, &error)) {
            m_OpenNewFunctionModal = false;
            m_NewFunctionError.clear();
            ImGui::CloseCurrentPopup();
        } else {
            m_NewFunctionError = error;
        }
    }
    ImGui::EndPopup();
}

void Application::DrawAboutPopup() {
    const char* title = Tr("About MaterialForge");
    if (m_OpenAbout) {
        ImGui::OpenPopup(title);
        m_OpenAbout = false;
    }
    ImGui::SetNextWindowSize(ImVec2(Ui::S(540.0f), 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }
        PushBoldFont();
#if defined(MF_VERSION_STRING)
        ImGui::TextUnformatted("MaterialForge " MF_VERSION_STRING);
#else
        ImGui::TextUnformatted("MaterialForge " MF_VERSION_STRING);
#endif
        PopBoldFont();
        ImGui::TextDisabled("%s", Tr("A lightweight UE5-inspired material node editor"));
        ImGui::Separator();
        ImGui::TextWrapped("%s", Tr("Build a material DAG from Unreal Engine-aligned expressions, preview it on a sphere or plane with generated GLSL or HLSL, and export a JSON document that the UE5 Python bridge can reconstruct as a material graph."));
        ImGui::TextWrapped("%s", Tr("Projects group material files, textures, models and reference assets under one .mfproject load file with recent-project history and broken-reference recovery."));
        ImGui::Spacing();
        Ui::SectionLabel(Tr("LIBRARIES"));
        ImGui::BulletText("Dear ImGui + imgui-node-editor");
        ImGui::BulletText("GLFW + glad (OpenGL 4.6 core)");
        ImGui::BulletText("Vulkan headers + glslang / D3DCompiler");
        ImGui::BulletText("nlohmann/json + stb_image");
        ImGui::Spacing();
        Ui::SectionLabel(Tr("SCHEMA"));
        ImGui::TextDisabled("%s", Tr("Material: materialforge.ue5.material v1"));
        ImGui::TextDisabled("%s", Tr("Project: materialforge.project v1"));
        ImGui::Spacing();
        Ui::SectionLabel(Tr("SHORTCUTS"));
        ImGui::BulletText("%s", Tr("Ctrl+N / Ctrl+O / Ctrl+S  -  New / Open / Save material"));
        ImGui::BulletText("%s", Tr("Ctrl+E  -  Export UE5 JSON"));
        ImGui::BulletText("%s", Tr("Ctrl+,  -  Preferences (rendering / interface / console)"));
        ImGui::BulletText("%s", Tr("Ctrl+`  -  CMD console    Ctrl+L  -  collapse the node library"));
        ImGui::BulletText("%s", Tr("Home / F  -  Frame all / frame selection"));
        ImGui::BulletText("%s", Tr("R  -  Reset preview camera"));
        ImGui::Spacing();
        if (Ui::AccentButton(Tr("Close"), ImVec2(100.0f, 28.0f))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void Application::RequestNewProject() {
    m_NewProjectName = Tr("New Project");
    const fs::path defaultLocation = PathUtil::DefaultProjectsDirectory();
    m_NewProjectLocation = PathUtil::ToUtf8(defaultLocation);
    m_NewProjectError.clear();
    m_ReturnToNewProjectAfterBrowse = false;

#if defined(_WIN32)
    fs::path selected;
    const NativeFolderResult result = PickFolderDialog(
        defaultLocation, &selected,
        m_Window ? glfwGetWin32Window(m_Window) : nullptr,
        Utf8ToWide(Tr("Select New Project Location")));
    if (result == NativeFolderResult::Cancelled) {
        return;
    }
    if (result == NativeFolderResult::Selected) {
        m_NewProjectLocation = PathUtil::ToUtf8(selected);
    }
#endif

    m_OpenNewProjectModal = true;
    m_NewProjectModalPending = true;
}

void Application::RequestOpenProject() {
    fs::path start = PathUtil::DefaultProjectsDirectory();
    if (m_Project.IsOpen()) {
        start = m_Project.ProjectFilePath().parent_path();
    } else if (!m_RecentProjects.Entries().empty()) {
        const fs::path recentParent = m_RecentProjects.Entries().front().projectFile.parent_path();
        if (PathUtil::Exists(recentParent)) start = recentParent;
    }

#if defined(_WIN32)
    fs::path folder;
    const NativeFolderResult result = PickFolderDialog(
        start, &folder, m_Window ? glfwGetWin32Window(m_Window) : nullptr,
        Utf8ToWide(Tr("Open Project Folder")));
    if (result == NativeFolderResult::Cancelled) {
        return;
    }
    if (result == NativeFolderResult::Failed) {
        SetStatus(Tr("Could not open the Windows folder picker."), true);
        return;
    }
    const std::vector<fs::path> projects = FindProjectFiles(folder);
    if (projects.empty()) {
        SetStatus(FormatLocalized("No .mfproject file was found in %s.",
                                  PathUtil::ToUtf8(folder).c_str()),
                  true);
        return;
    }
    OpenProjectFile(PathUtil::ToUtf8(ChooseProjectFile(folder, projects)));
#else
    m_FileAction = FileAction::OpenProject;
    m_FileBrowser.Open(Tr("Open MaterialForge Project"), FileBrowser::Mode::Open, start, "",
                       {".mfproject"});
#endif
}

void Application::RequestNewFolder() {
    if (!m_Project.IsOpen()) {
        SetStatus(Tr("Create or open a project before creating folders."), true);
        return;
    }
    m_NewFolderName.clear();
    m_NewFolderError.clear();
    m_OpenNewFolderModal = true;
    m_NewFolderModalPending = true;
}

void Application::CollapseSelectionToFunction() {
    if (!m_GraphView) return;
    if (!m_Project.IsOpen()) {
        SetStatus(Tr("Open a project before collapsing nodes to a function."), true);
        return;
    }
    const fs::path functionsDirectory = m_Project.RootPath() / "functions";
    std::error_code ec;
    fs::create_directories(functionsDirectory, ec);
    fs::path functionPath;
    for (int index = 1; index < 10000; ++index) {
        functionPath = functionsDirectory /
                       ("MF_Collapsed_" + std::to_string(index) + ".mffunction");
        if (!fs::exists(functionPath, ec)) break;
    }
    std::string collapseError;
    if (m_GraphView->CollapseSelectedToFunction(PathUtil::ToUtf8(functionPath),
                                                &collapseError)) {
        m_FunctionAssetsDirty = true;
        RefreshProjectEntries(false);
        SetStatus(FormatLocalized(
            "Collapsed selection to %s",
            PathUtil::ToUtf8(functionPath.filename()).c_str()));
        LogMessage(LogLevel::Info,
                   "Collapsed nodes to function: " + PathUtil::ToUtf8(functionPath));
    } else {
        SetStatus(LocalizedErrorText(collapseError), true);
        LogMessage(LogLevel::Error, collapseError);
    }
}

void Application::RequestNewMaterial() {
    if (!m_Project.IsOpen()) {
        SetStatus(Tr("Create or open a project before creating materials."), true);
        return;
    }
    const fs::path directory = m_Project.ResolveRelative(m_ProjectCurrentDirectory);
    if (directory.empty() || !PathUtil::Exists(directory)) {
        SetStatus(Tr("The current project folder is unavailable."), true);
        return;
    }

    const std::string base = "M_NewMaterial";
    std::string candidate = base;
    for (int suffix = 2; PathUtil::Exists(directory / (candidate + ".json")); ++suffix) {
        candidate = base + "_" + std::to_string(suffix);
    }
    m_NewMaterialName = candidate;
    m_NewMaterialError.clear();
    m_OpenNewMaterialModal = true;
    m_NewMaterialModalPending = true;
}

void Application::RequestNewFunction() {
    if (!m_Project.IsOpen()) {
        SetStatus(Tr("Create or open a project before creating functions."), true);
        return;
    }
    const fs::path directory = m_Project.ResolveRelative(m_ProjectCurrentDirectory);
    if (directory.empty() || !PathUtil::Exists(directory)) {
        SetStatus(Tr("The current project folder is unavailable."), true);
        return;
    }
    const std::string base = "MF_NewFunction";
    std::string candidate = base;
    for (int suffix = 2; PathUtil::Exists(directory / (candidate + ".mffunction")); ++suffix) {
        candidate = base + "_" + std::to_string(suffix);
    }
    m_NewFunctionName = candidate;
    m_NewFunctionError.clear();
    m_OpenNewFunctionModal = true;
    m_NewFunctionModalPending = true;
}

bool Application::CreateBlueprintFunction(const std::string& functionName, std::string* error) {
    if (!m_Project.IsOpen()) {
        if (error) *error = "Create or open a project before creating functions.";
        return false;
    }

    std::string stem = Trim(functionName);
    const std::string extension = ".mffunction";
    if (stem.size() > extension.size() &&
        ToLower(stem.substr(stem.size() - extension.size())) == extension) {
        stem.resize(stem.size() - extension.size());
        stem = Trim(stem);
    }
    std::string reason;
    if (!MaterialProject::IsValidFolderName(stem, &reason)) {
        if (error) *error = reason;
        return false;
    }

    const fs::path directory = m_Project.ResolveRelative(m_ProjectCurrentDirectory);
    std::error_code ec;
    if (directory.empty() || !fs::is_directory(directory, ec)) {
        if (error) *error = "The current project folder is unavailable.";
        return false;
    }
    const fs::path target = directory / PathUtil::FromUtf8(stem + extension);
    if (fs::exists(target, ec)) {
        if (error) *error = "A function with that name already exists in this folder.";
        return false;
    }
    const std::string relative = m_Project.RelativePathOf(target);
    if (relative.empty()) {
        if (error) *error = "The function path is outside the project root.";
        return false;
    }

    MaterialGraph fresh;
    std::string buildError;
    if (!BuildStarterFunctionGraph(fresh, stem, &buildError)) {
        if (error) *error = "Could not build the new function: " + buildError;
        return false;
    }
    if (!GraphSerializer::Save(fresh, PathUtil::ToUtf8(target), error)) {
        return false;
    }

    SyncActiveDocument();
    m_Graph = std::move(fresh);
    m_Graph.MarkSaved();
    m_DocumentPath = target;
    m_DocumentKind = DocumentKind::Function;
    if (m_GraphView) {
        m_GraphView->SetFunctionMode(true);
        m_GraphView->OnGraphReplaced();
    }
    RegisterLiveDocument(DocumentKind::Function, target);
    m_LastContentRevision = 0;
    m_ForceShaderRefresh = true;
    m_FocusShaderTab = false;

    m_Project.AddResource(relative, "function", true);
    std::string projectError;
    m_Project.RefreshIntegrity(&projectError);
    m_Project.Save(&projectError);
    m_ProjectEntriesDirty = true;
    RefreshProjectEntries();
    m_ProjectSelectedEntry = relative;
    m_ProjectSearch.clear();
    m_ScrollProjectSelection = true;
    UpdateWindowTitle();
    SetStatus(FormatLocalized("Created blueprint function: %s.mffunction", stem.c_str()));
    if (error) error->clear();
    return true;
}

void Application::OpenFunctionAsset(const std::filesystem::path& path) {
    std::string error;
    if (!LoadDocument(PathUtil::ToUtf8(path), &error)) {
        SetStatus(FormatLocalized("Could not open function: %s", error.c_str()), true);
    } else if (m_DocumentKind != DocumentKind::Function) {
        m_DocumentKind = DocumentKind::Function;
        if (m_GraphView) m_GraphView->SetFunctionMode(true);
        UpdateWindowTitle();
    }
}

void Application::RequestAddProjectFile() {
    if (!m_Project.IsOpen()) {
        SetStatus(Tr("Create or open a project before adding files."), true);
        return;
    }
    m_FileAction = FileAction::AddProjectFile;
    fs::path start = m_Project.ResolveRelative(m_ProjectCurrentDirectory);
    if (start.empty() || !PathUtil::Exists(start)) {
        start = m_Project.RootPath();
    }
    m_FileBrowser.Open(Tr("Add File / Model to Project"), FileBrowser::Mode::Open, start, "", {});
}

bool Application::OpenProjectFile(const std::string& path, std::string* error) {
    std::string loadError;
    auto loaded = MaterialProject::Load(PathUtil::FromUtf8(path), &loadError);
    if (!loaded) {
        if (error) *error = loadError;
        SetStatus(FormatLocalized("Project open failed: %s", loadError.c_str()), true);
        return false;
    }

    m_Documents.clear();
    m_ActiveDocument = -1;
    m_Project = std::move(*loaded);
    {
        std::string syncError;
        if (!m_UeSync.Load(m_Project.RootPath(), &syncError) && !syncError.empty()) {
            LogMessage(LogLevel::Warning, syncError);
        }
    }
    m_ProjectCurrentDirectory.clear();
    m_ProjectSearch.clear();
    m_ProjectSelectedEntry.clear();
    m_ProjectClipboardPath.clear();
    m_ProjectClipboardMove = false;
    m_ProjectEntriesDirty = true;

    const std::string mainRelative = m_Project.MainMaterialPath();
    const fs::path mainAbsolute = m_Project.MainMaterialAbsolutePath();
    bool materialLoaded = false;
    std::string materialError;
    if (!mainRelative.empty() && !mainAbsolute.empty() && PathUtil::Exists(mainAbsolute)) {
        materialLoaded = LoadDocument(PathUtil::ToUtf8(mainAbsolute), &materialError);
    } else {
        materialError = "main material is missing";
    }

    if (!materialLoaded) {
        const std::string graphName = "M_" + MaterialProject::SanitizeProjectName(m_Project.Name());
        ResetGraphToNewMaterial(graphName, mainAbsolute);
        if (mainRelative.empty()) {
            m_DocumentPath.clear();
        }
    }

    std::string projectError;
    m_Project.RefreshIntegrity(&projectError);
    if (!m_Project.Save(&projectError)) {
        std::cerr << "Could not update project manifest: " << projectError << "\n";
    }
    m_RecentProjects.Touch(m_Project.ProjectFilePath(), m_Project.Name());
    std::string recentError;
    if (!m_RecentProjects.Save(&recentError)) {
        std::cerr << "Could not save recent projects: " << recentError << "\n";
    }

    RefreshProjectEntries(true);
    m_LastContentRevision = 0;
    m_ForceShaderRefresh = true;
    UpdateWindowTitle();

    if (m_Project.IsBroken()) {
        SetStatus(FormatLocalized("Project is broken (%d marked files missing), but available files were loaded.",
                                  static_cast<int>(m_Project.Issues().size())),
                  true);
    } else if (!materialLoaded) {
        SetStatus(FormatLocalized("Project opened, but the main material could not be loaded: %s",
                                  materialError.c_str()), true);
    } else {
        SetStatus(FormatLocalized("Project opened: %s", m_Project.Name().c_str()));
    }
    if (error) error->clear();
    return true;
}

bool Application::CreateProject(const std::string& name, const std::string& location,
                                std::string* error) {
    std::string createError;
    auto created = MaterialProject::Create(PathUtil::FromUtf8(location), name, &createError);
    if (!created) {
        if (error) *error = createError;
        return false;
    }

    m_Documents.clear();
    m_ActiveDocument = -1;
    m_Project = std::move(*created);
    m_UeSync.Clear();
    m_UeSync.Load(m_Project.RootPath(), nullptr);
    m_ProjectCurrentDirectory.clear();
    m_ProjectSearch.clear();
    m_ProjectSelectedEntry.clear();
    m_ProjectClipboardPath.clear();
    m_ProjectClipboardMove = false;
    m_ProjectEntriesDirty = true;

    const std::string safeName = MaterialProject::SanitizeProjectName(m_Project.Name());
    const fs::path materialPath = m_Project.MainMaterialAbsolutePath();
    ResetGraphToNewMaterial("M_" + safeName, materialPath);

    std::string materialError;
    if (materialPath.empty()) {
        materialError = "the generated project has no valid main material path";
    } else if (!GraphSerializer::Save(m_Graph, PathUtil::ToUtf8(materialPath), &materialError)) {
        // The project directory and manifest still exist, but opening it again
        // will correctly report a broken main-material reference.
    } else {
        m_Graph.MarkSaved();
    }
    if (!materialError.empty()) {
        SetStatus(FormatLocalized("Project created, but the initial material could not be saved: %s",
                                  materialError.c_str()), true);
    }

    std::string projectError;
    m_Project.RefreshIntegrity(&projectError);
    if (!m_Project.Save(&projectError)) {
        std::cerr << "Could not write project manifest: " << projectError << "\n";
    }
    m_RecentProjects.Touch(m_Project.ProjectFilePath(), m_Project.Name());
    std::string recentError;
    if (!m_RecentProjects.Save(&recentError)) {
        std::cerr << "Could not save recent projects: " << recentError << "\n";
    }
    RefreshProjectEntries(true);
    UpdateWindowTitle();
    if (!materialError.empty()) {
        SetStatus(Tr("Project created with warnings. See the Files panel."), true);
    } else if (m_Project.IsBroken()) {
        SetStatus(Tr("Project created but is broken: some marked files are missing."), true);
    } else {
        SetStatus(FormatLocalized("Project created: %s", m_Project.Name().c_str()));
    }
    if (error) error->clear();
    return true;
}

void Application::CloseProject() {
    if (!m_Project.IsOpen()) {
        return;
    }
    const std::string name = m_Project.Name();
    m_Project = MaterialProject{};
    m_UeSync.Clear();
    m_ProjectCurrentDirectory.clear();
    m_ProjectSearch.clear();
    m_ProjectSelectedEntry.clear();
    m_ProjectClipboardPath.clear();
    m_ProjectClipboardMove = false;
    m_ProjectEntries.clear();
    m_ProjectEntriesDirty = true;
    m_ProjectEntriesError.clear();
    SetStatus(FormatLocalized("Closed project: %s", name.c_str()));
    UpdateWindowTitle();
}

void Application::RefreshProjectEntries(bool resetDirectory) {
    m_FunctionAssetsDirty = true;
    if (!m_Project.IsOpen()) {
        m_ProjectEntries.clear();
        m_ProjectEntriesError.clear();
        m_ProjectSelectedEntry.clear();
        m_ProjectSelectedEntries.clear();
        m_ProjectSelectionAnchor.clear();
        m_ProjectEntriesDirty = false;
        return;
    }

    std::string error;
    m_ProjectEntries = m_Project.ListDirectory(m_ProjectCurrentDirectory, &error);
    if (!error.empty() && resetDirectory) {
        m_ProjectCurrentDirectory.clear();
        m_ProjectSelectedEntry.clear();
        m_ProjectSelectedEntries.clear();
        m_ProjectSelectionAnchor.clear();
        error.clear();
        m_ProjectEntries = m_Project.ListDirectory("", &error);
    }
    m_ProjectEntriesError = error;
    m_ProjectEntriesDirty = false;
}

void Application::NavigateProjectFolder(const std::string& relativeDirectory) {
    if (!m_Project.IsOpen()) {
        return;
    }
    m_ProjectCurrentDirectory = relativeDirectory;
    m_ProjectSelectedEntry.clear();
    m_ProjectSelectedEntries.clear();
    m_ProjectSelectionAnchor.clear();
    m_ProjectSearch.clear();
    m_ProjectEntriesDirty = true;
    RefreshProjectEntries();
}

void Application::RefreshProjectIntegrity() {
    if (!m_Project.IsOpen()) {
        return;
    }
    std::string error;
    m_Project.RefreshIntegrity(&error);
    if (!error.empty()) {
        SetStatus(FormatLocalized("Could not verify project: %s", error.c_str()), true);
    }
}

std::filesystem::path Application::ResolveInstanceParent(
    const MaterialInstance& instance, const std::filesystem::path& instancePath) const {
    const std::string& parent = instance.ParentMaterial();
    if (parent.empty()) return {};
    std::error_code ec;
    fs::path candidate = PathUtil::FromUtf8(parent);
    if (candidate.is_absolute()) {
        return fs::is_regular_file(candidate, ec) ? candidate : fs::path();
    }
    if (m_Project.IsOpen()) {
        candidate = m_Project.ResolveRelative(parent);
        if (!candidate.empty() && fs::is_regular_file(candidate, ec)) {
            return candidate;
        }
    }
    if (!instancePath.empty()) {
        candidate = instancePath.parent_path() / PathUtil::FromUtf8(parent);
        if (fs::is_regular_file(candidate, ec)) return candidate;
    }
    candidate = m_ContentRoot / PathUtil::FromUtf8(parent);
    if (fs::is_regular_file(candidate, ec)) return candidate;
    if (!m_DocumentPath.empty()) {
        candidate = m_DocumentPath.parent_path() / PathUtil::FromUtf8(parent);
        if (fs::is_regular_file(candidate, ec)) return candidate;
    }
    return {};
}

void Application::OpenMaterialInstance(const std::filesystem::path& path) {
    const std::string key = NormalizedPathKey(path);
    for (std::size_t i = 0; i < m_Documents.size(); ++i) {
        if (NormalizedPathKey(m_Documents[i].path) == key) {
            ActivateDocument(static_cast<int>(i));
            return;
        }
    }

    MaterialInstance instance;
    std::string error;
    if (!instance.Load(path, &error)) {
        SetStatus(FormatLocalized("Could not open material instance: %s", error.c_str()), true);
        return;
    }
    const fs::path parentPath = ResolveInstanceParent(instance, path);
    if (parentPath.empty()) {
        SetStatus(FormatLocalized("Material instance parent is missing: %s",
                                  instance.ParentMaterial().c_str()), true);
        return;
    }
    MaterialGraph parentGraph;
    if (!GraphSerializer::Load(PathUtil::ToUtf8(parentPath), parentGraph, &error)) {
        SetStatus(FormatLocalized("Could not load instance parent: %s", error.c_str()), true);
        return;
    }

    FlushUndoPending(true);
    SyncActiveDocument();
    std::vector<std::string> parameterWarnings;
    m_InstanceParameters = CollectMaterialParameters(parentGraph, &parameterWarnings);
    for (const std::string& warning : parameterWarnings) {
        LogMessage(LogLevel::Warning, warning);
    }
    std::vector<std::string> warnings;
    instance.ApplyToGraph(parentGraph, &warnings);
    for (const std::string& warning : warnings) {
        LogMessage(LogLevel::Warning, warning);
    }
    m_Instance = instance;
    if (m_Instance.Name().empty()) {
        m_Instance.SetName(path.stem().u8string());
    }
    m_Graph = std::move(parentGraph);
    m_Graph.MarkSaved();
    m_DocumentPath = path;
    m_DocumentKind = DocumentKind::Instance;
    if (m_GraphView) {
        m_GraphView->SetFunctionMode(false);
        m_GraphView->OnGraphReplaced();
    }
    RegisterLiveDocument(DocumentKind::Instance, path);
    m_LastContentRevision = 0;
    m_ForceShaderRefresh = true;
    m_FocusShaderTab = false;
    m_FocusInstanceTab = true;
    UpdateWindowTitle();
    SetStatus(FormatLocalized("Opened material instance: %s",
                              PathUtil::ToUtf8(path.filename()).c_str()));
}

bool Application::CreateMaterialInstanceFromMaterial(const std::string& relativeMaterial,
                                                      std::string* error) {
    if (error) error->clear();
    if (!m_Project.IsOpen()) {
        if (error) *error = "No project is open.";
        return false;
    }
    const fs::path parentPath = m_Project.ResolveRelative(relativeMaterial);
    if (parentPath.empty() || !PathUtil::Exists(parentPath)) {
        if (error) *error = "The parent material does not exist.";
        return false;
    }
    const fs::path parentRelative(relativeMaterial);
    const fs::path directory = parentRelative.parent_path();
    const std::string base = "MI_" + parentRelative.stem().u8string();
    std::string candidate = base;
    for (int suffix = 2; suffix < 10000; ++suffix) {
        const fs::path candidateRelative =
            directory.empty() ? fs::path(candidate + ".mfinstance")
                              : directory / (candidate + ".mfinstance");
        if (!PathUtil::Exists(m_Project.ResolveRelative(
                PathUtil::ToUtf8(candidateRelative.generic_u8string())))) {
            break;
        }
        candidate = base + "_" + std::to_string(suffix);
    }
    const fs::path instanceRelative =
        directory.empty() ? fs::path(candidate + ".mfinstance")
                          : directory / (candidate + ".mfinstance");
    const std::string instanceRelativeText = instanceRelative.generic_u8string();
    const fs::path instanceAbsolute = m_Project.ResolveRelative(instanceRelativeText);
    if (instanceAbsolute.empty()) {
        if (error) *error = "The instance path is outside the project.";
        return false;
    }

    MaterialInstance instance;
    instance.SetName(candidate);
    instance.SetParentMaterial(relativeMaterial);
    std::string saveError;
    if (!instance.Save(instanceAbsolute, &saveError)) {
        if (error) *error = saveError;
        return false;
    }
    m_Project.AddResource(instanceRelativeText, "instance", true);
    m_Project.Save(&saveError);
    m_Project.RefreshIntegrity(&saveError);
    m_ProjectEntriesDirty = true;
    RefreshProjectEntries();
    OpenMaterialInstance(instanceAbsolute);
    return true;
}

void Application::OpenProjectMaterial(const std::string& relativePath) {
    if (!m_Project.IsOpen()) {
        return;
    }
    const fs::path absolute = m_Project.ResolveRelative(relativePath);
    if (absolute.empty() || !PathUtil::Exists(absolute)) {
        RefreshProjectIntegrity();
        SetStatus(FormatLocalized("Material file is missing: %s", relativePath.c_str()), true);
        return;
    }
    std::string error;
    if (!LoadDocument(PathUtil::ToUtf8(absolute), &error)) {
        SetStatus(FormatLocalized("Could not open material: %s", error.c_str()), true);
        return;
    }
    if (!m_Project.SetMainMaterial(relativePath, &error)) {
        SetStatus(FormatLocalized("Material opened, but the project main material could not be updated: %s",
                                  error.c_str()), true);
        return;
    }
    std::string projectError;
    m_Project.RefreshIntegrity(&projectError);
    m_Project.Save(&projectError);
    m_ProjectEntriesDirty = true;
    RefreshProjectEntries();
    SetStatus(FormatLocalized("Opened project material: %s", BaseName(absolute).c_str()));
    UpdateWindowTitle();
}

void Application::SaveProject() {
    if (!m_Project.IsOpen()) {
        SetStatus(Tr("No project is open."), true);
        return;
    }
    std::string error;
    m_Project.RefreshIntegrity(&error);
    if (!m_Project.Save(&error)) {
        SetStatus(FormatLocalized("Could not save project: %s", error.c_str()), true);
        return;
    }
    m_RecentProjects.Touch(m_Project.ProjectFilePath(), m_Project.Name());
    std::string recentError;
    m_RecentProjects.Save(&recentError);
    m_ProjectEntriesDirty = true;
    UpdateWindowTitle();
    SetStatus(FormatLocalized("Project saved: %s", m_Project.Name().c_str()));
}

void Application::RequestRenameProjectFolder(const std::string& relativePath) {
    if (!m_Project.IsOpen() || relativePath.empty()) return;
    const fs::path path = PathUtil::FromUtf8(relativePath);
    m_RenameFolderRelative = relativePath;
    m_RenameFolderName = PathUtil::ToUtf8(path.filename());
    m_RenameFolderError.clear();
    m_OpenRenameFolderModal = true;
    m_RenameFolderModalPending = true;
}

void Application::DrawRenameFolderModal() {
    ImGui::SetNextWindowSize(ImVec2(Ui::S(440.0f), 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(Tr("Rename Folder"), &m_OpenRenameFolderModal,
                                ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        m_OpenRenameFolderModal = false;
        m_RenameFolderError.clear();
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::TextWrapped("%s", Tr("Rename the selected folder. Open documents inside it will keep their tabs."));
    ImGui::Spacing();
    ImGui::TextUnformatted(Tr("Folder name"));
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##rename_folder_name", "New folder name", &m_RenameFolderName,
                             ImGuiInputTextFlags_AutoSelectAll);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.52f, 0.66f, 0.69f, 1.0f));
    ImGui::TextWrapped("%s", Tr("Supports Chinese and spaces. Forbidden: \\ / : * ? \" < > |"));
    ImGui::PopStyleColor();
    if (!m_RenameFolderError.empty()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.45f, 0.39f, 1.0f));
        ImGui::TextWrapped("%s", LocalizedErrorText(m_RenameFolderError).c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
    ImGui::Separator();
    const float width = Ui::S(108.0f);
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - width * 2.0f -
                         ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::Button(Tr("Cancel"), ImVec2(width, Ui::S(30.0f)))) {
        m_OpenRenameFolderModal = false;
        m_RenameFolderError.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (Ui::AccentButton(Tr("Rename"), ImVec2(width, Ui::S(30.0f)))) {
        std::string error;
        RenameProjectFolder(m_RenameFolderRelative, m_RenameFolderName, &error);
        if (error.empty()) {
            m_OpenRenameFolderModal = false;
            m_RenameFolderError.clear();
            ImGui::CloseCurrentPopup();
        } else {
            m_RenameFolderError = error;
        }
    }
    ImGui::EndPopup();
}

void Application::RequestRenameProjectFile(const std::string& relativePath) {
    if (!m_Project.IsOpen() || relativePath.empty()) return;
    const fs::path path = PathUtil::FromUtf8(relativePath);
    m_RenameFileRelative = relativePath;
    m_RenameFileName = PathUtil::ToUtf8(path.filename());
    m_RenameFileError.clear();
    m_OpenRenameFileModal = true;
    m_RenameFileModalPending = true;
}

void Application::DrawRenameFileModal() {
    ImGui::SetNextWindowSize(ImVec2(Ui::S(460.0f), 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(Tr("Rename File"), &m_OpenRenameFileModal,
                                ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        m_OpenRenameFileModal = false;
        m_RenameFileError.clear();
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::TextWrapped("%s", Tr("Rename the selected file. The original extension is kept when omitted."));
    ImGui::Spacing();
    ImGui::TextDisabled("%s", m_RenameFileRelative.c_str());
    ImGui::Spacing();
    ImGui::TextUnformatted(Tr("File name"));
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1.0f);
    const bool acceptedWithEnter = ImGui::InputTextWithHint(
        "##rename_file_name", "New file name", &m_RenameFileName,
        ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.52f, 0.66f, 0.69f, 1.0f));
    ImGui::TextWrapped("%s", Tr("Supports Chinese and spaces. Forbidden: \\ / : * ? \" < > |"));
    ImGui::PopStyleColor();
    if (!m_RenameFileError.empty()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.45f, 0.39f, 1.0f));
        ImGui::TextWrapped("%s", LocalizedErrorText(m_RenameFileError).c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
    ImGui::Separator();
    const float width = Ui::S(108.0f);
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - width * 2.0f -
                         ImGui::GetStyle().ItemSpacing.x);
    bool cancel = ImGui::Button(Tr("Cancel"), ImVec2(width, Ui::S(30.0f)));
    ImGui::SameLine();
    const bool confirm = Ui::AccentButton(Tr("Rename"), ImVec2(width, Ui::S(30.0f)));
    if (cancel) {
        m_OpenRenameFileModal = false;
        m_RenameFileError.clear();
        ImGui::CloseCurrentPopup();
    } else if (confirm || acceptedWithEnter) {
        std::string error;
        RenameProjectFile(m_RenameFileRelative, m_RenameFileName, &error);
        if (error.empty()) {
            m_OpenRenameFileModal = false;
            m_RenameFileError.clear();
            ImGui::CloseCurrentPopup();
        } else {
            m_RenameFileError = error;
        }
    }
    ImGui::EndPopup();
}

void Application::RenameProjectFile(const std::string& relativePath,
                                    const std::string& newName, std::string* error) {
    if (error) error->clear();
    if (!m_Project.IsOpen()) {
        if (error) *error = "No project is open.";
        return;
    }
    const fs::path oldPath = m_Project.ResolveRelative(relativePath);
    std::error_code ec;
    if (oldPath.empty() || !fs::is_regular_file(oldPath, ec)) {
        if (error) *error = "The file no longer exists.";
        return;
    }

    const std::string oldExtension = PathUtil::ToUtf8(oldPath.extension());
    std::string cleaned = Trim(newName);
    if (cleaned.empty()) {
        if (error) *error = "Name cannot be empty.";
        return;
    }
    // Keep the asset extension; MaterialForge relies on it to classify files
    // (.json material/function, .mfinstance, textures, models, ...).
    if (!oldExtension.empty()) {
        fs::path candidate = PathUtil::FromUtf8(cleaned);
        const std::string extension = ToLower(PathUtil::ToUtf8(candidate.extension()));
        if (extension.empty()) {
            cleaned += oldExtension;
        } else if (extension != ToLower(oldExtension)) {
            if (error) {
                *error = std::string(Tr("Keep the original file extension: ")) + oldExtension;
            }
            return;
        }
    }
    const std::string oldFileName = PathUtil::ToUtf8(oldPath.filename());
    if (cleaned == oldFileName) {
        return;
    }

    SyncActiveDocument();
    const fs::path relativeSource = PathUtil::FromUtf8(relativePath);
    const std::string parent = relativeSource.parent_path().generic_u8string();
    const std::string newRelative =
        parent.empty() || parent == "." ? cleaned : parent + "/" + cleaned;
    std::string moveError;
    if (!m_Project.RenameEntry(relativePath, cleaned, &moveError)) {
        if (error) *error = moveError;
        return;
    }
    UpdateOpenDocumentPrefixes(relativePath, newRelative);
    RemapUeSyncPrefix(relativePath, newRelative);
    if (!m_Project.Save(&moveError)) {
        if (error) *error = moveError;
        return;
    }
    RefreshProjectIntegrity();
    m_ProjectEntriesDirty = true;
    m_FunctionAssetsDirty = true;
    m_ProjectSelectedEntry = newRelative;
    m_ProjectSearch.clear();
    m_ScrollProjectSelection = true;
    RefreshProjectEntries();
    SetStatus(FormatLocalized("Renamed %s to %s.", oldFileName.c_str(), cleaned.c_str()));
}

void Application::RequestProjectEntryDestination(const std::string& relativePath, bool move) {
    RequestProjectEntryDestination(std::vector<std::string>{relativePath}, move);
}

void Application::RequestProjectEntryDestination(
    const std::vector<std::string>& relativePaths, bool move) {
    if (!m_Project.IsOpen() || relativePaths.empty()) return;
    m_BrowseProjectEntries = relativePaths;
    m_FileAction = move ? FileAction::MoveProjectEntryTo : FileAction::CopyProjectEntryTo;
    fs::path start = m_Project.ResolveRelative(m_ProjectCurrentDirectory);
    std::error_code ec;
    if (start.empty() || !fs::is_directory(start, ec)) {
        start = m_Project.RootPath();
    }
    m_FileBrowser.Open(move
                           ? (relativePaths.size() > 1 ? Tr("Move Files To") : Tr("Move File To"))
                           : (relativePaths.size() > 1 ? Tr("Copy Files To") : Tr("Copy File To")),
                       FileBrowser::Mode::SelectFolder, start, "", {});
}

std::vector<std::string> Application::EffectiveProjectSelection() const {
    std::vector<std::string> selection;
    if (!m_ProjectSelectedEntries.empty()) {
        selection.assign(m_ProjectSelectedEntries.begin(), m_ProjectSelectedEntries.end());
    } else if (!m_ProjectSelectedEntry.empty()) {
        selection.push_back(m_ProjectSelectedEntry);
    }
    return selection;
}

void Application::DuplicateProjectEntry(const std::string& relativePath) {
    DuplicateProjectEntries(std::vector<std::string>{relativePath});
}

void Application::DuplicateProjectEntries(const std::vector<std::string>& relativePaths) {
    if (!m_Project.IsOpen() || relativePaths.empty()) return;
    std::vector<std::string> imported;
    std::string firstError;
    for (const std::string& relativePath : relativePaths) {
        fs::path source = PathUtil::FromUtf8(relativePath);
        std::string parent = source.parent_path().generic_u8string();
        if (parent == ".") parent.clear();
        std::string copy;
        std::string error;
        if (!m_Project.CopyProjectFile(relativePath, parent, &copy, &error)) {
            if (firstError.empty()) firstError = error;
            continue;
        }
        imported.push_back(copy);
    }
    std::string error;
    if (!m_Project.Save(&error)) {
        SetStatus(FormatLocalized("Could not save project: %s", error.c_str()), true);
        return;
    }
    RefreshProjectIntegrity();
    m_ProjectEntriesDirty = true;
    m_FunctionAssetsDirty = true;
    m_ProjectSelectedEntries.clear();
    if (!imported.empty()) {
        m_ProjectSelectedEntries.insert(imported.begin(), imported.end());
        m_ProjectSelectedEntry = imported.back();
        m_ProjectSelectionAnchor = imported.back();
    }
    m_ProjectSearch.clear();
    m_ScrollProjectSelection = true;
    RefreshProjectEntries();
    if (imported.size() > 1) {
        SetStatus(FormatLocalized("Duplicated %d file(s).", static_cast<int>(imported.size())));
    } else if (imported.size() == 1) {
        SetStatus(FormatLocalized("Copied %s.", imported.front().c_str()));
    } else {
        SetStatus(LocalizedErrorText(firstError), true);
    }
}

void Application::CopyProjectEntryTo(const std::string& relativePath,
                                     const std::string& targetDirectory) {
    CopyProjectEntries(std::vector<std::string>{relativePath}, targetDirectory);
}

void Application::CopyProjectEntries(const std::vector<std::string>& relativePaths,
                                     const std::string& targetDirectory) {
    if (!m_Project.IsOpen() || relativePaths.empty()) {
        SetStatus(Tr("No project is open."), true);
        return;
    }
    std::string lastImported;
    std::string firstError;
    int copied = 0;
    for (const std::string& source : relativePaths) {
        std::string imported;
        std::string error;
        if (!m_Project.CopyProjectFile(source, targetDirectory, &imported, &error)) {
            if (firstError.empty()) firstError = error;
            continue;
        }
        lastImported = imported;
        ++copied;
    }
    std::string saveError;
    if (!m_Project.Save(&saveError)) {
        SetStatus(FormatLocalized("Could not save project: %s", saveError.c_str()), true);
        return;
    }
    RefreshProjectIntegrity();
    m_ProjectEntriesDirty = true;
    m_FunctionAssetsDirty = true;
    m_ProjectSelectedEntries.clear();
    if (!lastImported.empty()) {
        m_ProjectSelectedEntries.insert(lastImported);
        m_ProjectSelectedEntry = lastImported;
        m_ProjectSelectionAnchor = lastImported;
    }
    m_ProjectSearch.clear();
    m_ScrollProjectSelection = true;
    RefreshProjectEntries();
    if (copied > 1) {
        SetStatus(FormatLocalized("Copied %d file(s).", copied));
    } else if (copied == 1) {
        SetStatus(FormatLocalized("Copied %s.", lastImported.c_str()));
    } else {
        SetStatus(LocalizedErrorText(firstError), true);
    }
}

void Application::RequestDeleteProjectEntries(const std::vector<std::string>& relativePaths) {
    if (!m_Project.IsOpen() || relativePaths.empty()) return;
    m_DeleteMultipleEntries = relativePaths;
    m_OpenDeleteMultipleModal = true;
    m_DeleteMultiplePending = true;
}

void Application::DeleteProjectEntries(const std::vector<std::string>& relativePaths) {
    if (!m_Project.IsOpen() || relativePaths.empty()) return;
    SyncActiveDocument();
    int deleted = 0;
    std::string firstError;
    for (const std::string& relative : relativePaths) {
        std::string error;
        if (!m_Project.DeleteEntry(relative, true, &error)) {
            if (firstError.empty()) firstError = error;
            continue;
        }
        const std::string prefix = relative + "/";
        for (int index = static_cast<int>(m_Documents.size()) - 1; index >= 0; --index) {
            const std::string documentRelative =
                m_Project.RelativePathOf(m_Documents[static_cast<std::size_t>(index)].path);
            if (documentRelative == relative ||
                documentRelative.rfind(prefix, 0) == 0) {
                CloseDocumentNow(index);
            }
        }
        RemoveUeSyncPrefix(relative);
        ++deleted;
    }
    std::string error;
    m_Project.Save(&error);
    const auto selectedWasDeleted = [&](const std::string& selected) {
        for (const std::string& relative : relativePaths) {
            if (selected == relative || selected.rfind(relative + "/", 0) == 0) {
                return true;
            }
        }
        return false;
    };
    const bool clearSelectedEntry = selectedWasDeleted(m_ProjectSelectedEntry);
    m_ProjectSelectedEntries.clear();
    if (clearSelectedEntry) {
        m_ProjectSelectedEntry.clear();
    }
    m_FunctionAssetsDirty = true;
    RefreshProjectIntegrity();
    RefreshProjectEntries(false);
    if (deleted > 0) {
        SetStatus(FormatLocalized("Deleted %d file(s).", deleted));
    } else {
        SetStatus(LocalizedErrorText(firstError), true);
    }
}

void Application::DrawDeleteMultipleModal() {
    ImGui::SetNextWindowSize(ImVec2(Ui::S(500.0f), 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(Tr("Delete Selected Files"), &m_OpenDeleteMultipleModal,
                                ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.45f, 0.39f, 1.0f));
    ImGui::TextWrapped("%s", Tr("This action cannot be undone."));
    ImGui::PopStyleColor();
    ImGui::Spacing();
    ImGui::TextWrapped("%s",
                       FormatLocalized("Delete %d selected file(s)?",
                                       static_cast<int>(m_DeleteMultipleEntries.size()))
                           .c_str());
    ImGui::BeginChild("##delete_multiple_list", ImVec2(0.0f, Ui::S(180.0f)),
                      ImGuiChildFlags_Borders);
    for (const std::string& relative : m_DeleteMultipleEntries) {
        ImGui::TextUnformatted(relative.c_str());
    }
    ImGui::EndChild();
    ImGui::Spacing();
    ImGui::Separator();
    const float width = Ui::S(108.0f);
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - width * 2.0f -
                         ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::Button(Tr("Cancel"), ImVec2(width, Ui::S(30.0f)))) {
        m_OpenDeleteMultipleModal = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.72f, 0.20f, 0.18f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.86f, 0.27f, 0.23f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.62f, 0.15f, 0.13f, 1.0f));
    const bool confirmed = ImGui::Button(Tr("Delete"), ImVec2(width, Ui::S(30.0f)));
    ImGui::PopStyleColor(3);
    if (confirmed) {
        const std::vector<std::string> entries = m_DeleteMultipleEntries;
        m_OpenDeleteMultipleModal = false;
        m_DeleteMultipleEntries.clear();
        ImGui::CloseCurrentPopup();
        DeleteProjectEntries(entries);
    }
    ImGui::EndPopup();
}

void Application::RemapUeSyncPrefix(const std::string& oldPrefix,
                                    const std::string& newPrefix) {
    if (oldPrefix.empty() || newPrefix.empty() || oldPrefix == newPrefix) return;
    std::string error;
    if (!m_UeSync.RemapPrefix(oldPrefix, newPrefix, &error)) return;
    if (!m_UeSync.Save(&error)) {
        LogMessage(LogLevel::Warning, "Could not save UE sync state: " + error);
    }
}

void Application::RemoveUeSyncPrefix(const std::string& prefix) {
    if (prefix.empty()) return;
    std::string error;
    if (!m_UeSync.RemovePrefix(prefix, &error)) return;
    if (!m_UeSync.Save(&error)) {
        LogMessage(LogLevel::Warning, "Could not save UE sync state: " + error);
    }
}

void Application::DrawDeleteEntryModal() {
    ImGui::SetNextWindowSize(ImVec2(Ui::S(500.0f), 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(Tr("Delete Entry"), &m_OpenDeleteEntryModal,
                                ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        m_OpenDeleteEntryModal = false;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.45f, 0.39f, 1.0f));
    ImGui::TextWrapped("%s", Tr("This action cannot be undone."));
    ImGui::PopStyleColor();
    ImGui::Spacing();
    if (m_DeleteEntryIsDirectory) {
        ImGui::TextWrapped(
            "%s", FormatLocalized("Delete the folder \"%s\" and all of its contents?",
                                  m_DeleteEntryName.c_str())
                      .c_str());
    } else {
        ImGui::TextWrapped("%s",
                           FormatLocalized("Delete the file \"%s\"?",
                                           m_DeleteEntryName.c_str())
                               .c_str());
    }
    ImGui::Spacing();
    ImGui::TextDisabled("%s", m_DeleteEntryRelative.c_str());

    bool openDocumentAffected = false;
    const std::string prefix = m_DeleteEntryRelative + "/";
    for (const EditorDocument& document : m_Documents) {
        const std::string relative = m_Project.RelativePathOf(document.path);
        if (relative == m_DeleteEntryRelative ||
            relative.rfind(prefix, 0) == 0) {
            openDocumentAffected = true;
            break;
        }
    }
    if (openDocumentAffected) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.98f, 0.72f, 0.35f, 1.0f), "%s",
                           Tr("An open document under this path will be closed."));
    }

    ImGui::Spacing();
    ImGui::Separator();
    const float width = Ui::S(108.0f);
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - width * 2.0f -
                         ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::Button(Tr("Cancel"), ImVec2(width, Ui::S(30.0f)))) {
        m_OpenDeleteEntryModal = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.72f, 0.20f, 0.18f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.86f, 0.27f, 0.23f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.62f, 0.15f, 0.13f, 1.0f));
    const bool confirmed = ImGui::Button(Tr("Delete"), ImVec2(width, Ui::S(30.0f)));
    ImGui::PopStyleColor(3);
    if (confirmed) {
        std::string error;
        const std::string deleted = m_DeleteEntryRelative;
        if (m_Project.DeleteEntry(deleted, true, &error)) {
            for (int index = static_cast<int>(m_Documents.size()) - 1; index >= 0; --index) {
                const std::string relative =
                    m_Project.RelativePathOf(m_Documents[static_cast<std::size_t>(index)].path);
                if (relative == deleted || relative.rfind(prefix, 0) == 0) {
                    CloseDocumentNow(index);
                }
            }
            if (m_DeleteEntryIsDirectory &&
                (m_ProjectCurrentDirectory == deleted ||
                 m_ProjectCurrentDirectory.rfind(prefix, 0) == 0)) {
                const std::size_t slash = deleted.find_last_of('/');
                m_ProjectCurrentDirectory =
                    slash == std::string::npos ? std::string() : deleted.substr(0, slash);
            }
            if (m_ProjectSelectedEntry == deleted ||
                m_ProjectSelectedEntry.rfind(prefix, 0) == 0) {
                m_ProjectSelectedEntry.clear();
            }
            m_FunctionAssetsDirty = true;
            RemoveUeSyncPrefix(deleted);
            RefreshProjectIntegrity();
            RefreshProjectEntries(false);
            LogMessage(LogLevel::Info,
                       std::string("Deleted project entry: ") + deleted);
            SetStatus(std::string(Tr("Deleted: ")) + deleted);
            m_OpenDeleteEntryModal = false;
            ImGui::CloseCurrentPopup();
        } else {
            SetStatus(LocalizedErrorText(error), true);
            LogMessage(LogLevel::Error, error);
        }
    }
    ImGui::EndPopup();
}

void Application::RenameProjectFolder(const std::string& relativePath,
                                      const std::string& newName, std::string* error) {
    if (error) error->clear();
    if (!m_Project.IsOpen()) {
        if (error) *error = "No project is open.";
        return;
    }
    const fs::path oldPath = PathUtil::FromUtf8(relativePath);
    const std::string parent = oldPath.parent_path().generic_u8string();
    const std::string cleanedName = Trim(newName);
    std::string candidate = parent.empty() || parent == "." ? cleanedName : parent + "/" + cleanedName;
    SyncActiveDocument();
    std::string moveError;
    if (!m_Project.RenameEntry(relativePath, cleanedName, &moveError)) {
        if (error) *error = moveError;
        return;
    }
    UpdateOpenDocumentPrefixes(relativePath, candidate);
    RemapUeSyncPrefix(relativePath, candidate);
    if (!m_Project.Save(&moveError)) {
        if (error) *error = moveError;
        return;
    }
    RefreshProjectIntegrity();
    m_ProjectEntriesDirty = true;
    m_ProjectSelectedEntry = candidate;
    RefreshProjectEntries();
    SetStatus(FormatLocalized("Renamed folder to %s.", cleanedName.c_str()));
}

void Application::SetProjectFolderColor(const std::string& relativePath,
                                        const std::string& color) {
    if (!m_Project.IsOpen()) return;
    std::string error;
    if (!m_Project.SetFolderColor(relativePath, color, &error)) {
        SetStatus(LocalizedErrorText(error), true);
        return;
    }
    if (!m_Project.Save(&error)) {
        SetStatus(FormatLocalized("Could not save project: %s", error.c_str()), true);
        return;
    }
    m_ProjectEntriesDirty = true;
}

void Application::MoveProjectEntry(const std::string& relativePath,
                                   const std::string& targetDirectory) {
    MoveProjectEntries(std::vector<std::string>{relativePath}, targetDirectory);
}

void Application::MoveProjectEntries(const std::vector<std::string>& relativePaths,
                                     const std::string& targetDirectory) {
    if (!m_Project.IsOpen() || relativePaths.empty()) return;
    SyncActiveDocument();
    std::string lastMoved;
    std::string firstError;
    int moved = 0;
    for (const std::string& relativePath : relativePaths) {
        const fs::path source = PathUtil::FromUtf8(relativePath);
        const std::string name = PathUtil::ToUtf8(source.filename());
        const std::string newRelative =
            targetDirectory.empty() ? name : targetDirectory + "/" + name;
        std::string error;
        if (!m_Project.MoveEntry(relativePath, targetDirectory, &error)) {
            if (firstError.empty()) firstError = error;
            continue;
        }
        UpdateOpenDocumentPrefixes(relativePath, newRelative);
        RemapUeSyncPrefix(relativePath, newRelative);
        lastMoved = newRelative;
        ++moved;
    }
    std::string error;
    if (!m_Project.Save(&error)) {
        SetStatus(FormatLocalized("Could not save project: %s", error.c_str()), true);
        return;
    }
    RefreshProjectIntegrity();
    m_ProjectEntriesDirty = true;
    m_FunctionAssetsDirty = true;
    m_ProjectSelectedEntries.clear();
    if (!lastMoved.empty()) {
        m_ProjectSelectedEntries.insert(lastMoved);
        m_ProjectSelectedEntry = lastMoved;
        m_ProjectSelectionAnchor = lastMoved;
    }
    RefreshProjectEntries();
    if (moved > 1) {
        SetStatus(FormatLocalized("Moved %d file(s).", moved));
    } else if (moved == 1) {
        SetStatus(FormatLocalized("Moved %s.", PathUtil::ToUtf8(PathUtil::FromUtf8(lastMoved).filename()).c_str()));
    } else {
        SetStatus(LocalizedErrorText(firstError), true);
    }
}

void Application::CopyProjectEntryToClipboard(const std::string& relativePath, bool move) {
    if (!m_Project.IsOpen() || relativePath.empty()) return;
    m_ProjectClipboardPath = relativePath;
    m_ProjectClipboardMove = move;
    SetStatus(move ? Tr("Marked for moving. Paste it into a folder.")
                   : Tr("Copied. Paste it into a folder."));
}

void Application::PasteProjectClipboard(const std::string& targetDirectory) {
    if (!m_Project.IsOpen() || m_ProjectClipboardPath.empty()) {
        SetStatus(Tr("The project clipboard is empty."), true);
        return;
    }
    const std::string source = m_ProjectClipboardPath;
    const fs::path sourcePath = PathUtil::FromUtf8(source);
    const std::string name = PathUtil::ToUtf8(sourcePath.filename());
    std::string imported;
    std::string error;
    if (m_ProjectClipboardMove) {
        const std::string newRelative =
            targetDirectory.empty() ? name : targetDirectory + "/" + name;
        SyncActiveDocument();
        if (!m_Project.MoveEntry(source, targetDirectory, &error)) {
            SetStatus(LocalizedErrorText(error), true);
            return;
        }
        UpdateOpenDocumentPrefixes(source, newRelative);
        RemapUeSyncPrefix(source, newRelative);
        m_ProjectClipboardPath.clear();
        imported = newRelative;
    } else {
        if (!m_Project.CopyProjectFile(source, targetDirectory, &imported, &error)) {
            SetStatus(LocalizedErrorText(error), true);
            return;
        }
    }
    if (!m_Project.Save(&error)) {
        SetStatus(FormatLocalized("Could not save project: %s", error.c_str()), true);
        return;
    }
    RefreshProjectIntegrity();
    m_ProjectEntriesDirty = true;
    m_ProjectSelectedEntry = imported;
    m_ProjectSearch.clear();
    m_ScrollProjectSelection = true;
    RefreshProjectEntries();
    SetStatus(FormatLocalized("Pasted %s.", name.c_str()));
}

void Application::UpdateOpenDocumentPrefixes(const std::string& oldPrefix,
                                             const std::string& newPrefix) {
    if (oldPrefix.empty() || newPrefix.empty()) return;
    const auto matches = [&](const std::string& path) {
        return path == oldPrefix || path.rfind(oldPrefix + "/", 0) == 0;
    };
    const auto remap = [&](const std::string& path) {
        return newPrefix + path.substr(oldPrefix.size());
    };
    const fs::path projectRootNormalized = m_Project.RootPath().lexically_normal();
    const auto lexicalRelative = [&](const fs::path& absolute) {
        const fs::path relative =
            absolute.lexically_normal().lexically_relative(projectRootNormalized);
        const std::string generic = relative.generic_u8string();
        if (generic.empty() || generic == ".." || generic.rfind("../", 0) == 0) return std::string();
        return generic;
    };
    if (matches(m_ProjectCurrentDirectory)) {
        m_ProjectCurrentDirectory = remap(m_ProjectCurrentDirectory);
    }
    if (matches(m_ProjectSelectedEntry)) {
        m_ProjectSelectedEntry = remap(m_ProjectSelectedEntry);
    }
    if (matches(m_ProjectClipboardPath)) {
        m_ProjectClipboardPath = remap(m_ProjectClipboardPath);
    }
    for (EditorDocument& document : m_Documents) {
        if (document.path.empty()) continue;
        const std::string relative = lexicalRelative(document.path);
        if (!relative.empty() && matches(relative)) {
            document.path = m_Project.ResolveRelative(remap(relative));
            document.title = DocumentTitle(document.path, document.graph,
                                           document.kind == DocumentKind::Function);
        }
    }
    if (!m_DocumentPath.empty()) {
        const std::string relative = lexicalRelative(m_DocumentPath);
        if (!relative.empty() && matches(relative)) {
            m_DocumentPath = m_Project.ResolveRelative(remap(relative));
        }
    }
    UpdateWindowTitle();
}

bool Application::AddFileToProject(const std::string& sourcePath) {
    if (!m_Project.IsOpen()) {
        SetStatus(Tr("Create or open a project before adding files."), true);
        return false;
    }
    std::string importedRelative;
    std::string error;
    if (!m_Project.ImportFile(PathUtil::FromUtf8(sourcePath), m_ProjectCurrentDirectory,
                              &importedRelative, &error)) {
        SetStatus(FormatLocalized("Could not add file: %s", error.c_str()), true);
        return false;
    }
    RefreshProjectIntegrity();
    m_ProjectEntriesDirty = true;
    RefreshProjectEntries();
    m_ProjectSelectedEntry = importedRelative;
    m_ProjectSearch.clear();
    m_ScrollProjectSelection = true;
    SetStatus(FormatLocalized("Added %s to the project.", importedRelative.c_str()));
    return true;
}

void Application::FileDropCallback(GLFWwindow* window, int count, const char** paths) {
    Application* app = static_cast<Application*>(glfwGetWindowUserPointer(window));
    if (!app) return;
    app->QueueDroppedFiles(count, paths);
}

void Application::QueueDroppedFiles(int count, const char** paths) {
    if (count <= 0 || !paths) return;
    double cursorX = 0.0;
    double cursorY = 0.0;
    if (m_Window) glfwGetCursorPos(m_Window, &cursorX, &cursorY);
    const float margin = Ui::S(14.0f);
    const bool insideBrowser =
        !m_ContentBrowserRectValid ||
        (cursorX >= m_ContentBrowserMinX - margin && cursorY >= m_ContentBrowserMinY - margin &&
         cursorX <= m_ContentBrowserMaxX + margin && cursorY <= m_ContentBrowserMaxY + margin);
    if (insideBrowser) m_DropOnContentBrowser = true;
    for (int i = 0; i < count; ++i) {
        if (paths[i] && paths[i][0]) m_PendingDroppedFiles.emplace_back(paths[i]);
    }
}

void Application::ProcessDroppedFiles() {
    if (m_PendingDroppedFiles.empty()) return;
    std::vector<std::string> files = std::move(m_PendingDroppedFiles);
    m_PendingDroppedFiles.clear();
    const bool onBrowser = m_DropOnContentBrowser;
    m_DropOnContentBrowser = false;

    if (!m_Project.IsOpen()) {
        SetStatus(Tr("Create or open a project before adding files."), true);
        return;
    }
    if (m_ContentBrowserRectValid && !onBrowser) {
        SetStatus(Tr("Drop files onto the Content Browser to import them."), true);
        return;
    }

    int imported = 0;
    int failed = 0;
    for (const std::string& path : files) {
        std::error_code ec;
        if (!fs::is_regular_file(PathUtil::FromUtf8(path), ec)) {
            ++failed;
            continue;
        }
        if (AddFileToProject(path)) {
            ++imported;
        } else {
            ++failed;
        }
    }
    if (imported > 0 && failed == 0) {
        SetStatus(FormatLocalized("Imported %d file(s) from Explorer.", imported));
    } else if (imported > 0) {
        SetStatus(FormatLocalized("Imported %d file(s); %d could not be imported.", imported,
                                  failed), true);
    } else {
        SetStatus(Tr("Could not import the dropped files."), true);
    }
}

void Application::ExecuteNewDocument() {
    ResetGraphToNewMaterial("M_NewMaterial", {});
    SetStatus(Tr("New material created."));
}

void Application::ResetGraphToNewMaterial(const std::string& materialName,
                                          const std::filesystem::path& documentPath) {
    FlushUndoPending(true);
    SyncActiveDocument();
    MaterialGraph fresh;
    std::string error;
    if (!BuildStarterMaterialGraph(fresh, materialName, &error)) {
        SetStatus(FormatLocalized("Could not create a material: %s", error.c_str()), true);
        return;
    }
    m_Graph = std::move(fresh);
    m_DocumentPath = documentPath;
    m_DocumentKind = DocumentKind::Material;
    if (m_GraphView) {
        m_GraphView->SetFunctionMode(false);
        m_GraphView->OnGraphReplaced();
    }
    RegisterLiveDocument(DocumentKind::Material, documentPath);
    m_LastContentRevision = 0;
    m_ForceShaderRefresh = true;
    m_FocusShaderTab = false;
    UpdateWindowTitle();
}

const Application::GraphThumbnail& Application::GetGraphThumbnail(
    const std::filesystem::path& path) {
    const std::string key = PathUtil::ToUtf8(path);
    auto& entry = m_GraphThumbnails[key];
    if (entry.attempted) {
        return entry;
    }
    entry.attempted = true;
    MaterialGraph graph;
    std::string error;
    if (!GraphSerializer::Load(PathUtil::ToUtf8(path), graph, &error)) {
        return entry;
    }
    std::unordered_map<NodeId, int> indexById;
    for (const auto& node : graph.Nodes()) {
        if (node->type == "Comment") continue;
        GraphThumbnail::Node thumbNode;
        thumbNode.position = node->position;
        thumbNode.type = node->type;
        indexById[node->id] = static_cast<int>(entry.nodes.size());
        entry.nodes.push_back(std::move(thumbNode));
    }
    for (const Link& link : graph.Links()) {
        const Pin* fromPin = graph.FindPin(link.fromPin);
        const Pin* toPin = graph.FindPin(link.toPin);
        if (!fromPin || !toPin) continue;
        const auto from = indexById.find(fromPin->nodeId);
        const auto to = indexById.find(toPin->nodeId);
        if (from != indexById.end() && to != indexById.end()) {
            entry.links.emplace_back(from->second, to->second);
        }
    }
    entry.valid = !entry.nodes.empty();
    return entry;
}

unsigned int Application::GetMaterialThumbnail(const std::filesystem::path& path) {
    const std::string key = PathUtil::ToUtf8(path);
    auto& entry = m_MaterialThumbnails[key];
    if (entry.attempted) {
        return entry.texture;
    }
    entry.attempted = true;

    MaterialGraph graph;
    std::string error;
    if (!GraphSerializer::Load(key, graph, &error)) {
        return 0;
    }
    const fs::path baseDirectory = m_Project.IsOpen() ? m_Project.RootPath()
                                                      : path.parent_path();
    GeneratedShader shader = GlslGenerator().Generate(
        graph, m_GlslVersion, baseDirectory, PreviewBuffer::Lit, PreviewLighting());
    if (!shader.success) {
        return 0;
    }
    for (ShaderTextureBinding& texture : shader.textures) {
        if (texture.assetPath.empty()) continue;
        fs::path resolved;
        if (m_Project.IsOpen()) {
            const fs::path candidate = m_Project.ResolveRelative(texture.assetPath);
            std::error_code ec;
            if (!candidate.empty() && fs::is_regular_file(candidate, ec)) {
                resolved = candidate;
            }
        }
        if (resolved.empty()) {
            resolved = PathUtil::ResolveResource(texture.assetPath);
        }
        texture.assetPath = PathUtil::ToUtf8(resolved);
    }

    if (!m_MaterialThumbnailRenderer) {
        m_MaterialThumbnailRenderer = std::make_unique<MaterialThumbnailRenderer>();
        if (!m_MaterialThumbnailRenderer->Initialize(&error)) {
            LogMessage(LogLevel::Warning, "Material thumbnails unavailable: " + error);
            m_MaterialThumbnailRenderer.reset();
            return 0;
        }
    }
    std::string renderError;
    int thumbnailWidth = 0;
    int thumbnailHeight = 0;
    // Render with the same aspect ratio as the content-browser preview area so
    // the sphere stays circular when the HUD stretches the texture into a wide
    // tile. The previous square 96x96 target was being stretched into ~108x62.
    const unsigned int texture = m_MaterialThumbnailRenderer->Render(
        graph, shader, 160, 92, &thumbnailWidth, &thumbnailHeight, &renderError);
    if (texture == 0) {
        if (!renderError.empty()) {
            LogMessage(LogLevel::Warning, "Material thumbnail failed: " + renderError);
        }
        return 0;
    }
    entry.texture = texture;
    entry.width = thumbnailWidth > 0 ? thumbnailWidth : 160;
    entry.height = thumbnailHeight > 0 ? thumbnailHeight : 92;
    return entry.texture;
}

bool Application::DrawGraphThumbnail(ImDrawList* drawList, ImVec2 min, ImVec2 max,
                                     const std::filesystem::path& path) {
    if (!drawList) return false;
    const GraphThumbnail& thumbnail = GetGraphThumbnail(path);
    if (!thumbnail.valid || thumbnail.nodes.empty()) return false;

    Vec2 boundsMin = thumbnail.nodes.front().position;
    Vec2 boundsMax = boundsMin;
    for (const GraphThumbnail::Node& node : thumbnail.nodes) {
        boundsMin.x = std::min(boundsMin.x, node.position.x);
        boundsMin.y = std::min(boundsMin.y, node.position.y);
        boundsMax.x = std::max(boundsMax.x, node.position.x);
        boundsMax.y = std::max(boundsMax.y, node.position.y);
    }
    const float spanX = std::max(1.0f, boundsMax.x - boundsMin.x);
    const float spanY = std::max(1.0f, boundsMax.y - boundsMin.y);
    const float padding = Ui::S(8.0f);
    const float availableWidth = std::max(1.0f, (max.x - min.x) - padding * 2.0f);
    const float availableHeight = std::max(1.0f, (max.y - min.y) - padding * 2.0f);
    const float scale = std::min(availableWidth / spanX, availableHeight / spanY);
    const float offsetX = (availableWidth - spanX * scale) * 0.5f;
    const float offsetY = (availableHeight - spanY * scale) * 0.5f;
    const auto project = [&](const Vec2& position) {
        return ImVec2(min.x + padding + offsetX + (position.x - boundsMin.x) * scale,
                      min.y + padding + offsetY + (position.y - boundsMin.y) * scale);
    };

    drawList->PushClipRect(min, max, true);
    for (const auto& [from, to] : thumbnail.links) {
        if (from < 0 || to < 0 ||
            from >= static_cast<int>(thumbnail.nodes.size()) ||
            to >= static_cast<int>(thumbnail.nodes.size())) {
            continue;
        }
        const ImVec2 a = project(thumbnail.nodes[static_cast<std::size_t>(from)].position);
        const ImVec2 b = project(thumbnail.nodes[static_cast<std::size_t>(to)].position);
        drawList->AddLine(a, b, IM_COL32(112, 160, 158, 80), 1.0f);
    }
    for (const GraphThumbnail::Node& node : thumbnail.nodes) {
        const ImVec2 position = project(node.position);
        const NodeDefinition* definition = NodeRegistry::Get().Find(node.type);
        const ImU32 color = Ui::CategoryU32(
            definition ? definition->category : std::string("Utility"));
        const float width = definition && definition->isMaterialOutput ? Ui::S(8.0f)
                                                                       : Ui::S(5.0f);
        const float height = definition && definition->isMaterialOutput ? Ui::S(5.0f)
                                                                        : Ui::S(3.0f);
        drawList->AddRectFilled(ImVec2(position.x - width * 0.5f,
                                       position.y - height * 0.5f),
                                ImVec2(position.x + width * 0.5f,
                                       position.y + height * 0.5f),
                                color, 1.5f);
    }
    drawList->PopClipRect();
    return true;
}

unsigned int Application::GetThumbnailTexture(const std::filesystem::path& path) {
    const std::string key = PathUtil::ToUtf8(path);
    auto& entry = m_Thumbnails[key];
    if (entry.attempted) {
        return entry.texture;
    }
    entry.attempted = true;

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return 0;
    }
    std::vector<unsigned char> encoded((std::istreambuf_iterator<char>(stream)),
                                       std::istreambuf_iterator<char>());
    if (encoded.empty() || encoded.size() > 64u * 1024u * 1024u) {
        return 0;
    }
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_set_flip_vertically_on_load(1);
    stbi_uc* pixels = stbi_load_from_memory(encoded.data(), static_cast<int>(encoded.size()),
                                            &width, &height, &channels, 4);
    if (!pixels || width <= 0 || height <= 0) {
        if (pixels) stbi_image_free(pixels);
        return 0;
    }

    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(pixels);

    entry.texture = texture;
    entry.width = width;
    entry.height = height;
    return texture;
}

void Application::ClearThumbnailCache() {
    for (auto& [key, entry] : m_Thumbnails) {
        (void)key;
        if (entry.texture != 0) {
            glDeleteTextures(1, &entry.texture);
            entry.texture = 0;
        }
    }
    m_Thumbnails.clear();
    for (auto& [key, entry] : m_MaterialThumbnails) {
        (void)key;
        if (entry.texture != 0) {
            glDeleteTextures(1, &entry.texture);
            entry.texture = 0;
        }
    }
    m_MaterialThumbnails.clear();
    m_GraphThumbnails.clear();
}

bool Application::CreateProjectMaterial(const std::string& materialName, std::string* error) {
    if (!m_Project.IsOpen()) {
        if (error) *error = "Create or open a project before creating materials.";
        return false;
    }

    std::string stem = Trim(materialName);
    if (stem.size() > 5 && ToLower(stem.substr(stem.size() - 5)) == ".json") {
        stem.resize(stem.size() - 5);
        stem = Trim(stem);
    }
    std::string reason;
    if (!MaterialProject::IsValidFolderName(stem, &reason)) {
        if (error) *error = reason;
        return false;
    }

    const fs::path directory = m_Project.ResolveRelative(m_ProjectCurrentDirectory);
    std::error_code ec;
    if (directory.empty() || !fs::is_directory(directory, ec)) {
        if (error) *error = "The current project folder is unavailable.";
        return false;
    }
    const fs::path target = directory / PathUtil::FromUtf8(stem + ".json");
    if (fs::exists(target, ec)) {
        if (error) *error = "A material with that name already exists in this folder.";
        return false;
    }
    const std::string relative = m_Project.RelativePathOf(target);
    if (relative.empty()) {
        if (error) *error = "The material path is outside the project root.";
        return false;
    }

    MaterialGraph fresh;
    std::string buildError;
    if (!BuildStarterMaterialGraph(fresh, stem, &buildError)) {
        if (error) *error = "Could not build the new material: " + buildError;
        return false;
    }
    if (!GraphSerializer::Save(fresh, PathUtil::ToUtf8(target), error)) {
        return false;
    }

    SyncActiveDocument();
    m_Graph = std::move(fresh);
    m_DocumentPath = target;
    m_DocumentKind = DocumentKind::Material;
    m_Graph.MarkSaved();
    m_GraphView->SetFunctionMode(false);
    m_GraphView->OnGraphReplaced();
    RegisterLiveDocument(DocumentKind::Material, target);
    m_LastContentRevision = 0;
    m_ForceShaderRefresh = true;
    m_FocusShaderTab = false;

    std::string projectError;
    m_Project.SetMainMaterial(relative, &projectError);
    m_Project.RefreshIntegrity(&projectError);
    m_Project.Save(&projectError);
    m_ProjectEntriesDirty = true;
    RefreshProjectEntries();
    m_ProjectSelectedEntry = relative;
    m_ProjectSearch.clear();
    m_ScrollProjectSelection = true;
    UpdateWindowTitle();
    SetStatus(FormatLocalized("Created project material: %s.json", stem.c_str()));
    if (error) error->clear();
    return true;
}

void Application::UpdateProjectMainMaterialAfterSave(const std::filesystem::path& path) {
    if (!m_Project.IsOpen() || !m_Project.Contains(path)) {
        return;
    }
    const std::string relative = m_Project.RelativePathOf(path);
    if (relative.empty()) {
        return;
    }
    std::string error;
    m_Project.SetMainMaterial(relative, &error);
    m_Project.RefreshIntegrity(&error);
    m_Project.Save(&error);
    m_ProjectEntriesDirty = true;
    UpdateWindowTitle();
}

void Application::RequestNewDocument() {
    ExecuteNewDocument();
}

void Application::RequestOpenDialog() {
    m_FileAction = FileAction::OpenGraph;
    fs::path start = m_DocumentPath.empty() ? DefaultSaveDirectory() : m_DocumentPath.parent_path();
    m_FileBrowser.Open(Tr("Open Material, Function or Instance"), FileBrowser::Mode::Open, start, "",
                       {".json", ".mffunction", ".mfinstance"});
}

void Application::RequestSave(bool saveAs) {
    if (!saveAs && !m_DocumentPath.empty()) {
        std::string error;
        if (SaveDocument(PathUtil::ToUtf8(m_DocumentPath), true, &error)) {
            SetStatus(FormatLocalized("Saved %s", BaseName(m_DocumentPath).c_str()));
        } else {
            SetStatus(FormatLocalized("Save failed: %s", error.c_str()), true);
            if (m_PendingCloseSaveAs) {
                m_PendingCloseSaveAs = false;
                m_OpenCloseDocumentModal = true;
            }
        }
        return;
    }
    m_FileAction = FileAction::SaveGraphAs;
    const fs::path start = m_DocumentPath.empty() ? DefaultSaveDirectory()
                                                  : m_DocumentPath.parent_path();
    const std::string extension = m_DocumentKind == DocumentKind::Function
                                      ? ".mffunction"
                                      : (m_DocumentKind == DocumentKind::Instance
                                             ? ".mfinstance"
                                             : ".json");
    const std::string baseName = m_DocumentKind == DocumentKind::Instance &&
                                         !m_Instance.Name().empty()
                                     ? m_Instance.Name()
                                     : m_Graph.Settings().name;
    const std::string fileName = m_DocumentPath.empty()
        ? SafeFileBase(baseName) + extension
        : BaseName(m_DocumentPath);
    const char* dialogTitle = m_DocumentKind == DocumentKind::Function
                                  ? Tr("Save Blueprint Function")
                                  : (m_DocumentKind == DocumentKind::Instance
                                         ? Tr("Save Material Instance")
                                         : Tr("Save Material Graph"));
    m_FileBrowser.Open(dialogTitle, FileBrowser::Mode::Save, start, fileName, {extension});
}

void Application::RequestExportDialog() {
    if (m_DocumentKind == DocumentKind::Function) {
        SetStatus(Tr("Blueprint function graphs cannot be exported as material JSON."), true);
        return;
    }
    if (m_DocumentKind == DocumentKind::Instance) {
        SetStatus(Tr("Material Instances cannot be exported directly; export the parent material."),
                  true);
        return;
    }
    m_FileAction = FileAction::ExportGraph;
    const fs::path start = m_DocumentPath.empty() ? DefaultSaveDirectory()
                                                  : m_DocumentPath.parent_path();
    const std::string fileName = SafeFileBase(m_Graph.Settings().name) + ".json";
    m_FileBrowser.Open(Tr("Export UE5 Material JSON"), FileBrowser::Mode::Save, start, fileName,
                       {".json"});
}

void Application::RequestTextureDialog(NodeId nodeId, const std::string& propertyKey) {
    Node* node = m_Graph.FindNode(nodeId);
    if (!node) return;
    m_FileAction = FileAction::SelectTexture;
    m_TextureTargetNode = nodeId;
    m_TextureTargetProperty = propertyKey;
    fs::path start = m_Project.IsOpen() ? m_Project.RootPath() / "assets" / "textures"
                                        : m_ContentRoot / "assets" / "textures";
    if (!PathUtil::Exists(start)) {
        start = m_Project.IsOpen() ? m_Project.RootPath() : m_ContentRoot;
    }
    m_FileBrowser.Open(m_Project.IsOpen() ? Tr("Select Project Texture") : Tr("Select Texture"),
                       FileBrowser::Mode::Open, start, "",
                       {".png", ".jpg", ".jpeg", ".tga", ".bmp"});
}

void Application::HandleFileBrowserResult(const std::string& selectedPath) {
    const FileAction action = m_FileAction;
    m_FileAction = FileAction::None;
    const fs::path chosen = PathUtil::FromUtf8(selectedPath);

    if (action == FileAction::OpenProject) {
        OpenProjectFile(selectedPath);
        return;
    }
    if (action == FileAction::AddProjectFile) {
        AddFileToProject(selectedPath);
        return;
    }
    if (action == FileAction::SelectProjectLocation) {
        m_NewProjectLocation = selectedPath;
        m_NewProjectError.clear();
        m_ReturnToNewProjectAfterBrowse = false;
        m_OpenNewProjectModal = true;
        m_NewProjectModalPending = true;
        return;
    }
    if (action == FileAction::InstallPlugin) {
        std::string installError;
        if (!m_PluginManager.Install(chosen, &installError)) {
            SetStatus(LocalizedErrorText(installError), true);
        } else {
            SetStatus(Tr("Plugin installed."));
        }
        return;
    }
    if (action == FileAction::SelectPluginCreateLocation) {
        m_PluginCreateLocation = selectedPath;
        m_PluginCreateError.clear();
        m_ReturnToPluginCreateAfterBrowse = false;
        m_OpenCreatePlugin = true;
        m_PluginCreateModalPending = true;
        return;
    }
    if (action == FileAction::MoveProjectEntryTo ||
        action == FileAction::CopyProjectEntryTo) {
        const std::vector<std::string> sources = m_BrowseProjectEntries;
        m_BrowseProjectEntries.clear();
        if (sources.empty()) {
            SetStatus(Tr("The source file no longer exists."), true);
            return;
        }
        std::error_code ec;
        if (!m_Project.Contains(chosen) || !fs::is_directory(chosen, ec)) {
            SetStatus(Tr("The destination folder is outside the project."), true);
            return;
        }
        std::error_code equivalentError;
        const bool selectingProjectRoot =
            fs::equivalent(chosen, m_Project.RootPath(), equivalentError) && !equivalentError;
        const std::string targetDirectory =
            selectingProjectRoot ? std::string() : m_Project.RelativePathOf(chosen);
        if (action == FileAction::MoveProjectEntryTo) {
            MoveProjectEntries(sources, targetDirectory);
        } else {
            CopyProjectEntries(sources, targetDirectory);
        }
        return;
    }

    if (action == FileAction::OpenGraph) {
        std::string error;
        if (LoadDocument(selectedPath, &error)) {
            m_StatusMessage = FormatLocalized("Loaded %s", BaseName(chosen).c_str());
            m_StatusError = false;
            m_StatusExpiresAt = glfwGetTime() + 5.0;
        } else {
            SetStatus(FormatLocalized("Open failed: %s", error.c_str()), true);
        }
        return;
    }

    if (action == FileAction::SaveGraphAs || action == FileAction::ExportGraph ||
        action == FileAction::SaveGraph) {
        std::string error;
        if (SaveDocument(selectedPath, true, &error)) {
            m_StatusMessage = action == FileAction::ExportGraph
                                  ? FormatLocalized("Exported UE5 material JSON: %s",
                                                    BaseName(chosen).c_str())
                                  : FormatLocalized("Saved %s", BaseName(chosen).c_str());
            m_StatusError = false;
            m_StatusExpiresAt = glfwGetTime() + 5.0;
            if (m_PendingCloseSaveAs && action != FileAction::ExportGraph) {
                const int pending = m_PendingCloseDocument;
                m_PendingCloseSaveAs = false;
                CloseDocumentNow(pending);
            }
        } else {
            SetStatus(FormatLocalized("Save failed: %s", error.c_str()), true);
            if (m_PendingCloseSaveAs) {
                m_PendingCloseSaveAs = false;
                m_OpenCloseDocumentModal = true;
            }
        }
        return;
    }

    if (action == FileAction::SelectPreviewMesh) {
        m_Settings.previewMeshPath = selectedPath;
        m_Settings.previewMeshMode = "Custom";
        ApplyPreviewSceneSettings();
        SaveUiSettings();
        if (m_ReturnToPreviewSceneAfterBrowse) {
            m_ReturnToPreviewSceneAfterBrowse = false;
            m_PreviewSceneModalPending = true;
        }
        return;
    }
    if (action == FileAction::ExportUeBatch) {
        std::string batchError;
        if (!WriteUeBatchManifest(PathUtil::FromUtf8(selectedPath), &batchError)) {
            SetStatus(LocalizedErrorText(batchError), true);
        } else {
            m_UeLastBatchPath = selectedPath;
            m_UeLastMessage = FormatLocalized("UE batch manifest exported: %s",
                                              selectedPath.c_str());
            SetStatus(m_UeLastMessage);
        }
        return;
    }
    if (action == FileAction::SelectPreviewHdri) {
        m_Settings.previewHdriPath = selectedPath;
        ApplyPreviewSceneSettings();
        SaveUiSettings();
        if (m_ReturnToPreviewSceneAfterBrowse) {
            m_ReturnToPreviewSceneAfterBrowse = false;
            m_PreviewSceneModalPending = true;
        }
        return;
    }
    if (action == FileAction::SelectTexture) {
        Node* node = m_Graph.FindNode(m_TextureTargetNode);
        if (!node) {
            SetStatus(Tr("The target node no longer exists."), true);
            return;
        }
        std::string portable;
        bool projectTexture = false;
        if (m_Project.IsOpen() && m_Project.Contains(chosen)) {
            portable = m_Project.RelativePathOf(chosen);
            if (!portable.empty() &&
                m_Project.AddResource(portable, "texture", true)) {
                projectTexture = true;
            }
        }
        if (portable.empty()) {
            portable = PathUtil::MakePortablePath(chosen, m_ContentRoot);
        }
        auto property = node->properties.find(m_TextureTargetProperty);
        if (property == node->properties.end()) {
            node->properties.emplace(m_TextureTargetProperty, PropertyValue::AssetPathValue(portable));
        } else {
            property->second = PropertyValue::AssetPathValue(portable);
        }
        m_Graph.MarkContentChanged();
        if (projectTexture) {
            std::string error;
            m_Project.RefreshIntegrity(&error);
            m_Project.Save(&error);
            m_ProjectEntriesDirty = true;
            RefreshProjectEntries();
        }
        SetStatus(FormatLocalized("Texture set to %s", portable.c_str()));
    }
}

void Application::SyncActiveDocument() {
    FlushUndoPending(true);
    if (m_ActiveDocument < 0 ||
        m_ActiveDocument >= static_cast<int>(m_Documents.size())) {
        return;
    }
    EditorDocument& document = m_Documents[static_cast<std::size_t>(m_ActiveDocument)];
    document.graph = m_Graph;
    document.instance = m_Instance;
    document.instanceParameters = m_InstanceParameters;
    document.path = m_DocumentPath;
    document.kind = m_DocumentKind;
    document.title = DocumentTitle(m_DocumentPath, m_Graph, m_DocumentKind == DocumentKind::Function);
}

Application::EditorDocument* Application::ActiveEditorDocument() {
    if (m_ActiveDocument < 0 ||
        m_ActiveDocument >= static_cast<int>(m_Documents.size())) {
        return nullptr;
    }
    return &m_Documents[static_cast<std::size_t>(m_ActiveDocument)];
}

const Application::EditorDocument* Application::ActiveEditorDocument() const {
    if (m_ActiveDocument < 0 ||
        m_ActiveDocument >= static_cast<int>(m_Documents.size())) {
        return nullptr;
    }
    return &m_Documents[static_cast<std::size_t>(m_ActiveDocument)];
}

void Application::InitializeUndoForDocument(EditorDocument& document) {
    document.undoStack.clear();
    document.redoStack.clear();
    document.undoBaseline = document.graph;
    document.pendingUndoBaseline = MaterialGraph{};
    document.undoContentRevision = document.graph.ContentRevision();
    document.undoLayoutRevision = document.graph.LayoutRevision();
    document.undoLastChangeTime = 0.0;
    document.undoInitialized = true;
    document.undoPending = false;
}

void Application::ResetUndoForActiveDocument() {
    if (EditorDocument* document = ActiveEditorDocument()) {
        InitializeUndoForDocument(*document);
    }
}

void Application::FlushUndoPending(bool force) {
    EditorDocument* document = ActiveEditorDocument();
    if (!document || !document->undoInitialized || !document->undoPending) {
        return;
    }
    if (!force) {
        const bool busy = ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
                          ImGui::IsAnyItemActive();
        if (busy || m_TimeSeconds - document->undoLastChangeTime < 0.15) {
            return;
        }
    }

    document->undoStack.push_back(std::move(document->pendingUndoBaseline));
    if (document->undoStack.size() > 64) {
        document->undoStack.erase(document->undoStack.begin());
    }
    document->undoBaseline = m_Graph;
    document->undoContentRevision = m_Graph.ContentRevision();
    document->undoLayoutRevision = m_Graph.LayoutRevision();
    document->undoPending = false;
    document->redoStack.clear();
}

void Application::UpdateUndoAndAutosave() {
    if (m_DocumentKind == DocumentKind::Instance) {
        return;
    }
    if (EditorDocument* document = ActiveEditorDocument()) {
        if (!document->undoInitialized) {
            InitializeUndoForDocument(*document);
        }
    }

    if (EditorDocument* document = ActiveEditorDocument()) {
        if (document->undoInitialized &&
            (m_Graph.ContentRevision() != document->undoContentRevision ||
             m_Graph.LayoutRevision() != document->undoLayoutRevision)) {
            if (!document->undoPending) {
                document->pendingUndoBaseline = document->undoBaseline;
                document->undoPending = true;
                document->redoStack.clear();
            }
            document->undoLastChangeTime = m_TimeSeconds;
        }
        FlushUndoPending(false);
    }

    const int autosaveInterval = std::clamp(m_Settings.autosaveIntervalSeconds, 2, 120);
    if (m_Settings.autosaveEnabled && m_CapturePath.empty() &&
        !m_AutosaveRecoveryPending && m_Options.dumpShaderPath.empty() &&
        m_Graph.IsDirty() && m_TimeSeconds - m_LastAutosaveTime >= autosaveInterval) {
        WriteAutosave();
    }
}

bool Application::CanUndo() const {
    if (m_DocumentKind == DocumentKind::Instance) return false;
    const EditorDocument* document = ActiveEditorDocument();
    return document && document->undoInitialized &&
           (document->undoPending || !document->undoStack.empty());
}

bool Application::CanRedo() const {
    if (m_DocumentKind == DocumentKind::Instance) return false;
    const EditorDocument* document = ActiveEditorDocument();
    return document && document->undoInitialized && !document->undoPending &&
           !document->redoStack.empty();
}

void Application::Undo() {
    FlushUndoPending(true);
    EditorDocument* document = ActiveEditorDocument();
    if (!document || document->undoStack.empty()) {
        return;
    }
    document->redoStack.push_back(m_Graph);
    m_Graph = std::move(document->undoStack.back());
    document->undoStack.pop_back();
    m_Graph.MarkContentChanged();
    document->undoBaseline = m_Graph;
    document->undoContentRevision = m_Graph.ContentRevision();
    document->undoLayoutRevision = m_Graph.LayoutRevision();
    document->undoPending = false;
    if (m_GraphView) {
        m_GraphView->OnGraphReplacedPreservingView();
    }
    m_LastContentRevision = 0;
    m_ForceShaderRefresh = true;
    SyncActiveDocument();
    UpdateWindowTitle();
    WriteAutosave(true);
    SetStatus(Tr("Undo"));
}

void Application::Redo() {
    FlushUndoPending(true);
    EditorDocument* document = ActiveEditorDocument();
    if (!document || document->redoStack.empty()) {
        return;
    }
    document->undoStack.push_back(m_Graph);
    if (document->undoStack.size() > 64) {
        document->undoStack.erase(document->undoStack.begin());
    }
    m_Graph = std::move(document->redoStack.back());
    document->redoStack.pop_back();
    m_Graph.MarkContentChanged();
    document->undoBaseline = m_Graph;
    document->undoContentRevision = m_Graph.ContentRevision();
    document->undoLayoutRevision = m_Graph.LayoutRevision();
    document->undoPending = false;
    if (m_GraphView) {
        m_GraphView->OnGraphReplacedPreservingView();
    }
    m_LastContentRevision = 0;
    m_ForceShaderRefresh = true;
    SyncActiveDocument();
    UpdateWindowTitle();
    WriteAutosave(true);
    SetStatus(Tr("Redo"));
}

std::filesystem::path Application::AutosaveDirectory() const {
    std::error_code ec;
    const fs::path preferred = PathUtil::UserConfigDirectory() / "autosave";
    fs::create_directories(preferred, ec);
    if (!ec) {
        return preferred;
    }
    const fs::path fallback =
        (m_ContentRoot.empty() ? fs::current_path() : m_ContentRoot) / ".materialforge" /
        "autosave";
    fs::create_directories(fallback, ec);
    return fallback;
}

std::filesystem::path Application::AutosaveDataPath() const {
    return AutosaveDirectory() / "active.json";
}

std::filesystem::path Application::AutosaveMetaPath() const {
    return AutosaveDirectory() / "active.meta.json";
}

void Application::DeleteAutosave() {
    std::error_code ec;
    fs::remove(AutosaveDataPath(), ec);
    fs::remove(AutosaveMetaPath(), ec);
}

void Application::WriteAutosave(bool force) {
    if (!m_Settings.autosaveEnabled || !m_CapturePath.empty() ||
        !m_Options.dumpShaderPath.empty() || m_AutosaveRecoveryPending) {
        return;
    }
    if (!force && !m_Graph.IsDirty()) {
        return;
    }
    const fs::path dataPath = AutosaveDataPath();
    const fs::path metaPath = AutosaveMetaPath();
    std::error_code ec;
    fs::create_directories(dataPath.parent_path(), ec);
    if (ec) {
        if (!m_AutosaveFailed) {
            LogMessage(LogLevel::Warning, "Could not create autosave directory: " + ec.message());
            m_AutosaveFailed = true;
        }
        return;
    }

    const fs::path temporaryData = dataPath.string() + ".tmp";
    std::string saveError;
    if (!GraphSerializer::Save(m_Graph, PathUtil::ToUtf8(temporaryData), &saveError)) {
        if (!m_AutosaveFailed) {
            LogMessage(LogLevel::Warning, "Could not write autosave: " + saveError);
            m_AutosaveFailed = true;
        }
        return;
    }
    fs::remove(dataPath, ec);
    fs::rename(temporaryData, dataPath, ec);
    if (ec) {
        if (!m_AutosaveFailed) {
            LogMessage(LogLevel::Warning, "Could not replace autosave: " + ec.message());
            m_AutosaveFailed = true;
        }
        return;
    }

    nlohmann::json meta;
    meta["originalPath"] = PathUtil::ToUtf8(m_DocumentPath);
    meta["kind"] = m_DocumentKind == DocumentKind::Function ? "function" : "material";
    meta["title"] = DocumentTitle(m_DocumentPath, m_Graph, m_DocumentKind == DocumentKind::Function);
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::ostringstream timestamp;
    timestamp << std::put_time(&local, "%Y-%m-%d %H:%M:%S");
    meta["timestamp"] = timestamp.str();

    const fs::path temporaryMeta = metaPath.string() + ".tmp";
    {
        std::ofstream stream(temporaryMeta, std::ios::binary | std::ios::trunc);
        if (!stream) {
            m_AutosaveFailed = true;
            return;
        }
        const std::string text = meta.dump(2);
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!stream) {
            m_AutosaveFailed = true;
            return;
        }
    }
    fs::remove(metaPath, ec);
    fs::rename(temporaryMeta, metaPath, ec);
    if (ec) {
        m_AutosaveFailed = true;
        return;
    }
    m_LastAutosaveTime = m_TimeSeconds;
    m_AutosaveFailed = false;
}

void Application::CheckAutosaveRecovery() {
    if (!m_Settings.autosaveEnabled || !m_CapturePath.empty() ||
        !m_Options.dumpShaderPath.empty() || m_AutosavePromptShown) {
        return;
    }
    std::error_code ec;
    if (!fs::is_regular_file(AutosaveDataPath(), ec) ||
        !fs::is_regular_file(AutosaveMetaPath(), ec)) {
        return;
    }
    std::ifstream stream(AutosaveMetaPath(), std::ios::binary);
    if (!stream) {
        return;
    }
    nlohmann::json meta;
    try {
        stream >> meta;
    } catch (...) {
        DeleteAutosave();
        return;
    }
    m_AutosaveOriginalPath = meta.value("originalPath", std::string());
    m_AutosaveKind = meta.value("kind", std::string("material"));
    m_AutosaveTimestamp = meta.value("timestamp", std::string());
    m_AutosaveRecoveryPending = true;
}

void Application::DrawAutosaveRecoveryModal() {
    if (!m_AutosaveRecoveryPending) {
        return;
    }
    const std::string popup = std::string(Tr("Recover Unsaved Work")) + "###autosave_recovery";
    if (!m_AutosavePromptShown) {
        m_AutosavePromptShown = true;
        ImGui::OpenPopup(popup.c_str());
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(Ui::S(480.0f), 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(popup.c_str(), nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize |
                                    ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }
    ImGui::TextWrapped("%s", Tr("MaterialForge found unsaved work from a previous session."));
    ImGui::Spacing();
    if (!m_AutosaveTimestamp.empty()) {
        ImGui::TextDisabled("%s: %s", Tr("Autosave time"), m_AutosaveTimestamp.c_str());
    }
    if (!m_AutosaveOriginalPath.empty()) {
        ImGui::TextDisabled("%s", m_AutosaveOriginalPath.c_str());
    }
    ImGui::Spacing();
    if (Ui::AccentButton(Tr("Recover"), ImVec2(Ui::S(110.0f), Ui::S(30.0f)))) {
        RecoverAutosave();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (Ui::GhostButton(Tr("Discard"), ImVec2(Ui::S(110.0f), Ui::S(30.0f)))) {
        DiscardAutosave();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void Application::RecoverAutosave() {
    std::ifstream metaStream(AutosaveMetaPath(), std::ios::binary);
    if (metaStream) {
        nlohmann::json meta;
        try {
            metaStream >> meta;
            m_AutosaveOriginalPath = meta.value("originalPath", m_AutosaveOriginalPath);
            m_AutosaveKind = meta.value("kind", m_AutosaveKind);
        } catch (...) {
        }
    }

    MaterialGraph recovered;
    std::string error;
    if (!GraphSerializer::Load(PathUtil::ToUtf8(AutosaveDataPath()), recovered, &error)) {
        SetStatus("Could not recover autosave: " + error, true);
        m_AutosaveRecoveryPending = false;
        m_AutosavePromptShown = false;
        return;
    }

    SyncActiveDocument();
    m_Graph = std::move(recovered);
    m_DocumentPath = PathUtil::FromUtf8(m_AutosaveOriginalPath);
    m_DocumentKind = m_AutosaveKind == "function" ? DocumentKind::Function
                                                  : DocumentKind::Material;
    RegisterLiveDocument(m_DocumentKind, m_DocumentPath);
    m_Graph.MarkContentChanged();
    if (m_GraphView) {
        m_GraphView->SetFunctionMode(m_DocumentKind == DocumentKind::Function);
        m_GraphView->OnGraphReplaced();
    }
    if (EditorDocument* document = ActiveEditorDocument()) {
        InitializeUndoForDocument(*document);
    }
    m_LastContentRevision = 0;
    m_ForceShaderRefresh = true;
    m_LastAutosaveTime = m_TimeSeconds;
    DeleteAutosave();
    m_AutosaveRecoveryPending = false;
    m_AutosavePromptShown = false;
    UpdateWindowTitle();
    SetStatus(Tr("Recovered unsaved material work."));
}

void Application::DiscardAutosave() {
    DeleteAutosave();
    m_AutosaveRecoveryPending = false;
    m_AutosavePromptShown = false;
    SetStatus(Tr("Discarded recovered autosave."));
}

void Application::RegisterLiveDocument(DocumentKind kind, const std::filesystem::path& path) {
    const std::string key = NormalizedPathKey(path);
    if (!key.empty()) {
        for (std::size_t i = 0; i < m_Documents.size(); ++i) {
            if (NormalizedPathKey(m_Documents[i].path) != key) continue;
            m_Documents[i].graph = m_Graph;
            m_Documents[i].instance = m_Instance;
            m_Documents[i].instanceParameters = m_InstanceParameters;
            m_Documents[i].path = path;
            m_Documents[i].kind = kind;
            m_Documents[i].title = DocumentTitle(path, m_Graph, kind == DocumentKind::Function);
            InitializeUndoForDocument(m_Documents[i]);
            m_ActiveDocument = static_cast<int>(i);
            m_DocumentKind = kind;
            m_DocumentPath = path;
            if (m_GraphView) m_GraphView->SetFunctionMode(kind == DocumentKind::Function);
            return;
        }
    }

    EditorDocument document;
    document.path = path;
    document.kind = kind;
    document.graph = m_Graph;
    document.instance = m_Instance;
    document.instanceParameters = m_InstanceParameters;
    document.title = DocumentTitle(path, m_Graph, kind == DocumentKind::Function);
    InitializeUndoForDocument(document);
    m_Documents.push_back(std::move(document));
    m_ActiveDocument = static_cast<int>(m_Documents.size()) - 1;
    m_DocumentKind = kind;
    m_DocumentPath = path;
    if (m_GraphView) m_GraphView->SetFunctionMode(kind == DocumentKind::Function);
}

void Application::ActivateDocument(int index) {
    if (index < 0 || index >= static_cast<int>(m_Documents.size())) return;
    if (index == m_ActiveDocument) return;
    StopNodePreview();
    FlushUndoPending(true);
    SyncActiveDocument();

    m_Graph = m_Documents[static_cast<std::size_t>(index)].graph;
    m_Instance = m_Documents[static_cast<std::size_t>(index)].instance;
    m_InstanceParameters = m_Documents[static_cast<std::size_t>(index)].instanceParameters;
    m_DocumentPath = m_Documents[static_cast<std::size_t>(index)].path;
    m_DocumentKind = m_Documents[static_cast<std::size_t>(index)].kind;
    m_ActiveDocument = index;
    if (!m_Documents[static_cast<std::size_t>(index)].undoInitialized) {
        InitializeUndoForDocument(m_Documents[static_cast<std::size_t>(index)]);
    }
    if (m_GraphView) {
        m_GraphView->SetFunctionMode(m_DocumentKind == DocumentKind::Function);
        m_GraphView->OnGraphReplaced();
    }
    m_LastContentRevision = 0;
    m_ForceShaderRefresh = true;
    m_FocusShaderTab = false;
    m_FocusInstanceTab = m_DocumentKind == DocumentKind::Instance;
    UpdateWindowTitle();
}

void Application::RequestCloseDocument(int index) {
    if (index < 0 || index >= static_cast<int>(m_Documents.size())) return;
    FlushUndoPending(true);
    SyncActiveDocument();
    const EditorDocument& document = m_Documents[static_cast<std::size_t>(index)];
    if (!document.graph.IsDirty()) {
        CloseDocumentNow(index);
        return;
    }
    m_PendingCloseDocument = index;
    m_OpenCloseDocumentModal = true;
}

void Application::CloseDocumentNow(int index) {
    if (index < 0 || index >= static_cast<int>(m_Documents.size())) {
        m_PendingCloseDocument = -1;
        return;
    }

    const bool closingActive = index == m_ActiveDocument;
    if (closingActive) StopNodePreview();
    m_Documents.erase(m_Documents.begin() + index);
    m_PendingCloseDocument = -1;
    m_OpenCloseDocumentModal = false;

    if (m_Documents.empty()) {
        // Closing the last document leaves an intentionally empty workspace.
        // Do not synthesize a starter material or register an untitled tab.
        m_Graph.Clear();
        m_Graph.Settings().name.clear();
        m_Instance = MaterialInstance{};
        m_InstanceParameters.clear();
        m_DocumentPath.clear();
        m_DocumentKind = DocumentKind::Material;
        m_ActiveDocument = -1;
        if (m_GraphView) {
            m_GraphView->SetFunctionMode(false);
            m_GraphView->OnGraphReplaced();
        }
        m_LastContentRevision = 0;
        m_ForceShaderRefresh = true;
        UpdateWindowTitle();
        return;
    }

    if (closingActive) {
        const int next = std::min(index, static_cast<int>(m_Documents.size()) - 1);
        m_Graph = m_Documents[static_cast<std::size_t>(next)].graph;
        m_Instance = m_Documents[static_cast<std::size_t>(next)].instance;
        m_InstanceParameters = m_Documents[static_cast<std::size_t>(next)].instanceParameters;
        m_DocumentPath = m_Documents[static_cast<std::size_t>(next)].path;
        m_DocumentKind = m_Documents[static_cast<std::size_t>(next)].kind;
        m_ActiveDocument = next;
        if (m_GraphView) {
            m_GraphView->SetFunctionMode(m_DocumentKind == DocumentKind::Function);
            m_GraphView->OnGraphReplaced();
        }
        m_LastContentRevision = 0;
        m_ForceShaderRefresh = true;
    } else if (index < m_ActiveDocument) {
        --m_ActiveDocument;
    }
    UpdateWindowTitle();
}

bool Application::LoadDocument(const std::string& path, std::string* error) {
    StopNodePreview();
    const fs::path requested = PathUtil::FromUtf8(path);
    fs::path resolved = requested;
    if (requested.is_relative()) {
        resolved = PathUtil::ResolveResource(path);
    }

    if (ToLower(PathUtil::ToUtf8(resolved.extension())) == ".mfinstance") {
        OpenMaterialInstance(resolved);
        if (error) error->clear();
        return true;
    }

    const std::string key = NormalizedPathKey(resolved);
    for (std::size_t i = 0; i < m_Documents.size(); ++i) {
        if (NormalizedPathKey(m_Documents[i].path) == key) {
            ActivateDocument(static_cast<int>(i));
            if (error) error->clear();
            return true;
        }
    }

    FlushUndoPending(true);
    SyncActiveDocument();
    std::string loadError;
    if (!GraphSerializer::Load(PathUtil::ToUtf8(resolved), m_Graph, &loadError)) {
        if (error) *error = loadError;
        return false;
    }
    m_DocumentPath = resolved;
    m_Instance = MaterialInstance{};
    m_InstanceParameters.clear();
    const std::string extension = ToLower(PathUtil::ToUtf8(resolved.extension()));
    m_DocumentKind = extension == ".mffunction" ? DocumentKind::Function
                                                 : DocumentKind::Material;
    if (m_GraphView) {
        m_GraphView->SetFunctionMode(m_DocumentKind == DocumentKind::Function);
        m_GraphView->OnGraphReplaced();
    }
    RegisterLiveDocument(m_DocumentKind, resolved);
    m_LastContentRevision = 0;
    m_ForceShaderRefresh = true;
    m_FocusShaderTab = false;
    m_FocusInstanceTab = false;
    if (error) error->clear();
    UpdateWindowTitle();
    return true;
}

bool Application::SaveDocument(const std::string& path, bool makeCurrentDocument, std::string* error) {
    FlushUndoPending(true);
    std::string saveError;
    if (m_DocumentKind == DocumentKind::Instance) {
        if (!m_Instance.Save(PathUtil::FromUtf8(path), &saveError)) {
            if (error) *error = saveError;
            return false;
        }
    } else if (!GraphSerializer::Save(m_Graph, path, &saveError)) {
        if (error) *error = saveError;
        return false;
    }
    if (makeCurrentDocument) {
        std::error_code pathError;
        const fs::path savedPath = fs::absolute(PathUtil::FromUtf8(path), pathError);
        m_DocumentPath = pathError ? PathUtil::FromUtf8(path) : savedPath.lexically_normal();
        if (m_DocumentKind != DocumentKind::Instance) {
            UpdateProjectMainMaterialAfterSave(m_DocumentPath);
        }
    }
    m_Graph.MarkSaved();
    for (auto& [key, entry] : m_MaterialThumbnails) {
        (void)key;
        if (entry.texture != 0) glDeleteTextures(1, &entry.texture);
    }
    m_MaterialThumbnails.clear();
    m_GraphThumbnails.clear();
    m_ProjectEntriesDirty = true;
    SyncActiveDocument();
    DeleteAutosave();
    if (m_DocumentKind == DocumentKind::Function) {
        m_FunctionAssetsDirty = true;
    }
    if (error) error->clear();
    UpdateWindowTitle();
    return true;
}

void Application::SetStatus(const std::string& message, bool error) {
    m_StatusMessage = message;
    m_StatusError = error;
    m_StatusExpiresAt = glfwGetTime() + 6.0;
    LogMessage(error ? LogLevel::Error : LogLevel::Info, message);
}

void Application::LogMessage(LogLevel level, const std::string& message) {
    if (message.empty()) {
        return;
    }
    if (!m_ConsoleEntries.empty()) {
        const ConsoleEntry& last = m_ConsoleEntries.back();
        if (last.level == level && last.message == message) {
            m_ConsoleScrollToBottom = true;
            return;
        }
    }
    ConsoleEntry entry;
    entry.time = m_TimeSeconds;
    entry.level = level;
    entry.message = message;
    m_ConsoleEntries.push_back(std::move(entry));
    if (m_ConsoleEntries.size() > kMaxConsoleEntries) {
        m_ConsoleEntries.erase(m_ConsoleEntries.begin(),
                               m_ConsoleEntries.begin() +
                                   static_cast<std::ptrdiff_t>(m_ConsoleEntries.size() -
                                                               kMaxConsoleEntries));
    }
    m_ConsoleScrollToBottom = true;
    if (level == LogLevel::Error) {
        std::cerr << "[error] " << message << "\n";
    } else if (level == LogLevel::Warning) {
        std::cerr << "[warning] " << message << "\n";
    }
}

void Application::ClearConsole() {
    m_ConsoleEntries.clear();
    m_ConsoleScrollToBottom = true;
    m_ConsoleFilter = 0;
}

void Application::SetConsoleVisible(bool visible) {
    m_ConsoleVisible = visible;
    m_ConsoleFocusInput = visible;
    m_Settings.consoleVisible = visible;
    SaveUiSettings();
}

void Application::SetNodeLibraryCollapsed(bool collapsed) {
    m_NodeLibraryCollapsed = collapsed;
    m_Settings.nodeLibraryCollapsed = collapsed;
    SaveUiSettings();
}

void Application::DrainExternalMessages() {
    if (g_GlfwErrors.empty()) {
        return;
    }
    std::vector<std::string> messages = std::move(g_GlfwErrors);
    g_GlfwErrors.clear();
    for (const std::string& message : messages) {
        LogMessage(LogLevel::Error, message);
    }
}

void Application::HandleShortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || m_FileBrowser.IsActive()) {
        return;
    }
    if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        return;
    }
    if (io.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
            if (io.KeyShift) {
                Redo();
            } else {
                Undo();
            }
            return;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
            Redo();
            return;
        }
        if (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
            RequestSave(true);
            return;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_S, false)) {
            SaveProject();
            return;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_O, false)) {
            RequestOpenDialog();
            return;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_N, false)) {
            RequestNewDocument();
            return;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_E, false)) {
            RequestExportDialog();
            return;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_L, false)) {
            SetNodeLibraryCollapsed(!m_NodeLibraryCollapsed);
            return;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_GraveAccent, false)) {
            SetConsoleVisible(!m_ConsoleVisible);
            return;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Comma, false)) {
            m_OpenPreferences = true;
            return;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_W, false)) {
            if (m_ActiveDocument >= 0) RequestCloseDocument(m_ActiveDocument);
            return;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
            if (m_Documents.size() > 1) {
                const int count = static_cast<int>(m_Documents.size());
                const int step = io.KeyShift ? count - 1 : 1;
                ActivateDocument((m_ActiveDocument + step) % count);
            }
            return;
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) {
        m_GraphView->FrameAll();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
        m_GraphView->FrameSelection();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_R, false)) {
        m_PreviewRenderer.Camera().Reset();
        m_PreviewNeedsRender = true;
    }
}

void Application::UpdateWindowTitle() {
    std::string title = "MaterialForge - ";
    if (m_Project.IsOpen()) {
        title += m_Project.Name();
        if (m_Project.IsBroken()) {
            title += " [" + std::string(Tr("broken")) + "]";
        }
        title += " / ";
    }
    if (m_DocumentKind == DocumentKind::Instance) {
        const std::string name = m_Instance.Name().empty()
                                     ? PathUtil::ToUtf8(m_DocumentPath.stem())
                                     : m_Instance.Name();
        title += name.empty() ? std::string(Tr("Material Instance")) : name;
    } else {
        title += m_Graph.Settings().name.empty() ? Tr("Untitled Material")
                                                 : m_Graph.Settings().name;
    }
    if (m_Graph.IsDirty()) title += " *";
    if (m_Window) {
        static std::string lastTitle;
        if (title != lastTitle) {
            glfwSetWindowTitle(m_Window, title.c_str());
            lastTitle = title;
        }
    }
}

std::filesystem::path Application::DefaultSaveDirectory() const {
    if (!m_DocumentPath.empty() && m_DocumentPath.has_parent_path()) {
        return m_DocumentPath.parent_path();
    }
    const fs::path graphs = m_ContentRoot / "examples" / "graphs";
    std::error_code ec;
    if (fs::is_directory(graphs, ec)) {
        return graphs;
    }
    return m_ContentRoot.empty() ? PathUtil::CurrentDirectory() : m_ContentRoot;
}

bool Application::CaptureScreenshot(const std::string& path) {
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(m_Window, &width, &height);
    if (width <= 0 || height <= 0) {
        std::cerr << "Screenshot failed: window framebuffer is empty.\n";
        return false;
    }

    std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    std::vector<unsigned char> flipped(pixels.size());
    const std::size_t rowBytes = static_cast<std::size_t>(width) * 4;
    for (int y = 0; y < height; ++y) {
        std::copy_n(pixels.begin() + static_cast<std::ptrdiff_t>((height - 1 - y) * rowBytes),
                    rowBytes, flipped.begin() + static_cast<std::ptrdiff_t>(y * rowBytes));
    }

    std::vector<unsigned char> png;
    if (!stbi_write_png_to_func(WriteToVector, &png, width, height, 4, flipped.data(), width * 4)) {
        std::cerr << "Screenshot failed: PNG encoding failed.\n";
        return false;
    }

    const fs::path output = PathUtil::FromUtf8(path);
    std::error_code ec;
    if (output.has_parent_path()) {
        fs::create_directories(output.parent_path(), ec);
    }
    std::ofstream stream(output, std::ios::binary);
    if (!stream) {
        std::cerr << "Screenshot failed: cannot open " << path << "\n";
        return false;
    }
    stream.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    if (!stream) {
        std::cerr << "Screenshot failed: could not write " << path << "\n";
        return false;
    }
    std::cout << "Screenshot written to " << PathUtil::ToUtf8(output) << " (" << width << "x"
              << height << ")\n";
    return true;
}

} // namespace mf


