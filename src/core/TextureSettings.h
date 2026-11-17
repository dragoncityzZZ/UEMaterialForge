#pragma once

#include "core/Node.h"

#include <string>

namespace mf {

struct SamplerSettings {
    std::string filter = "Linear";
    std::string addressU = "Repeat";
    std::string addressV = "Repeat";
    bool srgb = true;
    float mipBias = 0.0f;
    float maxAnisotropy = 8.0f;
};

SamplerSettings ReadSamplerSettings(const Node& node);

// Cache key used by the OpenGL preview so textures with different color-space
// or sampler settings are not shared accidentally.
std::string SamplerSettingsKey(const SamplerSettings& settings);

} // namespace mf
