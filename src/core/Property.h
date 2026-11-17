#pragma once

#include "core/Types.h"

#include <string>
#include <vector>

namespace mf {

enum class PropertyKind : std::uint8_t {
    Float = 0,
    Vector2,
    Vector3,
    Vector4,
    Color3,
    Color4,
    Bool,
    String,
    AssetPath
};

// A small tagged value used both for node properties and unconnected pin
// defaults. Keeping it plain data makes JSON export and editor inspection
// straightforward.
struct PropertyValue {
    PropertyKind kind = PropertyKind::Float;
    float number = 0.0f;
    Vec2 vec2{};
    Vec3 vec3{};
    Vec4 vec4{};
    bool boolean = false;
    std::string text;

    static PropertyValue Float(float value);
    static PropertyValue Vec2Value(float x, float y);
    static PropertyValue Vec3Value(float x, float y, float z);
    static PropertyValue Vec4Value(float x, float y, float z, float w);
    static PropertyValue Color3Value(float r, float g, float b);
    static PropertyValue Color4Value(float r, float g, float b, float a);
    static PropertyValue BoolValue(bool value);
    static PropertyValue StringValue(std::string value);
    static PropertyValue AssetPathValue(std::string value);

    PinType NumericPinType() const;
};

struct PropertySpec {
    std::string key;
    std::string label;
    std::string tooltip;
    PropertyValue defaultValue{};
    float dragSpeed = 0.01f;
    bool hasRange = false;
    float minValue = 0.0f;
    float maxValue = 1.0f;
};

inline bool IsColorProperty(PropertyKind kind) {
    return kind == PropertyKind::Color3 || kind == PropertyKind::Color4;
}

inline bool IsStringProperty(PropertyKind kind) {
    return kind == PropertyKind::String || kind == PropertyKind::AssetPath;
}

} // namespace mf
