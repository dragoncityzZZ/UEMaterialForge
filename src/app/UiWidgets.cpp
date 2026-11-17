#include "app/UiWidgets.h"

#include <algorithm>

namespace mf::Ui {

namespace {
Theme g_Theme = Theme::Graphite;
float g_Scale = 1.0f;
}

void SetTheme(Theme theme) {
    g_Theme = theme;
}

Theme CurrentTheme() {
    return g_Theme;
}

void SetScale(float scale) {
    g_Scale = std::clamp(scale, 0.75f, 2.5f);
}

float Scale() {
    return g_Scale;
}

const char* ThemeName(Theme theme) {
    switch (theme) {
    case Theme::Graphite: return "graphite";
    case Theme::Midnight: return "midnight";
    }
    return "graphite";
}

void ApplyStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(14.0f, 12.0f);
    style.FramePadding = ImVec2(10.0f, 6.0f);
    style.CellPadding = ImVec2(8.0f, 5.0f);
    style.ItemSpacing = ImVec2(8.0f, 7.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 5.0f);
    style.IndentSpacing = 18.0f;
    style.ScrollbarSize = 12.0f;
    style.GrabMinSize = 10.0f;

    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.TabBorderSize = 0.0f;

    style.WindowRounding = 8.0f;
    style.ChildRounding = 8.0f;
    style.FrameRounding = 5.0f;
    style.PopupRounding = 8.0f;
    style.ScrollbarRounding = 10.0f;
    style.GrabRounding = 5.0f;
    style.TabRounding = 5.0f;

    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    style.ButtonTextAlign = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign = ImVec2(0.0f, 0.5f);

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = ImVec4(0.84f, 0.89f, 0.91f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.42f, 0.51f, 0.55f, 1.00f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.043f, 0.059f, 0.075f, 1.00f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.055f, 0.076f, 0.094f, 1.00f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.055f, 0.076f, 0.094f, 0.98f);
    colors[ImGuiCol_Border] = ImVec4(0.13f, 0.19f, 0.22f, 1.00f);
    colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.075f, 0.105f, 0.13f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.10f, 0.15f, 0.18f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.12f, 0.19f, 0.22f, 1.00f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.035f, 0.047f, 0.059f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.05f, 0.07f, 0.085f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.035f, 0.047f, 0.059f, 0.75f);
    colors[ImGuiCol_MenuBarBg] = ImVec4(0.035f, 0.047f, 0.059f, 1.00f);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.035f, 0.047f, 0.059f, 1.00f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.18f, 0.25f, 0.28f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.25f, 0.34f, 0.37f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.30f, 0.43f, 0.45f, 1.00f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.21f, 0.82f, 0.75f, 1.00f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.21f, 0.82f, 0.75f, 1.00f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.30f, 0.94f, 0.84f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.075f, 0.115f, 0.135f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.11f, 0.17f, 0.19f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.14f, 0.23f, 0.25f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.09f, 0.14f, 0.16f, 1.00f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.13f, 0.20f, 0.22f, 1.00f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.16f, 0.26f, 0.28f, 1.00f);
    colors[ImGuiCol_Separator] = ImVec4(0.12f, 0.18f, 0.21f, 1.00f);
    colors[ImGuiCol_SeparatorHovered] = ImVec4(0.24f, 0.55f, 0.52f, 1.00f);
    colors[ImGuiCol_SeparatorActive] = ImVec4(0.29f, 0.73f, 0.67f, 1.00f);
    colors[ImGuiCol_ResizeGrip] = ImVec4(0.18f, 0.30f, 0.32f, 0.50f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.29f, 0.73f, 0.67f, 0.67f);
    colors[ImGuiCol_ResizeGripActive] = ImVec4(0.35f, 0.88f, 0.79f, 0.90f);
    colors[ImGuiCol_Tab] = ImVec4(0.06f, 0.09f, 0.11f, 1.00f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.13f, 0.24f, 0.25f, 1.00f);
    colors[ImGuiCol_TabSelected] = ImVec4(0.10f, 0.20f, 0.21f, 1.00f);
    colors[ImGuiCol_TabSelectedOverline] = ImVec4(0.28f, 0.82f, 0.75f, 1.00f);
    colors[ImGuiCol_TabDimmed] = ImVec4(0.05f, 0.075f, 0.09f, 1.00f);
    colors[ImGuiCol_TabDimmedSelected] = ImVec4(0.08f, 0.13f, 0.15f, 1.00f);
    colors[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.20f, 0.40f, 0.40f, 1.00f);
    colors[ImGuiCol_PlotLines] = ImVec4(0.63f, 0.78f, 0.83f, 1.00f);
    colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.00f, 0.66f, 0.30f, 1.00f);
    colors[ImGuiCol_PlotHistogram] = ImVec4(0.27f, 0.78f, 0.71f, 1.00f);
    colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1.00f, 0.66f, 0.30f, 1.00f);
    colors[ImGuiCol_TableHeaderBg] = ImVec4(0.07f, 0.10f, 0.12f, 1.00f);
    colors[ImGuiCol_TableBorderStrong] = ImVec4(0.13f, 0.19f, 0.22f, 1.00f);
    colors[ImGuiCol_TableBorderLight] = ImVec4(0.09f, 0.13f, 0.16f, 1.00f);
    colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.015f);
    colors[ImGuiCol_TextSelectedBg] = ImVec4(0.20f, 0.60f, 0.56f, 0.38f);
    colors[ImGuiCol_DragDropTarget] = ImVec4(0.98f, 0.71f, 0.29f, 0.90f);
    colors[ImGuiCol_NavCursor] = ImVec4(0.21f, 0.82f, 0.75f, 1.00f);
    colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.0f, 1.0f, 1.0f, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.35f);
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.01f, 0.02f, 0.03f, 0.62f);

    if (g_Theme == Theme::Graphite) {
        colors[ImGuiCol_Text] = ImVec4(0.90f, 0.91f, 0.92f, 1.00f);
        colors[ImGuiCol_TextDisabled] = ImVec4(0.56f, 0.59f, 0.62f, 1.00f);
        colors[ImGuiCol_WindowBg] = ImVec4(0.125f, 0.135f, 0.145f, 1.00f);
        colors[ImGuiCol_ChildBg] = ImVec4(0.155f, 0.165f, 0.175f, 1.00f);
        colors[ImGuiCol_PopupBg] = ImVec4(0.165f, 0.175f, 0.185f, 0.99f);
        colors[ImGuiCol_Border] = ImVec4(0.26f, 0.28f, 0.30f, 1.00f);
        colors[ImGuiCol_FrameBg] = ImVec4(0.195f, 0.210f, 0.225f, 1.00f);
        colors[ImGuiCol_FrameBgHovered] = ImVec4(0.235f, 0.255f, 0.270f, 1.00f);
        colors[ImGuiCol_FrameBgActive] = ImVec4(0.275f, 0.300f, 0.320f, 1.00f);
        colors[ImGuiCol_TitleBg] = ImVec4(0.115f, 0.125f, 0.135f, 1.00f);
        colors[ImGuiCol_TitleBgActive] = ImVec4(0.145f, 0.155f, 0.165f, 1.00f);
        colors[ImGuiCol_MenuBarBg] = ImVec4(0.115f, 0.125f, 0.135f, 1.00f);
        colors[ImGuiCol_ScrollbarBg] = ImVec4(0.115f, 0.125f, 0.135f, 1.00f);
        colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.30f, 0.32f, 0.34f, 1.00f);
        colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.38f, 0.40f, 0.42f, 1.00f);
        colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.46f, 0.48f, 0.50f, 1.00f);
        colors[ImGuiCol_Button] = ImVec4(0.215f, 0.230f, 0.245f, 1.00f);
        colors[ImGuiCol_ButtonHovered] = ImVec4(0.275f, 0.295f, 0.315f, 1.00f);
        colors[ImGuiCol_ButtonActive] = ImVec4(0.32f, 0.34f, 0.36f, 1.00f);
        colors[ImGuiCol_Header] = ImVec4(0.225f, 0.245f, 0.260f, 1.00f);
        colors[ImGuiCol_HeaderHovered] = ImVec4(0.285f, 0.305f, 0.325f, 1.00f);
        colors[ImGuiCol_HeaderActive] = ImVec4(0.33f, 0.35f, 0.37f, 1.00f);
        colors[ImGuiCol_Separator] = ImVec4(0.24f, 0.26f, 0.28f, 1.00f);
        colors[ImGuiCol_Tab] = ImVec4(0.165f, 0.175f, 0.185f, 1.00f);
        colors[ImGuiCol_TabHovered] = ImVec4(0.255f, 0.275f, 0.295f, 1.00f);
        colors[ImGuiCol_TabSelected] = ImVec4(0.225f, 0.245f, 0.260f, 1.00f);
        colors[ImGuiCol_TabDimmed] = ImVec4(0.145f, 0.155f, 0.165f, 1.00f);
        colors[ImGuiCol_TabDimmedSelected] = ImVec4(0.195f, 0.210f, 0.225f, 1.00f);
        colors[ImGuiCol_TableHeaderBg] = ImVec4(0.195f, 0.210f, 0.225f, 1.00f);
        colors[ImGuiCol_TableBorderStrong] = ImVec4(0.28f, 0.30f, 0.32f, 1.00f);
        colors[ImGuiCol_TableBorderLight] = ImVec4(0.22f, 0.24f, 0.26f, 1.00f);
        colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.04f, 0.045f, 0.05f, 0.58f);
    }
    style.ScaleAllSizes(g_Scale);
}

