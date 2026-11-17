#pragma once

#include "core/Node.h"
#include "app/UiWidgets.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace mf {
namespace Ui {

inline void DrawCheckboard(ImDrawList* drawList, ImVec2 min, float size, ImU32 a,
                           ImU32 b) {
    const float half = size * 0.5f;
    drawList->AddRectFilled(min, ImVec2(min.x + half, min.y + half), a);
    drawList->AddRectFilled(ImVec2(min.x + half, min.y), ImVec2(min.x + size, min.y + half), b);
    drawList->AddRectFilled(ImVec2(min.x, min.y + half), ImVec2(min.x + half, min.y + size), b);
    drawList->AddRectFilled(ImVec2(min.x + half, min.y + half), ImVec2(min.x + size, min.y + size), a);
}

// A compact procedural thumbnail used by the node library and node headers.
// Keeping these vector-drawn avoids a texture atlas and stays crisp at any UI
// scale.
inline void DrawNodeThumbnail(ImDrawList* drawList, ImVec2 min, float size,
                              const std::string& type, const std::string& category) {
    if (!drawList || size <= 0.0f) return;
    const ImVec4 accent = CategoryColor(category);
    const ImU32 background = IM_COL32(
        static_cast<int>(26.0f + accent.x * 68.0f),
        static_cast<int>(32.0f + accent.y * 68.0f),
        static_cast<int>(36.0f + accent.z * 68.0f), 255);
    const ImVec2 max(min.x + size, min.y + size);
    drawList->AddRectFilled(min, max, background, size * 0.22f);
    drawList->AddRect(min, max, IM_COL32(255, 255, 255, 78), size * 0.22f);
    drawList->AddRectFilled(min, ImVec2(min.x + size * 0.16f, max.y),
                            ToU32(accent), size * 0.10f,
                            ImDrawFlags_RoundCornersLeft);

    const ImU32 ink = IM_COL32(226, 243, 244, 235);
    const ImVec2 center(min.x + size * 0.5f + size * 0.06f, min.y + size * 0.5f);
    const float glyph = size * 0.56f;
    const bool texture = type.find("Texture") != std::string::npos ||
                         type.find("Sampler") != std::string::npos ||
                         type.find("SceneTexture") != std::string::npos;
    const bool parameter = type.find("Parameter") != std::string::npos;
    const bool constant = type.find("Constant") != std::string::npos;
    const bool uv = type.find("UV") != std::string::npos ||
                    type.find("Coordinate") != std::string::npos ||
                    type.find("Panner") != std::string::npos ||
                    type.find("Rotator") != std::string::npos;
    if (texture) {
        DrawCheckboard(drawList, ImVec2(center.x - glyph * 0.4f, center.y - glyph * 0.4f),
                       glyph * 0.8f, IM_COL32(225, 242, 242, 235),
                       IM_COL32(92, 128, 132, 235));
        drawList->AddRect(ImVec2(center.x - glyph * 0.4f, center.y - glyph * 0.4f),
                          ImVec2(center.x + glyph * 0.4f, center.y + glyph * 0.4f),
                          IM_COL32(240, 255, 255, 150));
    } else if (uv) {
        for (int i = 0; i < 3; ++i) {
            const float t = (static_cast<float>(i) + 1.0f) * 0.25f;
            drawList->AddLine(ImVec2(center.x - glyph * 0.4f, center.y - glyph * 0.4f + glyph * 0.8f * t),
                              ImVec2(center.x + glyph * 0.4f, center.y - glyph * 0.4f + glyph * 0.8f * t),
                              IM_COL32(220, 240, 240, 190), 1.0f);
            drawList->AddLine(ImVec2(center.x - glyph * 0.4f + glyph * 0.8f * t, center.y - glyph * 0.4f),
                              ImVec2(center.x - glyph * 0.4f + glyph * 0.8f * t, center.y + glyph * 0.4f),
                              IM_COL32(220, 240, 240, 190), 1.0f);
        }
    } else if (parameter) {
        drawList->AddCircleFilled(center, glyph * 0.4f, IM_COL32(255, 255, 255, 30), 12);
        drawList->AddText(ImGui::GetFont(), size * 0.62f,
                          ImVec2(center.x - size * 0.18f, center.y - size * 0.32f), ink, "P");
    } else if (constant) {
        drawList->AddText(ImGui::GetFont(), size * 0.72f,
                          ImVec2(center.x - size * 0.20f, center.y - size * 0.38f), ink, "#");
    } else {
        const char* glyphText = "?";
        if (type.find("Add") != std::string::npos) glyphText = "+";
        else if (type.find("Subtract") != std::string::npos) glyphText = "-";
        else if (type.find("Multiply") != std::string::npos) glyphText = "x";
        else if (type.find("Divide") != std::string::npos) glyphText = "/";
        else if (type.find("Function") != std::string::npos) glyphText = "f";
        else if (type.find("Noise") != std::string::npos) glyphText = "~";
        else if (category == "Math") glyphText = "=";
        else if (category == "Vector") glyphText = "v";
        else if (category == "Textures") glyphText = "T";
        else if (category == "Parameters") glyphText = "P";
        drawList->AddText(ImGui::GetFont(), size * 0.66f,
                          ImVec2(center.x - size * 0.18f, center.y - size * 0.34f), ink,
                          glyphText);
    }
}

inline void DrawPinThumbnail(ImDrawList* drawList, ImVec2 min, float width, float height,
                             PinType type, const PropertyValue& value) {
    if (!drawList || width <= 0.0f) return;
    const ImVec2 max(min.x + width, min.y + height);
    drawList->AddRectFilled(min, max, IM_COL32(12, 18, 22, 255), 3.0f);
    if (type == PinType::Texture || type == PinType::Sampler) {
        DrawCheckboard(drawList, min, std::min(width, height), IM_COL32(215, 235, 235, 255),
                       IM_COL32(64, 94, 98, 255));
    } else if (type == PinType::Vector4 || type == PinType::Vector3 ||
               type == PinType::Vector2) {
        ImVec4 color(0.2f, 0.65f, 0.7f, 1.0f);
        if (value.kind == PropertyKind::Color4) color = ImVec4(value.vec4.x, value.vec4.y, value.vec4.z, 1.0f);
        else if (value.kind == PropertyKind::Color3) color = ImVec4(value.vec3.x, value.vec3.y, value.vec3.z, 1.0f);
        else if (value.kind == PropertyKind::Vector4) color = ImVec4(value.vec4.x, value.vec4.y, value.vec4.z, 1.0f);
        else if (value.kind == PropertyKind::Vector3) color = ImVec4(value.vec3.x, value.vec3.y, value.vec3.z, 1.0f);
        drawList->AddRectFilled(min, max, ToU32(color), 3.0f);
        drawList->AddRect(min, max, IM_COL32(255, 255, 255, 55), 3.0f);
    } else if (type == PinType::Wildcard) {
        drawList->AddRectFilled(min, ImVec2(min.x + width * 0.5f - 1.0f, max.y),
                                IM_COL32(90, 190, 120, 255), 3.0f);
        drawList->AddRectFilled(ImVec2(min.x + width * 0.5f + 1.0f, min.y), max,
                                IM_COL32(190, 75, 70, 255), 3.0f);
    } else {
        drawList->AddRectFilledMultiColor(
            min, max, IM_COL32(35, 60, 68, 255), IM_COL32(110, 210, 205, 255),
            IM_COL32(110, 210, 205, 255), IM_COL32(35, 60, 68, 255));
        char text[32] = {};
        if (value.kind == PropertyKind::Float) {
            std::snprintf(text, sizeof(text), "%.2f", value.number);
            drawList->AddText(ImVec2(min.x + 4.0f, min.y + 3.0f),
                              IM_COL32(245, 255, 255, 235), text);
        }
    }
}

} // namespace Ui
} // namespace mf
