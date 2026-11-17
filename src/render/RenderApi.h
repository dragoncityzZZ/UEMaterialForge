#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace mf {

// Selectable GPU pipeline used by the live material preview. The editor shell
// itself is composited with OpenGL on all platforms; this enum selects the
// backend that shades the preview surface.
enum class RenderApi {
    OpenGL = 0,
    Vulkan = 1,
    DirectX12 = 2
};

inline const char* RenderApiName(RenderApi api) {
    switch (api) {
    case RenderApi::Vulkan: return "vulkan";
    case RenderApi::DirectX12: return "dx12";
    case RenderApi::OpenGL:
    default: return "opengl";
    }
}

inline const char* RenderApiDisplayName(RenderApi api) {
    switch (api) {
    case RenderApi::Vulkan: return "Vulkan";
    case RenderApi::DirectX12: return "DirectX 12";
    case RenderApi::OpenGL:
    default: return "OpenGL";
    }
}

inline RenderApi ParseRenderApi(const std::string& value, RenderApi fallback = RenderApi::OpenGL) {
    std::string normalized = value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (normalized == "vulkan") return RenderApi::Vulkan;
    if (normalized == "dx12" || normalized == "d3d12" || normalized == "directx12" ||
        normalized == "directx 12") {
        return RenderApi::DirectX12;
    }
    if (normalized == "opengl" || normalized == "gl") return RenderApi::OpenGL;
    return fallback;
}

} // namespace mf
