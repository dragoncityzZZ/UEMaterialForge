#pragma once

#include "core/Graph.h"
#include "core/Property.h"

#include <filesystem>
#include <string>
#include <vector>

namespace mf {

struct MaterialInstanceOverride {
    std::string name;
    PinType type = PinType::Float;
    PropertyValue value{};
};

// A material instance asset: a parent material plus a sparse set of parameter
// overrides. The parent graph itself is never modified on disk.
class MaterialInstance {
public:
    bool Load(const std::filesystem::path& path, std::string* error = nullptr);
    bool Save(const std::filesystem::path& path, std::string* error = nullptr) const;

    const std::string& Name() const { return m_Name; }
    void SetName(std::string name) { m_Name = std::move(name); }

    const std::string& ParentMaterial() const { return m_ParentMaterial; }
    void SetParentMaterial(std::string parent) { m_ParentMaterial = std::move(parent); }

    const std::vector<MaterialInstanceOverride>& Overrides() const { return m_Overrides; }
    bool IsOverridden(const std::string& name) const;
    const PropertyValue* FindOverride(const std::string& name) const;

    void SetOverride(const std::string& name, PinType type, const PropertyValue& value);
    bool ClearOverride(const std::string& name);
    void ClearOverrides();

    // Applies overrides to scalar/vector parameter nodes in `graph`. Texture
    // parameters are metadata-only in this version.
    bool ApplyToGraph(MaterialGraph& graph,
                      std::vector<std::string>* warnings = nullptr) const;

private:
    std::string m_Name;
    std::string m_ParentMaterial;
    std::vector<MaterialInstanceOverride> m_Overrides;
};

} // namespace mf
