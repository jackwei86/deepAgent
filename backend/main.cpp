#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>

#include <windows.h>
#include <shellapi.h>

#include "agent_graph.h"
#include "chat_store.h"
#include "config.h"
#include "http_server.h"
#include "knowledge_base.h"
#include "log.h"
#include "node_catalog.h"
#include "node_context.h"
#include "tray.h"
#include "udrt_compiler.h"

namespace fs = std::filesystem;

namespace {

std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0,
                                   nullptr, nullptr);
    std::string s(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], size, nullptr,
                        nullptr);
    return s;
}

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], size);
    return w;
}

bool looksLikeRoot(const fs::path& dir) {
    return fs::exists(dir / "config" / ".env") && fs::exists(dir / "frontend" / "index.html");
}

// 公共部署目录（exe 所在目录）：U-DeepRT.exe 与 DeepAgentBackend.exe 同目录部署，
// udrt/资产等交付物使用相对该目录的路径
std::string getExeDir() {
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dir(exePath);
    auto pos = dir.find_last_of(L'\\');
    return wideToUtf8(dir.substr(0, pos));
}

// Locate the DeepAgent root (contains config/.env and frontend/index.html):
// 1. DEEPAGENT_ROOT env var
// 2. walk up from the executable directory (exe lives in MetaSDK/x64/Debug)
// 3. walk up from the current working directory
std::string findRootDir() {
    const char* env = std::getenv("DEEPAGENT_ROOT");
    if (env && *env) {
        fs::path p = fs::path(env);
        if (looksLikeRoot(p)) return p.string();
    }
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
        fs::path dir = fs::path(exePath).parent_path();
        for (int i = 0; i < 6 && !dir.empty(); ++i) {
            if (looksLikeRoot(dir / "DeepAgent")) return (dir / "DeepAgent").string();
            if (looksLikeRoot(dir)) return dir.string();
            dir = dir.parent_path();
        }
    }
    fs::path cwd = fs::current_path();
    for (int i = 0; i < 6 && !cwd.empty(); ++i) {
        if (looksLikeRoot(cwd)) return cwd.string();
        cwd = cwd.parent_path();
    }
    return (fs::current_path() / "DeepAgent").string();
}

void ensureDirectory(const std::string& path) {
    std::error_code ec;
    fs::create_directories(fs::path(path), ec);
}

std::string chatUrlOf(const deepagent::Config& config) {
    std::string host = config.server_host;
    if (host.empty() || host == "0.0.0.0") host = "127.0.0.1";
    return "http://" + host + ":" + std::to_string(config.server_port) + "/";
}

