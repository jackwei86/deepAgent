#pragma once

#include <string>
#include <vector>

#include "third_party/json.hpp"

namespace deepagent {

// Deterministic node catalog: maps semantic node keys and semantic port names
// to udrt numeric module ids and port indexes. The LLM never writes udrt
// directly; everything goes through this catalog.
class NodeCatalog {
public:
    bool load(const std::string& path, std::string* error);

    const nlohmann::json* node(const std::string& catalogKey) const;

    // Resolve "nodeKey:semanticPort" against a plan node list.
    // Returns true and fills numeric port index when found.
    bool resolvePort(const nlohmann::json& planNodes, const std::string& nodeKey,
                     const std::string& semanticPort, int* portIndex,
                     const nlohmann::json** catalogNode) const;

    // All catalog keys (sorted) — used by structured diagnostics (P2).
    std::vector<std::string> catalogKeys() const;

private:
    nlohmann::json nodes_ = nlohmann::json::object();
};

} // namespace deepagent
