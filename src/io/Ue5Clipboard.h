#pragma once

#include "core/Graph.h"

#include <string>
#include <vector>

namespace mf {

// Result of importing a UE5 material graph copied as text (Ctrl+C in the
// Unreal material editor) into the MaterialForge node graph.
struct Ue5ClipboardImportResult {
    bool success = false;
    int nodeCount = 0;
    int linkCount = 0;
    int skippedNodeCount = 0;
    // Links whose source was copied but whose target was the material root.
    // UE usually omits the root object from the clipboard, so these have to be
    // reconnected manually to a Material Output input.
    int droppedMaterialOutputLinks = 0;
    std::vector<NodeId> createdNodes;
    // UE material expression classes that have no MaterialForge equivalent.
    // Duplicates are removed; the order is the first appearance in the text.
    std::vector<std::string> skippedClasses;
    std::string error;
};

// Cheap heuristic used before attempting a full parse; it keeps the normal
// node clipboard path when unrelated text is on the system clipboard.
bool LooksLikeUe5MaterialClipboard(const std::string& text);

// Parses a UE4/UE5 material graph text export (the clipboard format produced by
// "Copy" in the Unreal material editor) and adds the supported expressions to
// `graph`. Nodes are placed relative to `origin` (logical canvas coordinates,
// i.e. already divided by the UI scale), keeping their UE layout.
bool ImportUe5MaterialClipboard(const std::string& text, MaterialGraph& graph,
                                const Vec2& origin,
                                Ue5ClipboardImportResult* result = nullptr);

} // namespace mf