ImVec4 CategoryColor(std::string_view category) {
    if (category == "Output") return ImVec4(1.00f, 0.47f, 0.32f, 1.0f);
    if (category == "Constants") return ImVec4(0.31f, 0.84f, 0.78f, 1.0f);
    if (category == "Parameters") return ImVec4(0.35f, 0.70f, 1.00f, 1.0f);
    if (category == "Math") return ImVec4(0.53f, 0.67f, 1.00f, 1.0f);
    if (category == "Coordinates") return ImVec4(0.70f, 0.56f, 1.00f, 1.0f);
    if (category == "Textures") return ImVec4(1.00f, 0.67f, 0.32f, 1.0f);
    if (category == "Utility") return ImVec4(0.55f, 0.83f, 0.67f, 1.0f);
    if (category == "Functions") return ImVec4(0.56f, 0.72f, 1.00f, 1.0f);
    return ImVec4(0.62f, 0.69f, 0.72f, 1.0f);
}

ImVec4 PinTypeColor(PinType type, bool connected) {
    ImVec4 color;
    switch (type) {
    case PinType::Float: color = ImVec4(0.72f, 0.87f, 0.92f, 1.0f); break;
    case PinType::Vector2: color = ImVec4(0.47f, 0.88f, 0.56f, 1.0f); break;
    case PinType::Vector3: color = ImVec4(0.97f, 0.78f, 0.32f, 1.0f); break;
    case PinType::Vector4: color = ImVec4(0.95f, 0.49f, 0.55f, 1.0f); break;
    case PinType::Texture: color = ImVec4(1.00f, 0.62f, 0.30f, 1.0f); break;
    case PinType::Sampler: color = ImVec4(0.72f, 0.56f, 0.98f, 1.0f); break;
    case PinType::Wildcard: color = ImVec4(0.60f, 0.67f, 0.72f, 1.0f); break;
    }
    color.w = connected ? 1.0f : 0.62f;
    return color;
}

