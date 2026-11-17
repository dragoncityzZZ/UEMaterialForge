#pragma once

namespace mf {

// Artistic controls for the studio preview lighting. Values are embedded into
// the generated shader so all preview backends stay in sync.
struct PreviewLightingSettings {
    float keyIntensity = 1.0f;
    float fillIntensity = 1.0f;
    float rimIntensity = 1.0f;
    float ambientIntensity = 1.0f;
    float exposure = 1.0f;
};

} // namespace mf
