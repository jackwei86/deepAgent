#pragma once

#include <string>

#include <neograph/provider.h>

#include "config.h"
#include "llm_client.h"

namespace deepagent {

// Agent Loop 的 LLM Provider：WinHTTP 传输（本服务进程内长期验证可靠的通道）。
// 实现完整 OpenAI 工具协议（非流式 tool_calls 检测 + 流式最终回复）。
// 为什么不用 NeoGraph OpenAIProvider：其 ConnPool 异步链路在本服务进程内
// 挂起不前（同步 asio/WinHTTP 均正常；详见 V1.4.0 CHANGELOG 修复项）。
class WinHttpProvider final : public neograph::Provider {
public:
    WinHttpProvider(LlmClient& llm, const Config& config) : llm_(llm), config_(config) {}

    // 非流式补全（Agent Loop 工具检测阶段）：解析 tool_calls 完整回传。
    neograph::ChatCompletion complete(const neograph::CompletionParams& params) override;

    // 流式补全（最终回复阶段）：token 增量回调。
    neograph::ChatCompletion complete_stream(const neograph::CompletionParams& params,
                                             const neograph::StreamCallback& on_chunk) override;

    std::string get_name() const override { return "deepagent-winhttp"; }

private:
    nlohmann::json buildBody(const neograph::CompletionParams& params, bool stream) const;

    LlmClient& llm_;
    const Config& config_;
};

} // namespace deepagent