ImU32 ToU32(const ImVec4& color) {
    return ImGui::ColorConvertFloat4ToU32(color);
}

ImU32 CategoryU32(std::string_view category) {
    return ToU32(CategoryColor(category));
}

ImU32 PinTypeU32(PinType type, bool connected) {
    return ToU32(PinTypeColor(type, connected));
}

const char* PinTypeShortName(PinType type) {
    switch (type) {
    case PinType::Float: return "Float";
    case PinType::Vector2: return "Vector2";
    case PinType::Vector3: return "Vector3";
    case PinType::Vector4: return "Vector4";
    case PinType::Texture: return "Texture";
    case PinType::Sampler: return "Sampler";
    case PinType::Wildcard: return "Wildcard";
    }
    return "Unknown";
}

void DrawPinDot(PinType type, bool connected) {
    const float lineHeight = ImGui::GetTextLineHeight();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float radius = std::max(3.5f, lineHeight * 0.22f);
    const ImVec2 center(start.x + lineHeight * 0.5f, start.y + lineHeight * 0.5f);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImU32 color = PinTypeU32(type, connected);
    drawList->AddCircleFilled(center, radius, color, 16);
    if (!connected) {
        drawList->AddCircleFilled(center, radius * 0.44f, IM_COL32(13, 19, 25, 255), 12);
    } else {
        drawList->AddCircle(center, radius + 1.5f, IM_COL32(255, 255, 255, 40), 16, 1.0f);
    }
    ImGui::Dummy(ImVec2(lineHeight, lineHeight));
}

