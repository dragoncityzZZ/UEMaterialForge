#pragma once

#include <string>

namespace mf {

enum class PreviewMesh : int {
    Sphere = 0,
    Plane = 1,
    Custom = 2
};

const char* PreviewMeshName(PreviewMesh mesh);

struct PreviewEnvironmentSettings {
    std::string hdriPath;
    bool groundEnabled = false;
    float groundHeight = -1.05f;
    float environmentRotation = 0.0f;
    float exposure = 1.0f;
};

struct PreviewCamera {
    float yaw = 0.28f;
    float pitch = 0.12f;
    float distance = 3.55f;

    void Reset() { *this = PreviewCamera{}; }
};

} // namespace mf