// 用系统默认浏览器打开对话页面
void openChatInBrowser(const deepagent::Config& config) {
    ShellExecuteW(nullptr, L"open", utf8ToWide(chatUrlOf(config)).c_str(), nullptr,
                  nullptr, SW_SHOWNORMAL);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    // 1. 单实例：重复启动时直接唤起已有服务的对话页面后退出
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\DeepAgentBackend_SingleInstance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        deepagent::Config existing = deepagent::Config::load(findRootDir(), getExeDir());
        openChatInBrowser(existing);
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    // 2. 初始化：根目录 / 日志 / 配置
    std::string rootDir = findRootDir();
    ensureDirectory(rootDir + "\\output");
    deepagent::InitLog((rootDir + "\\output\\deepagent.log").c_str());
    deepagent::Config config = deepagent::Config::load(rootDir, getExeDir());
    ensureDirectory(config.udrt_output_dir);

    deepagent::Logf("=== DeepAgent Backend (tray) ===");
    deepagent::Logf("root dir      : %s", rootDir.c_str());
    deepagent::Logf("LLM           : %s%s (model=%s, key=%s)", config.llm_base_url.c_str(),
                    config.llm_path.c_str(), config.llm_model.c_str(),
                    config.llmConfigured() ? "configured" : "MISSING");

    // 3. 知识库 / 节点目录
    deepagent::KnowledgeBase knowledge;
    deepagent::NodeCatalog catalog;
    std::string err;
    if (!knowledge.load(config.knowledge_dir + "\\knowledge_base.json", &err)) {
        deepagent::Logf("[FATAL] %s", err.c_str());
        MessageBoxW(nullptr, utf8ToWide(err).c_str(), L"DeepAgent 启动失败", MB_ICONERROR);
        if (mutex) CloseHandle(mutex);
        return 1;
    }
    if (!catalog.load(config.knowledge_dir + "\\node_catalog.json", &err)) {
        deepagent::Logf("[FATAL] %s", err.c_str());
        MessageBoxW(nullptr, utf8ToWide(err).c_str(), L"DeepAgent 启动失败", MB_ICONERROR);
        if (mutex) CloseHandle(mutex);
        return 1;
    }
    deepagent::Logf("knowledge     : %zu entries loaded", knowledge.entries().size());
    deepagent::Logf("udrt output   : %s", config.udrt_output_dir.c_str());

    // 4. 服务器对象（回调与服务器线程都会使用，生命周期覆盖消息泵）
    // Node 按 GUID 的 LLM 对话上下文存储 + 过期清理线程（AgentGraph 构造依赖，先建）
    deepagent::NodeContextStore nodeContexts(rootDir + "\\output\\node_contexts.json");
    if (!nodeContexts.load(&err)) deepagent::Logf("[WARN] %s", err.c_str());
    deepagent::LlmClient llm(config);
    deepagent::UdrtCompiler compiler(catalog);
    deepagent::AgentGraph agent(config, llm, knowledge, catalog, compiler, nodeContexts);
    deepagent::ChatStore chats(config.chats_file);
    if (!chats.load(&err)) deepagent::Logf("[WARN] %s", err.c_str());
    deepagent::Logf("chats file   : %s", config.chats_file.c_str());

    std::atomic<bool> purgeStop{false};
    std::thread purgeThread([&nodeContexts, &config, &purgeStop]() {
        nodeContexts.purgeExpired(config.ctx_retention_days);
        while (!purgeStop.load()) {
            for (int i = 0; i < 1800 && !purgeStop.load(); ++i)
                Sleep(1000);   // 每 30 分钟扫描一次，可被退出打断
            if (purgeStop.load()) break;
            nodeContexts.purgeExpired(config.ctx_retention_days);
        }
    });

    deepagent::HttpServer server(config, chats, knowledge, agent, nodeContexts, llm);

    // 5. 托盘：左键/双击 = 打开对话；右键 = 菜单
    std::string chatUrl = chatUrlOf(config);
    deepagent::TrayCallbacks trayCb;
    trayCb.onOpenChat = [&config]() { openChatInBrowser(config); };
    trayCb.onOpenUdrtDir = [&config]() {
        ShellExecuteW(nullptr, L"open", utf8ToWide(config.udrt_output_dir).c_str(),
                      nullptr, nullptr, SW_SHOWNORMAL);
    };
    trayCb.onExit = []() { PostQuitMessage(0); };

    std::wstring tip = L"DeepAgent · Graph_Saturn AI 助手\n" + utf8ToWide(chatUrl);
    if (!deepagent::tray::Create(hInstance, tip, trayCb)) {
        deepagent::Logf("[FATAL] tray icon creation failed");
        if (mutex) CloseHandle(mutex);
        return 1;
    }

    // 6. HTTP 服务器（独立线程）
    std::thread serverThread([&server, &config]() {
        if (!server.run()) {
            deepagent::Logf("[FATAL] failed to bind %s:%d", config.server_host.c_str(),
                            config.server_port);
            deepagent::tray::ShowErrorBalloon(
                L"DeepAgent 启动失败",
                utf8ToWide("端口 " + std::to_string(config.server_port) +
                           " 被占用，无法启动服务"));
            PostQuitMessage(1);
        }
    });

    // 7. 消息泵；退出时停清理线程与服务器
    deepagent::Logf("listening on  : %s (tray mode)", chatUrl.c_str());
    deepagent::tray::RunMessageLoop();

    purgeStop = true;
    if (purgeThread.joinable()) purgeThread.join();
    server.stop();
    if (serverThread.joinable()) serverThread.join();
    deepagent::Logf("server stopped, exiting");
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return 0;
}