void DrawBrandMark(ImVec2 position, float size) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 max(position.x + size, position.y + size);
    drawList->AddRectFilled(position, max, IM_COL32(25, 162, 150, 255), 8.0f);
    drawList->AddRectFilled(position, ImVec2(max.x, position.y + size * 0.48f),
                            IM_COL32(255, 255, 255, 16), 8.0f, ImDrawFlags_RoundCornersTop);
    drawList->AddRect(position, max, IM_COL32(255, 255, 255, 38), 8.0f, 0, 1.0f);

    const ImVec2 a(position.x + size * 0.28f, position.y + size * 0.67f);
    const ImVec2 b(position.x + size * 0.50f, position.y + size * 0.33f);
    const ImVec2 c(position.x + size * 0.73f, position.y + size * 0.60f);
    drawList->AddLine(a, b, IM_COL32(255, 255, 255, 195), 1.8f);
    drawList->AddLine(b, c, IM_COL32(255, 255, 255, 195), 1.8f);
    drawList->AddCircleFilled(a, size * 0.075f, IM_COL32(255, 255, 255, 230), 12);
    drawList->AddCircleFilled(b, size * 0.085f, IM_COL32(255, 255, 255, 245), 12);
    drawList->AddCircleFilled(c, size * 0.075f, IM_COL32(255, 255, 255, 230), 12);
}

void HelpMarker(const char* description) {
    if (!description || description[0] == '\0') {
        return;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.04f, 0.06f, 0.08f, 1.0f));
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
        ImGui::TextUnformatted(description);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
        ImGui::PopStyleColor();
    }
}

void SectionLabel(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.47f, 0.60f, 0.64f, 1.0f));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

void Badge(const char* text, const ImVec4& color) {
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 padding(7.0f, 3.0f);
    const ImVec2 end(start.x + textSize.x + padding.x * 2.0f, start.y + textSize.y + padding.y * 2.0f);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec4 background = color;
    background.w *= 0.18f;
    drawList->AddRectFilled(start, end, ToU32(background), 4.0f);
    drawList->AddRect(start, end, ToU32(ImVec4(color.x, color.y, color.z, 0.45f)), 4.0f);
    drawList->AddText(ImVec2(start.x + padding.x, start.y + padding.y), ToU32(color), text);
    ImGui::Dummy(ImVec2(textSize.x + padding.x * 2.0f, textSize.y + padding.y * 2.0f));
}

bool AccentButton(const char* label, const ImVec2& size) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.11f, 0.49f, 0.46f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.15f, 0.63f, 0.58f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.09f, 0.42f, 0.40f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.94f, 1.0f, 0.99f, 1.0f));
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return pressed;
}

bool GhostButton(const char* label, const ImVec2& size) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.12f, 0.20f, 0.22f, 0.65f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.25f, 0.27f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.63f, 0.73f, 0.76f, 1.0f));
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return pressed;
}

bool ToggleButton(const char* label, bool active, const ImVec2& size) {
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.10f, 0.38f, 0.36f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.14f, 0.50f, 0.47f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.09f, 0.33f, 0.32f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.91f, 1.0f, 0.98f, 1.0f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.06f, 0.09f, 0.11f, 0.6f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.10f, 0.16f, 0.18f, 0.9f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.12f, 0.20f, 0.22f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.57f, 0.68f, 0.71f, 1.0f));
    }
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return pressed;
}

void PushMutedText() {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
}

void PopMutedText() {
    ImGui::PopStyleColor();
}

} // namespace mf::Ui
