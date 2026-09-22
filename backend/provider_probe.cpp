// OpenAIProvider non-stream（Agent 工具检测同路径）探针：定位 120s 卡点
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <neograph/llm/openai_provider.h>

using namespace neograph;

int main(int argc, char** argv) {
    const char* key = std::getenv("DEEPSEEK_API_KEY");
    if (!key || !*key) { std::printf("no DEEPSEEK_API_KEY\n"); return 2; }

    const bool with_tool = argc > 1 && std::string(argv[1]) == "--tool";

    neograph::llm::OpenAIProvider::Config pc;
    pc.api_key = key;
    pc.base_url = "https://api.deepseek.com";
    pc.default_model = "deepseek-chat";
    pc.timeout_seconds = 150;
    auto p = neograph::llm::OpenAIProvider::create(pc);

    CompletionParams params;
    params.messages.push_back({"user", "reply with exactly: OK"});
    params.temperature = 0.2f;
    if (with_tool) {
        params.tools.push_back(
            {"get_time", "get current time, no args needed", json{{"type", "object"}, {"properties", json::object()}}});
    }

    const auto t0 = std::chrono::steady_clock::now();
    try {
        ChatCompletion c = p->complete(params);
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("OK %.2fs stop=%s tool_calls=%zu content=%.40s\n",
                    s, c.stop_reason.c_str(), c.message.tool_calls.size(),
                    c.message.content.c_str());
    } catch (const std::exception& e) {
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("FAIL %.2fs: %s\n", s, e.what());
    }
    return 0;
}
