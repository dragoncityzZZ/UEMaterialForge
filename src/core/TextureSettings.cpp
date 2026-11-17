#include "core/TextureSettings.h"

#include <iomanip>
#include <sstream>

namespace mf {

namespace {
std::string ReadString(const Node& node, const char* key, const std::string& fallback) {
    const auto it = node.properties.find(key);
    if (it != node.properties.end() && IsStringProperty(it->second.kind) &&
        !it->second.text.empty()) {
        return it->second.text;
    }
    return fallback;
}

float ReadFloat(const Node& node, const char* key, float fallback) {
    const auto it = node.properties.find(key);
    return it != node.properties.end() && it->second.kind == PropertyKind::Float
               ? it->second.number
               : fallback;
}

bool ReadBool(const Node& node, const char* key, bool fallback) {
    const auto it = node.properties.find(key);
    return it != node.properties.end() && it->second.kind == PropertyKind::Bool
               ? it->second.boolean
               : fallback;
}
} // namespace

SamplerSettings ReadSamplerSettings(const Node& node) {
    SamplerSettings settings;
    settings.filter = ReadString(node, "filter", settings.filter);
    settings.addressU = ReadString(node, "addressU", settings.addressU);
    settings.addressV = ReadString(node, "addressV", settings.addressV);
    settings.srgb = ReadBool(node, "srgb", settings.srgb);
    settings.mipBias = ReadFloat(node, "mipBias", settings.mipBias);
    settings.maxAnisotropy = ReadFloat(node, "maxAnisotropy", settings.maxAnisotropy);
    return settings;
}

std::string SamplerSettingsKey(const SamplerSettings& settings) {
    std::ostringstream stream;
    stream << settings.filter << '|' << settings.addressU << '|' << settings.addressV
           << '|' << (settings.srgb ? 1 : 0) << '|' << std::fixed
           << std::setprecision(3) << settings.mipBias << '|'
           << std::setprecision(1) << settings.maxAnisotropy;
    return stream.str();
}

} // namespace mf
