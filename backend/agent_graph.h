#pragma once

#include <functional>
#include <string>
#include <vector>

#include "config.h"
#include "knowledge_base.h"
#include "llm_client.h"
#include "node_catalog.h"
#include "third_party/json.hpp"
#include "udrt_compiler.h"

namespace deepagent {

// LangGraph-style agent pipeline over a JSON state:
//   analyze_intent -> validate_selection -> plan_graph
//    -> compile_udrt -> validate_udrt -> write_file -> reply
class AgentGraph {
public:
    struct EventSink {
        std::function<void(const std::string& description)> onStatus;
        std::function<void(const std::string& delta)> onToken;
        std::function<void(const nlohmann::json& payload)> onUdrt;
        std::function<void(const nlohmann::json& payload)> onAsset;   // HTML 资产卡片
    };

    AgentGraph(const Config& config, LlmClient& llm, const KnowledgeBase& kb,
               const NodeCatalog& catalog, const UdrtCompiler& compiler);

    // Runs the pipeline for one user turn; returns the final assistant reply.
    // history contains previous turns (system prompt excluded).
    std::string run(const std::string& userMessage,
                    const std::vector<ChatMessage>& history,
                    const EventSink& sink, std::string* error);

private:
    // Step 1: LLM decides whether a tool node is needed; returns parsed JSON
    // {is_tool_task, entry_id, subtitle_text, reason} (or null on LLM failure).
    nlohmann::json analyzeIntent(const std::string& userMessage, std::string* llmError);

    // Keyword-only fallback used when the LLM is unavailable.
    nlohmann::json analyzeIntentByKeywords(const std::string& userMessage) const;

    // Step 3: LLM proposes a Graph Plan IR; validated by compile. Falls back
    // to the knowledge base udrt_template when the LLM plan fails.
    bool planGraph(const nlohmann::json& entry, const nlohmann::json& intent,
                   const std::string& userMessage, nlohmann::json* planIr,
                   bool* usedTemplate);

    // Deterministic plan built straight from the entry's udrt_template.
    nlohmann::json templatePlan(const nlohmann::json& entry) const;

    const Config& config_;
    LlmClient& llm_;
    const KnowledgeBase& kb_;
    const NodeCatalog& catalog_;
    const UdrtCompiler& compiler_;
};

} // namespace deepagent
