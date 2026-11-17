#include "core/Types.h"

#include <algorithm>
#include <cctype>

namespace mf {

const char* PinTypeToString(PinType type) {
    switch (type) {
    case PinType::Float: return "Float";
    case PinType::Vector2: return "Vector2";
    case PinType::Vector3: return "Vector3";
    case PinType::Vector4: return "Vector4";
    case PinType::Texture: return "Texture";
    case PinType::Sampler: return "Sampler";
    case PinType::Wildcard: return "Wildcard";
    }
    return "Float";
}

PinType PinTypeFromString(const std::string& value, bool* ok) {
    if (ok) {
        *ok = true;
    }
    if (value == "Float") return PinType::Float;
    if (value == "Vector2" || value == "Float2" || value == "Vec2") return PinType::Vector2;
    if (value == "Vector3" || value == "Float3" || value == "Vec3") return PinType::Vector3;
    if (value == "Vector4" || value == "Float4" || value == "Vec4") return PinType::Vector4;
    if (value == "Texture" || value == "Texture2D") return PinType::Texture;
    if (value == "Sampler" || value == "SamplerState") return PinType::Sampler;
    if (value == "Wildcard") return PinType::Wildcard;
    if (ok) {
        *ok = false;
    }
    return PinType::Float;
}

bool IsNumericPin(PinType type) {
    return type == PinType::Float || type == PinType::Vector2 ||
           type == PinType::Vector3 || type == PinType::Vector4 ||
           type == PinType::Wildcard;
}

int PinComponentCount(PinType type) {
    switch (type) {
    case PinType::Float: return 1;
    case PinType::Vector2: return 2;
    case PinType::Vector3: return 3;
    case PinType::Vector4: return 4;
    default: return 0;
    }
}

PinType NumericPinType(int components) {
    switch (components) {
    case 1: return PinType::Float;
    case 2: return PinType::Vector2;
    case 3: return PinType::Vector3;
    case 4: return PinType::Vector4;
    default: return PinType::Float;
    }
}

PinType PromotePinTypes(PinType a, PinType b) {
    if (a == PinType::Wildcard) return b;
    if (b == PinType::Wildcard) return a;
    if (!IsNumericPin(a) || !IsNumericPin(b)) {
        return a == b ? a : PinType::Wildcard;
    }
    const int components = std::max(PinComponentCount(a), PinComponentCount(b));
    return NumericPinType(components);
}

bool ArePinTypesLinkCompatible(PinType output, PinType input) {
    const bool outNumeric = IsNumericPin(output);
    const bool inNumeric = IsNumericPin(input);
    if (outNumeric && inNumeric) {
        return true;
    }
    if (output == PinType::Wildcard || input == PinType::Wildcard) {
        return false;
    }
    return output == input;
}

const char* PinDirectionToString(PinDirection direction) {
    return direction == PinDirection::Input ? "input" : "output";
}

} // namespace mf
