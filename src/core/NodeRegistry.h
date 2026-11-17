#pragma once

#include "core/Node.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace mf {

class NodeRegistry {
public:
    static const NodeRegistry& Get();

    const NodeDefinition* Find(const std::string& type) const;
    const std::vector<NodeDefinition>& All() const { return m_Definitions; }
    std::vector<std::string> Categories() const;

private:
    NodeRegistry();
    void Register(NodeDefinition definition);

    std::vector<NodeDefinition> m_Definitions;
    std::unordered_map<std::string, std::size_t> m_Index;
};

} // namespace mf
