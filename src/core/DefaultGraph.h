#pragma once

#include "core/Graph.h"

#include <string>

namespace mf {

// Builds the demo material ("Holographic Circuit") used when no document is
// loaded. The sample JSON shipped with the project is produced from this graph.
bool BuildDefaultGraph(MaterialGraph& graph, std::string* error = nullptr);

} // namespace mf
