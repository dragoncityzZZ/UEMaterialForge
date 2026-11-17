#pragma once

#include "core/Types.h"

#include <imgui.h>

#include <string>
#include <string_view>

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
