#pragma once

#include <string>

namespace deepagent {

// DeepAgent runtime configuration.
// Loaded from config/.env next to the executable's project root, then
// overridden by same-named process environment variables.
struct Config {
    // LLM (DeepSeek, OpenAI-compatible; mirrors E:\langgraph-cpp examples/graph_studio/.env)
    std::string llm_api_key;
    std::string llm_model = "deepseek-chat";
    std::string llm_base_url = "https://api.deepseek.com";
    std::string llm_path = "/v1/chat/completions";
    int llm_max_tokens = 2048;
    double llm_temperature = 0.7;

    // Server
    std::string server_host = "127.0.0.1";
    int server_port = 8090;

    // Node 上下文管理：discarded 上下文保留天数（超过后物理删除）
    int ctx_retention_days = 15;

    // 公共部署目录：U-DeepRT.exe 与 DeepAgentBackend.exe 同目录部署，
    // udrt/资产等跨进程交付物使用相对该目录的路径传递（可移植）
    std::string exe_dir;

    // Paths
    std::string frontend_dir;
    std::string knowledge_dir;
    std::string udrt_output_dir;     // 默认 <exe_dir>\udrt（相对路径基准目录）
    std::string chats_file;

    // 工程宿主进程启动（V1.5.0）：auto_launch=1 时资产生成完自动启动对应 exe；
    // 默认关（两 exe 直接启动目前由用户完善中），可 POST /api/launch 手动启动
    bool auto_launch = false;
    std::string avatar_exe;          // 默认 <exe_dir>\Avatar.exe
    std::string udrt_exe;            // 默认 <exe_dir>\UDeepRT.exe

    // Load from <root>/config/.env with env-var overrides.
    // root_dir: directory that contains frontend/, knowledge/, config/, output/.
    static Config load(const std::string& root_dir, const std::string& exe_dir);

    bool llmConfigured() const { return !llm_api_key.empty(); }
};

} // namespace deepagent
