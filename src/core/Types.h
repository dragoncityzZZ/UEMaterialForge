#pragma once

#include "math/Math.h"

#include <cstdint>
#include <string>

namespace mf {

using NodeId = std::uint64_t;
using PinId = std::uint64_t;
using LinkId = std::uint64_t;

// Pin types mirror the types surfaced by Unreal Engine's material expression
// system. Wildcard is only used for polymorphic math expressions; the editor
// resolves it to a concrete numeric type from the connected inputs.
enum class PinType : std::uint8_t {
    Float = 0,
    Vector2,
    Vector3,
    Vector4,
    Texture,
    Sampler,
    Wildcard
};

enum class PinDirection : std::uint8_t {
    Input = 0,
    Output
};

const char* PinTypeToString(PinType type);
PinType PinTypeFromString(const std::string& value, bool* ok = nullptr);
bool IsNumericPin(PinType type);
int PinComponentCount(PinType type);
PinType NumericPinType(int components);
PinType PromotePinTypes(PinType a, PinType b);
bool ArePinTypesLinkCompatible(PinType output, PinType input);
const char* PinDirectionToString(PinDirection direction);

constexpr const char* kSchemaId = "materialforge.ue5.material";
constexpr int kSchemaVersion = 1;

} // namespace mf
