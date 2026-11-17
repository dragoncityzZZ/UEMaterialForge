#pragma once

#include "core/Graph.h"

#include <unordered_map>
#include <unordered_set>

namespace mf {

// Resolves Wildcard pins to concrete numeric types by walking upstream links.
// The resolver is cheap to construct and is cached per query instance. It is
// shared by the editor (pin colors / tooltips) and the GLSL generator (type
// conversions) so both always agree on the effective pin types.
class TypeResolver {
public:
    explicit TypeResolver(const MaterialGraph& graph);

    PinType GetEffectiveType(const Pin& pin) const;
    PinType GetInputType(const Pin& pin) const { return ResolveInput(pin); }
    PinType GetOutputType(const Pin& pin) const { return ResolveOutput(pin); }

private:
    PinType ResolveInput(const Pin& pin) const;
    PinType ResolveOutput(const Pin& pin) const;
    PinType DefaultTypeForPin(const Pin& pin) const;

    const MaterialGraph& m_Graph;
    mutable std::unordered_map<PinId, PinType> m_Cache;
    mutable std::unordered_set<PinId> m_Visiting;
};

} // namespace mf
