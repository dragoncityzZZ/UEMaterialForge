#pragma once

#include "core/Types.h"

#include <imgui.h>

#include <string>
#include <string_view>
#include <unordered_map>

namespace mf::Ui {

enum class Theme {
    Graphite,
    Midnight
};

void ApplyStyle();
void SetTheme(Theme theme);
Theme CurrentTheme();
void SetScale(float scale);
float Scale();
inline float S(float value) { return value * Scale(); }
const char* ThemeName(Theme theme);

ImVec4 CategoryColor(std::string_view category);
ImVec4 PinTypeColor(PinType type, bool connected = true);
ImU32 ToU32(const ImVec4& color);
ImU32 CategoryU32(std::string_view category);
ImU32 PinTypeU32(PinType type, bool connected = true);

// Plugin-controllable visual overrides. Colors are 0xAABBGGRR (ImU32).
// A node color key can be either a node type ("Constant3Vector") or a category
// ("Textures"). Type overrides take precedence over category overrides.
void SetPluginTheme(std::string_view name);
bool SetPluginStyleColor(std::string_view key, ImU32 color);
bool ClearPluginStyleColor(std::string_view key);
void SetPluginNodeColor(std::string_view key, ImU32 color);
void ClearPluginNodeColor(std::string_view key);
void SetPluginLinkStyle(ImU32 color, float thickness, int shape);
void ClearPluginLinkStyle();
bool SetPluginStyleFloat(std::string_view key, float value);
void SetPluginNodeStyle(float rounding, float border, float pinRounding, float pinBorder);
bool HasPluginNodeStyle();
float PluginNodeRounding();
float PluginNodeBorder();
float PluginPinRounding();
float PluginPinBorder();
void SetPluginCanvasStyle(ImU32 background, ImU32 grid, ImU32 nodeBackground,
                          ImU32 nodeBorder, ImU32 selection);
bool HasPluginCanvasStyle();
ImU32 PluginCanvasBackground();
ImU32 PluginCanvasGrid();
ImU32 PluginCanvasNodeBackground();
ImU32 PluginCanvasNodeBorder();
ImU32 PluginCanvasSelection();
ImVec4 NodeAccentColor(std::string_view nodeType, std::string_view category);
bool HasPluginLinkStyle();
ImU32 PluginLinkColor(ImU32 fallback);
float PluginLinkThickness();
int PluginLinkShape();

const char* PinTypeShortName(PinType type);

void DrawPinDot(PinType type, bool connected);
void DrawBrandMark(ImVec2 position, float size);
void HelpMarker(const char* description);
void SectionLabel(const char* text);
void Badge(const char* text, const ImVec4& color);

bool AccentButton(const char* label, const ImVec2& size = ImVec2(0.0f, 0.0f));
bool GhostButton(const char* label, const ImVec2& size = ImVec2(0.0f, 0.0f));
bool ToggleButton(const char* label, bool active, const ImVec2& size = ImVec2(0.0f, 0.0f));

void PushMutedText();
void PopMutedText();

} // namespace mf::Ui
