#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "config.h"
#include "knowledge_base.h"
#include "llm_client.h"
#include "node_context.h"
#include "node_catalog.h"
#include "third_party/json.hpp"
#include "udrt_compiler.h"

#include <neograph/llm/agent.h>
#include <neograph/llm/openai_provider.h>

namespace deepagent {

class NodeContextStore;

// LangGraph 风格 agent：聊天编排升级为 LLM 自主决策的循环模式（Agent Loop）。
// LLM 拿到工具清单（知识库/udrt 生成/资产生成/上下文查询）后自主决定
// 调用哪个工具、是否需要多步；程序执行工具并回填结果，循环至最终回答。
// 工具协议不可用时回退固定管线（runLegacyPipeline）。
class AgentGraph {
public:
    struct EventSink {
        std::function<void(const std::string& description)> onStatus;
        std::function<void(const std::string& delta)> onToken;
        std::function<void(const nlohmann::json& payload)> onUdrt;   // udrt 工程卡片
        std::function<void(const nlohmann::json& payload)> onAsset;  // HTML 资产卡片
        // 子步状态事件：{guid, step(bg/content/compose 节点名), state(running/done/failed)}
        std::function<void(const nlohmann::json& payload)> onNodeState;
    };

    AgentGraph(const Config& config, LlmClient& llm, const KnowledgeBase& kb,
               const NodeCatalog& catalog, const UdrtCompiler& compiler,
               NodeContextStore& nodeContexts);

    // 运行一轮对话：优先 Agent Loop（LLM 自主工具调用），
    // 工具协议不可用时回退固定管线。返回最终回复。
    std::string run(const std::string& userMessage,
                    const std::vector<ChatMessage>& history,
                    const EventSink& sink, std::string* error);

    // ---- 工具执行器（供 neograph::llm::Agent 的 tool_calls 调用）----
    // 各执行器返回 JSON 字符串（回填给 LLM 的工具结果）。
    std::string toolListKnowledgeNodes() const;
    std::string toolCreateUdrt(const std::string& entryId, const std::string& subtitleText,
                               const std::string& stylePrompt, std::optional<bool> background,
                               std::string* guidOut, std::string* udrtFileOut,
                               std::string* htmlFileOut);
    std::string toolGenerateNodeAsset(const std::string& guid, const std::string& kind,
                                      const std::string& text, std::optional<bool> background);
    std::string toolGetNodeContext(const std::string& guid) const;

private:
    // ---- 旧固定管线（fallback：LLM tools 协议异常时回退）----
    std::string runLegacyPipeline(const std::string& userMessage,
                                  const std::vector<ChatMessage>& history,
                                  const EventSink& sink, std::string* error);
    // 绝对路径 → 相对 exe 目录（与 /api/asset/content 的 path 参数协议一致）
    std::string makeRelative(const std::string& absolute) const;
    nlohmann::json analyzeIntent(const std::string& userMessage, std::string* llmError);
    nlohmann::json analyzeIntentByKeywords(const std::string& userMessage) const;
    bool planGraph(const nlohmann::json& entry, const nlohmann::json& intent,
                   const std::string& userMessage, nlohmann::json* planIr,
                   bool* usedTemplate);
    nlohmann::json templatePlan(const nlohmann::json& entry) const;

    const Config& config_;
    LlmClient& llm_;
    const KnowledgeBase& kb_;
    const NodeCatalog& catalog_;
    const UdrtCompiler& compiler_;
    NodeContextStore& nodeContexts_;
    const EventSink* m_activeSink = nullptr;   // run() 期间有效，工具执行器用于发事件
};

} // namespace deepagent
