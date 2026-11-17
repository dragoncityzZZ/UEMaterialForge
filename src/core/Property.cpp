#include "core/Property.h"

namespace mf {

PropertyValue PropertyValue::Float(float value) {
    PropertyValue out;
    out.kind = PropertyKind::Float;
    out.number = value;
    return out;
}

PropertyValue PropertyValue::Vec2Value(float x, float y) {
    PropertyValue out;
    out.kind = PropertyKind::Vector2;
    out.vec2 = Vec2(x, y);
    return out;
}

PropertyValue PropertyValue::Vec3Value(float x, float y, float z) {
    PropertyValue out;
    out.kind = PropertyKind::Vector3;
    out.vec3 = Vec3(x, y, z);
    return out;
}

PropertyValue PropertyValue::Vec4Value(float x, float y, float z, float w) {
    PropertyValue out;
    out.kind = PropertyKind::Vector4;
    out.vec4 = Vec4(x, y, z, w);
    return out;
}

PropertyValue PropertyValue::Color3Value(float r, float g, float b) {
    PropertyValue out;
    out.kind = PropertyKind::Color3;
    out.vec3 = Vec3(r, g, b);
    return out;
}

PropertyValue PropertyValue::Color4Value(float r, float g, float b, float a) {
    PropertyValue out;
    out.kind = PropertyKind::Color4;
    out.vec4 = Vec4(r, g, b, a);
    return out;
}

PropertyValue PropertyValue::BoolValue(bool value) {
    PropertyValue out;
    out.kind = PropertyKind::Bool;
    out.boolean = value;
    return out;
}

PropertyValue PropertyValue::StringValue(std::string value) {
    PropertyValue out;
    out.kind = PropertyKind::String;
    out.text = std::move(value);
    return out;
}

PropertyValue PropertyValue::AssetPathValue(std::string value) {
    PropertyValue out;
    out.kind = PropertyKind::AssetPath;
    out.text = std::move(value);
    return out;
}

PinType PropertyValue::NumericPinType() const {
    switch (kind) {
    case PropertyKind::Float:
    case PropertyKind::Bool:
        return PinType::Float;
    case PropertyKind::Vector2:
        return PinType::Vector2;
    case PropertyKind::Vector3:
    case PropertyKind::Color3:
        return PinType::Vector3;
    case PropertyKind::Vector4:
    case PropertyKind::Color4:
        return PinType::Vector4;
    default:
        return PinType::Float;
    }
}

} // namespace mf
