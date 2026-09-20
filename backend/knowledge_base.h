#pragma once

#include <string>
#include <vector>

#include "third_party/json.hpp"

namespace deepagent {

// External knowledge base: preset entries describing callable tool Nodes.
// Each entry carries a semantic id, module id, description/keywords for LLM
// matching, and a deterministic udrt_template used by the compiler.
class KnowledgeBase {
public:
    bool load(const std::string& path, std::string* error);

    const nlohmann::json& entries() const { return entries_; }
    bool empty() const { return entries_.empty(); }

    // Find an entry by semantic id.
    const nlohmann::json* find(const std::string& id) const;

    // Keyword-score every entry against the user message; returns
    // [{id, score}] sorted by score desc (only entries with score > 0).
    nlohmann::json scoreByKeywords(const std::string& message) const;

    // Render entries as compact JSON text for LLM system prompts.
    std::string toPromptText() const;

private:
    nlohmann::json entries_ = nlohmann::json::array();
};

} // namespace deepagent
