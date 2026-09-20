#pragma once

#include <functional>
#include <string>
#include <vector>

#include "config.h"

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

private:
    std::string request(const std::vector<ChatMessage>& messages, bool stream,
                        const std::function<void(const std::string&)>& onToken,
                        std::string* error, double temperature);

    const Config& config_;
};

} // namespace deepagent
