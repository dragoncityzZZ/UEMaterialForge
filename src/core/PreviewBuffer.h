#pragma once

namespace mf {

// Selects an intermediate material buffer for the live preview.
enum class PreviewBuffer : int {
    Lit = 0,
    BaseColor,
    Metallic,
    Specular,
    Roughness,
    Emissive,
    Normal,
    AmbientOcclusion,
    Opacity,
    WorldNormal,
    Depth,
    Count
};

inline const char* PreviewBufferName(PreviewBuffer buffer) {
    switch (buffer) {
    case PreviewBuffer::BaseColor: return "Base Color";
    case PreviewBuffer::Metallic: return "Metallic";
    case PreviewBuffer::Specular: return "Specular";
    case PreviewBuffer::Roughness: return "Roughness";
    case PreviewBuffer::Emissive: return "Emissive";
    case PreviewBuffer::Normal: return "Normal";
    case PreviewBuffer::AmbientOcclusion: return "Ambient Occlusion";
    case PreviewBuffer::Opacity: return "Opacity";
    case PreviewBuffer::WorldNormal: return "World Normal";
    case PreviewBuffer::Depth: return "Depth";
    case PreviewBuffer::Lit:
    default: return "Lit";
    }
}

inline bool IsScalarPreviewBuffer(PreviewBuffer buffer) {
    return buffer == PreviewBuffer::Metallic || buffer == PreviewBuffer::Specular ||
           buffer == PreviewBuffer::Roughness || buffer == PreviewBuffer::AmbientOcclusion ||
           buffer == PreviewBuffer::Opacity || buffer == PreviewBuffer::Depth;
}

} // namespace mf
