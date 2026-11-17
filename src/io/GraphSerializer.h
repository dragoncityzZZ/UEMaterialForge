#pragma once

#include "core/Graph.h"

#include <nlohmann/json.hpp>

#include <string>

namespace mf {

// Serializes the graph to a stable, UE5-oriented JSON document. The document
// keeps both the editor data (positions, ids, effective types) and explicit UE
// expression class names so a Python bridge can reconstruct the material.
class GraphSerializer {
public:
    static nlohmann::json ToJson(const MaterialGraph& graph);
    static bool FromJson(const nlohmann::json& document, MaterialGraph& graph, std::string* error = nullptr);

    static bool Save(const MaterialGraph& graph, const std::string& path, std::string* error = nullptr);
    static bool Load(const std::string& path, MaterialGraph& graph, std::string* error = nullptr);

    static const char* SchemaId();
    static int SchemaVersion();
};

} // namespace mf
