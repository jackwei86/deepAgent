#pragma once

#include <functional>
#include <string>
#include <vector>

#include "config.h"
#include "third_party/json.hpp"

namespace deepagent {

struct ChatMessage {
    std::string role;    // "system" | "user" | "assistant"
    std::string content;
};

// OpenAI-compatible chat-completions client (DeepSeek) over WinHTTP.
// Mirrors the wire protocol used by E:\langgraph-cpp ProviderChatModel:
//   POST {base}/v1/chat/completions, Bearer auth, optional SSE streaming.
class LlmClient {
public:
    explicit LlmClient(const Config& config);

    // Non-streaming invoke. Returns full assistant content.
    // On failure returns empty string and fills error.
    // temperature < 0 uses the configured default.
    std::string invoke(const std::vector<ChatMessage>& messages, std::string* error,
                       double temperature = -1.0);

    // Streaming invoke; onToken receives incremental deltas.
    // Returns the fully accumulated assistant content.
    std::string invokeStream(const std::vector<ChatMessage>& messages,
                             const std::function<void(const std::string&)>& onToken,
                             std::string* error);

    // ---- 通用请求层（WinHttpProvider 复用：完整 OpenAI 协议体由调用方组装）----

    // 非流式：发送完整请求体，成功返回完整响应 JSON（含 choices[].message.tool_calls），
    // 失败返回 null 并填 error。
    nlohmann::json postChatJson(const nlohmann::json& body, std::string* error);

    // 流式：发送完整请求体，onDelta 收到增量 content，返回累积 content；
    // 失败返回空串并填 error。
    // toolCallsOut（可空）：模型在流中发起的工具调用，按 OpenAI 协议形态累积：
    //   [{id, type:"function", function:{name, arguments}}]
    // finishReasonOut（可空）：choices[0].finish_reason 原值（如 "tool_calls"/"stop"）。
    std::string postChatStream(const nlohmann::json& body,
                               const std::function<void(const std::string&)>& onDelta,
                               std::string* error,
                               nlohmann::json* toolCallsOut = nullptr,
                               std::string* finishReasonOut = nullptr);

private:
    std::string request(const std::vector<ChatMessage>& messages, bool stream,
                        const std::function<void(const std::string&)>& onToken,
                        std::string* error, double temperature);

    // 公共 HTTP 层：发送 body，非流式时 *rawBody 收完整响应体；流式时逐 delta 回调
    // （rawBody=累积 content），并按 index 累积流式 tool_calls（协议形态数组）与
    // finish_reason（均可空）。
    bool sendChat(const nlohmann::json& body, bool stream,
                  const std::function<void(const std::string&)>& onDelta,
                  std::string* rawBody, std::string* error,
                  nlohmann::json* toolCallsOut = nullptr,
                  std::string* finishReasonOut = nullptr);

    const Config& config_;
};

} // namespace deepagent
