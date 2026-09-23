#include "http_server.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <iterator>

#include <windows.h>
#include <shellapi.h>

#include "asset_context.h"
#include "asset_pipeline.h"
#include "asset_runner.h"
#include "event_hub.h"
#include "log.h"
#include "process_launcher.h"
#include "third_party/httplib.h"
#include "third_party/json.hpp"

using json = nlohmann::json;
using namespace httplib;

namespace deepagent {
namespace {

// 只读文件会使 remove_all 失败：先递归清属性再删（工程副本可能继承源文件只读位）
void forceRemoveAll(const std::string& dir) {
    std::error_code ec;
    const std::filesystem::path root(dir);
    if (!std::filesystem::exists(root, ec)) return;
    for (auto it = std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code pec;
        if (it->is_regular_file(pec))
            std::filesystem::permissions(it->path(), std::filesystem::perms::all, pec);
    }
    std::filesystem::remove_all(root, ec);
}

void writeFrame(DataSink& sink, const std::string& event, const json& payload) {
    std::string frame = "event: " + event + "\ndata: " + payload.dump() + "\n\n";
    sink.write(frame.c_str(), frame.size());
}

// value() throws when the key exists with a non-matching type (e.g. null);
// read fields defensively — the frontend may send "chat_id": null.
std::string safeString(const json& body, const char* key) {
    auto it = body.find(key);
    if (it == body.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

// 枚举资源管理器窗口（CabinetWClass）辅助状态
struct EnumScanState {
    const std::set<HWND>* exclude;   // 打开前快照（排除集合；可空）
    std::wstring titleNeedle;        // 标题需包含（复用导航场景）；空 = 不过滤
    HWND found;
};

static BOOL CALLBACK scanCabinetProc(HWND hwnd, LPARAM lp) {
    auto* st = (EnumScanState*)lp;
    if (!IsWindowVisible(hwnd)) return TRUE;
    char cls[64] = {};
    if (GetClassNameA(hwnd, cls, sizeof(cls)) && _stricmp(cls, "CabinetWClass") == 0) {
        if (st->exclude && st->exclude->count(hwnd)) return TRUE;
        wchar_t title[256] = {};
        GetWindowTextW(hwnd, title, 256);
        if (st->titleNeedle.empty() ||
            std::wstring(title).find(st->titleNeedle) != std::wstring::npos) {
            st->found = hwnd;
            return FALSE;
        }
    }
    return TRUE;
}

static void bringToFront(HWND hwnd) {
    // 置顶（TOPMOST 保持）：窗口始终悬浮在最前，直到用户关闭或手动取消
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd);
}

} // namespace

HttpServer::HttpServer(const Config& config, ChatStore& chats, const KnowledgeBase& kb,
                       AgentGraph& agent, NodeContextStore& nodeContexts, LlmClient& llm)
    : config_(config), chats_(chats), kb_(kb), agent_(agent),
      nodeContexts_(nodeContexts), llm_(llm) {
    server_ = new Server();
    // Node generate 请求会在 handler 内同步等待 LLM 完成（可达数十秒），
    // 调大读写超时避免长请求被服务端掐断
    server_->set_read_timeout(300, 0);
    server_->set_write_timeout(300, 0);
}

HttpServer::~HttpServer() { delete server_; }

bool HttpServer::run() {
    registerHandlers();
    // 静态文件强制 revalidate：前端改动即时生效（ETag 协商，未变更仍 304）
    httplib::Headers staticHeaders = { {"Cache-Control", "no-cache"} };
    if (!server_->set_mount_point("/", config_.frontend_dir, staticHeaders)) return false;
    return server_->listen(config_.server_host, config_.server_port);
}

void HttpServer::stop() {
    if (server_) server_->stop();
}

void HttpServer::registerHandlers() {
    server_->Get("/api/health", [this](const Request&, Response& res) {
        json body;
        body["status"] = "ok";
        body["llm_configured"] = config_.llmConfigured();
        body["llm_model"] = config_.llm_model;
        body["llm_base_url"] = config_.llm_base_url;
        body["knowledge_entries"] = kb_.entries().size();
        body["udrt_output_dir"] = config_.udrt_output_dir;
        res.set_content(body.dump(), "application/json; charset=utf-8");
    });

    server_->Get("/api/chats", [this](const Request&, Response& res) {
        res.set_content(chats_.listChats().dump(), "application/json; charset=utf-8");
    });

    server_->Get(R"(/api/chats/([0-9a-zA-Z_]+))", [this](const Request& req, Response& res) {
        json chat = chats_.getChat(req.matches[1].str());
        if (chat.is_null()) {
            res.status = 404;
            res.set_content(R"({"error":"chat not found"})", "application/json; charset=utf-8");
            return;
        }
        res.set_content(chat.dump(), "application/json; charset=utf-8");
    });

    server_->Delete(R"(/api/chats/([0-9a-zA-Z_]+))", [this](const Request& req, Response& res) {
        std::string delChatId = req.matches[1].str();
        if (!chats_.removeChat(delChatId)) {
            res.status = 404;
            res.set_content(R"({"error":"chat not found"})", "application/json; charset=utf-8");
            return;
        }
        // 联动清理（V1.5.1）：删除上传文件、工程副本、该会话生成的资产产物与上下文
        // （工程副本可能继承源文件只读位，须先清属性再删）
        AssetContextStore ctxStore(config_.udrt_output_dir);
        for (const auto& artifact : ctxStore.removeByChat(delChatId, nullptr)) {
            if (artifact.empty()) continue;
            std::error_code ec2;
            std::filesystem::remove(artifact, ec2);
        }
        forceRemoveAll(config_.exe_dir + "\\uploads\\" + delChatId);
        forceRemoveAll(config_.exe_dir + "\\projects\\" + delChatId);
        res.set_content(R"({"deleted":true})", "application/json; charset=utf-8");
    });

    // 归档（软状态，数据完整保留，移入"已归档"视图）/ 取消归档
    server_->Post(R"(/api/chats/([0-9a-zA-Z_]+)/archive)", [this](const Request& req, Response& res) {
        if (!chats_.archiveChat(req.matches[1].str())) {
            res.status = 404;
            res.set_content(R"({"error":"chat not found"})", "application/json; charset=utf-8");
            return;
        }
        res.set_content(R"({"archived":true})", "application/json; charset=utf-8");
    });

    server_->Post(R"(/api/chats/([0-9a-zA-Z_]+)/unarchive)", [this](const Request& req, Response& res) {
        if (!chats_.unarchiveChat(req.matches[1].str())) {
            res.status = 404;
            res.set_content(R"({"error":"chat not found"})", "application/json; charset=utf-8");
            return;
        }
        res.set_content(R"({"unarchived":true})", "application/json; charset=utf-8");
    });

    server_->Get("/api/knowledge", [this](const Request&, Response& res) {
        json body;
        body["entries"] = kb_.entries();
        res.set_content(body.dump(), "application/json; charset=utf-8");
    });

    server_->Post("/api/chat/stream",
                  [this](const Request& req, Response& res) { handleChatStream(req, res); });

    // ===== 工程宿主进程启动（V1.5.0 R2）：CreateProcessW + 句柄表 =====

    // 手动启动：body {kind:"avatar"|"udrt", project:<绝对或相对路径>}
    server_->Post("/api/launch", [this](const Request& req, Response& res) {
        json body;
        try {
            body = json::parse(req.body);
        } catch (const std::exception&) {
            res.status = 400;
            res.set_content(R"({"error":"invalid JSON body"})", "application/json; charset=utf-8");
            return;
        }
        std::string kind = safeString(body, "kind");
        std::string project = safeString(body, "project");
        if (kind != "avatar" && kind != "udrt") {
            res.status = 400;
            res.set_content(R"({"error":"kind must be avatar or udrt"})",
                            "application/json; charset=utf-8");
            return;
        }
        if (project.empty()) {
            res.status = 400;
            res.set_content(R"({"error":"project is required"})",
                            "application/json; charset=utf-8");
            return;
        }
        // 相对路径按 exe_dir 解析；要求文件存在
        std::string p = project;
        for (auto& ch : p) if (ch == '/') ch = '\\';
        std::string abs = p;
        if (p.size() < 2 || p[1] != ':') abs = config_.exe_dir + "\\" + p;
        DWORD attr = GetFileAttributesW([&] {
            int wl = MultiByteToWideChar(CP_UTF8, 0, abs.c_str(), (int)abs.size(), nullptr, 0);
            std::wstring w(wl, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, abs.c_str(), (int)abs.size(), &w[0], wl);
            return w;
        }().c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            res.status = 404;
            json err{{"error", "project file not found: " + abs}};
            res.set_content(err.dump(), "application/json; charset=utf-8");
            return;
        }
        const std::string& exe = (kind == "avatar") ? config_.avatar_exe : config_.udrt_exe;
        DWORD pid = 0;
        std::string err;
        json out;
        if (ProcessLauncher::instance().launch(kind, exe, abs, &pid, &err)) {
            out["launched"] = true;
            out["pid"] = pid;
            out["exe"] = exe;
            out["project"] = abs;
        } else {
            out["launched"] = false;
            out["error"] = err;
        }
        res.set_content(out.dump(), "application/json; charset=utf-8");
    });

    // 启动记录快照
    server_->Get("/api/processes", [this](const Request&, Response& res) {
        json arr = json::array();
        for (const auto& info : ProcessLauncher::instance().snapshot()) {
            arr.push_back({{"kind", info.kind},
                           {"project", info.project},
                           {"exe", info.exe},
                           {"pid", info.pid},
                           {"running", info.running},
                           {"exit_code", info.exitCode},
                           {"error", info.error}});
        }
        json out{{"processes", arr}};
        res.set_content(out.dump(), "application/json; charset=utf-8");
    });

    // ===== 双向 IPC（V1.5.0 R3）：exe SSE 订阅 + 参数变更通知 =====

    // exe 订阅资产更新事件（SSE 长连接；project 过滤为空则收全部）
    server_->Get("/api/exe/events", [this](const Request& req, Response& res) {
        std::string project = req.get_param_value("project");
        auto sub = EventHub::instance().subscribe(project);
        res.set_header("Cache-Control", "no-cache");
        res.set_header("X-Accel-Buffering", "no");
        res.set_chunked_content_provider(
            "text/event-stream", [sub](size_t /*offset*/, DataSink& sink) -> bool {
                while (true) {
                    std::string event, data;
                    EventHub::PopResult r = sub->waitPop(&event, &data, 15000);
                    if (r == EventHub::PopResult::Closed) break;
                    if (r == EventHub::PopResult::Got) {
                        std::string frame =
                            "event: " + event + "\ndata: " + data + "\n\n";
                        if (!sink.write(frame.c_str(), frame.size())) break;
                    } else {
                        // 15s 无事件发 keepalive 注释帧，探测断连并防中间层超时
                        if (!sink.write(": keepalive\n\n", 13)) break;
                    }
                }
                sink.done();
                return true;
            });
    });

    // exe 参数变更通知 → 单节点重生成 → hub 推 asset_updated
    server_->Post("/api/exe/notify", [this](const Request& req, Response& res) {
        json body;
        try {
            body = json::parse(req.body);
        } catch (const std::exception&) {
            res.status = 400;
            res.set_content(R"({"error":"invalid JSON body"})", "application/json; charset=utf-8");
            return;
        }
        const std::string project = safeString(body, "project");
        const std::string source = safeString(body, "source");
        // V1.5.1: changes 带 guid 时 project 可空（索引直查）
        bool changesHaveGuid = false;
        if (body.contains("changes") && body["changes"].is_array())
            for (const auto& ch : body["changes"])
                if (ch.is_object() && !ch.value("guid", "").empty()) changesHaveGuid = true;
        if (project.empty() && !changesHaveGuid) {
            res.status = 400;
            res.set_content(R"({"error":"project is required"})",
                            "application/json; charset=utf-8");
            return;
        }
        if (!body.contains("changes") || !body["changes"].is_array() || body["changes"].empty()) {
            res.status = 400;
            res.set_content(R"({"error":"changes array is required"})",
                            "application/json; charset=utf-8");
            return;
        }
        json results = json::array();
        int accepted = 0;
        for (const auto& ch : body["changes"]) {
            std::string nodePtr = ch.value("node_ptr", "");
            const std::string nodeId = ch.value("node_id", "");
            const std::string chGuid = ch.value("guid", "");
            if (nodePtr.empty() && !chGuid.empty()) nodePtr = chGuid;  // guid 直查
            const std::string param = ch.value("param", "text_in");
            const std::string value = ch.value("value", "");
            if (param != "text_in") {
                results.push_back(json{{"node_ptr", nodePtr},
                                       {"param", param},
                                       {"ok", false},
                                       {"error", "unsupported param (V1.5.0 only text_in)"}});
                continue;
            }
            if (value.empty()) {
                results.push_back(json{{"node_ptr", nodePtr},
                                       {"param", param},
                                       {"ok", false},
                                       {"error", "empty value"}});
                continue;
            }
            std::string raw =
                agent_.regenerateAvatarNode(project, nodePtr, nodeId, value);
            json r = json::parse(raw, nullptr, false);
            const bool ok = r.is_object() && r.value("ok", false);
            if (ok) ++accepted;
            results.push_back(json{{"node_ptr", nodePtr},
                                   {"node_id", nodeId},
                                   {"param", param},
                                   {"ok", ok},
                                   {"result", r.is_object() ? r : json{{"raw", raw}}}});
        }
        json out{{"source", source}, {"accepted", accepted}, {"results", results}};
        res.set_content(out.dump(), "application/json; charset=utf-8");
    });

    // ===== Node 网关：LLM 请求统一经 DeepAgentBackend 代理 =====

    // 生成入口：guid 从路径取；body {system?, kind:"content"|"prompt", text}
    server_->Post(R"(/api/nodes/([^/]+)/generate)",
                  [this](const Request& req, Response& res) {
                      handleNodeGenerate(req.matches[1].str(), req, res);
                  });

    // 上下文管理协议
    server_->Post("/api/context/discard", [this](const Request& req, Response& res) {
        json body;
        try {
            body = json::parse(req.body);
        } catch (const std::exception&) {
            res.status = 400;
            res.set_content(R"({"error":"invalid JSON body"})", "application/json; charset=utf-8");
            return;
        }
        std::string guid = safeString(body, "guid");
        if (guid.empty()) {
            res.status = 400;
            res.set_content(R"({"error":"guid is required"})", "application/json; charset=utf-8");
            return;
        }
        nodeContexts_.discard(guid);
        res.set_content(R"({"discarded":true})", "application/json; charset=utf-8");
    });

    server_->Get(R"(/api/context/([^/]+))", [this](const Request& req, Response& res) {
        json ctx = nodeContexts_.getContext(req.matches[1].str());
        if (ctx.is_null()) {
            res.status = 404;
            res.set_content(R"({"error":"context not found"})", "application/json; charset=utf-8");
            return;
        }
        res.set_content(ctx.dump(), "application/json; charset=utf-8");
    });

    server_->Get("/api/contexts", [this](const Request&, Response& res) {
        json body;
        body["contexts"] = nodeContexts_.listContexts();
        body["retention_days"] = config_.ctx_retention_days;
        res.set_content(body.dump(), "application/json; charset=utf-8");
    });

    // 资产内容预览：返回生成的 HTML（供前端浏览器查看）。仅允许 output/udrt 目录内的文件。
    server_->Get("/api/asset/content", [this](const Request& req, Response& res) {
        std::string path = req.get_param_value("path");
        // 相对路径（协议约定）以公共部署目录（exe 目录）为基准解析
        if (path.size() >= 2 && path[1] != ':') {
            path = config_.exe_dir + "\\" + path;
        }
        // 规范化斜杠方向，避免正/反斜杠混用绕过或误判前缀
        for (auto& ch : path) if (ch == '/') ch = '\\';
        std::string base = config_.udrt_output_dir;
        for (auto& ch : base) if (ch == '/') ch = '\\';
        if (path.empty() || path.find("..") != std::string::npos ||
            path.find(base) != 0) {
            res.status = 403;
            res.set_content(R"({"error":"path not allowed"})", "application/json; charset=utf-8");
            return;
        }
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) {
            res.status = 404;
            res.set_content(R"({"error":"asset not found"})", "application/json; charset=utf-8");
            return;
        }
        std::string content((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
        res.set_content(content, "text/html; charset=utf-8");
    });

    // 打开生成资产所在的目录（本机托盘服务有权限；网页沙箱无法直接打开本地目录）。
    // body {"path": "文件或目录"}：文件 → 资源管理器打开所在目录并选中该文件；目录 → 直接打开。
    // 打开后将目标窗口置顶（TOPMOST 保持，直到用户切换应用）。
    server_->Post("/api/open_folder", [this](const Request& req, Response& res) {
        json body;
        try {
            body = json::parse(req.body);
        } catch (const std::exception&) {
            res.status = 400;
            res.set_content(R"({"error":"invalid JSON body"})", "application/json; charset=utf-8");
            return;
        }
        std::string pathUtf8 = safeString(body, "path");
        if (pathUtf8.empty()) {
            res.status = 400;
            res.set_content(R"({"error":"path is required"})", "application/json; charset=utf-8");
            return;
        }
        // 相对路径（协议约定）以公共部署目录（exe 目录）为基准解析
        if (pathUtf8.size() >= 2 && pathUtf8[1] != ':') {
            pathUtf8 = config_.exe_dir + "\\" + pathUtf8;
        }
        int wlen = MultiByteToWideChar(CP_UTF8, 0, pathUtf8.c_str(), (int)pathUtf8.size(), nullptr, 0);
        std::wstring wpath(wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, pathUtf8.c_str(), (int)pathUtf8.size(), &wpath[0], wlen);

        DWORD attr = GetFileAttributesW(wpath.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) {
            res.status = 404;
            res.set_content(R"({"error":"path not found"})", "application/json; charset=utf-8");
            return;
        }

        // 记录打开前已有的资源管理器窗口，用于识别新窗口并置顶
        std::set<HWND> before;
        EnumScanState stBefore{&before, L"", nullptr};
        EnumWindows(scanCabinetProc, (LPARAM)&stBefore);

        bool opened = false;
        if (attr & FILE_ATTRIBUTE_DIRECTORY) {
            opened = (uintptr_t)ShellExecuteW(nullptr, L"open", wpath.c_str(),
                                              nullptr, nullptr, SW_SHOWNORMAL) > 32;
        } else {
            // 打开所在目录并选中文件
            std::wstring params = L"/select,\"" + wpath + L"\"";
            opened = (uintptr_t)ShellExecuteW(nullptr, L"open", L"explorer.exe",
                                              params.c_str(), nullptr, SW_SHOWNORMAL) > 32;
        }
        Logf("[open_folder] path=%s opened=%d", pathUtf8.c_str(), opened ? 1 : 0);

        // 等待目标窗口出现（新开或复用导航）并置顶（TOPMOST 保持，最多 3 秒）
        if (opened) {
            std::wstring dirLeaf = wpath;
            {
                size_t pos = dirLeaf.find_last_of(L"\\/");
                if (pos != std::wstring::npos) dirLeaf = dirLeaf.substr(0, pos);
                size_t p2 = dirLeaf.find_last_of(L"\\/");
                if (p2 != std::wstring::npos) dirLeaf = dirLeaf.substr(p2 + 1);
            }
            HWND target = nullptr;
            for (int i = 0; i < 30 && !target; ++i) {
                Sleep(100);
                EnumScanState st{&before, dirLeaf, nullptr};
                EnumWindows(scanCabinetProc, (LPARAM)&st);
                target = st.found;
            }
            if (target) {
                bringToFront(target);
                Logf("[open_folder] brought to front, title contains '%s'",
                     std::string(dirLeaf.begin(), dirLeaf.end()).c_str());
            }
        }

        res.set_content(R"({"opened":true})", "application/json; charset=utf-8");
    });
}

// Node 生成入口：更新上下文 → 同步调 LLM → assistant 回写上下文 → 返回结果。
// 响应即结果（LLM 可能耗时数十秒，服务端已调大超时）。
void HttpServer::handleNodeGenerate(const std::string& guid, const Request& req,
                                    Response& res) {
    json body;
    try {
        body = json::parse(req.body);
    } catch (const std::exception& e) {
        res.status = 400;
        res.set_content(R"({"error":"invalid JSON body"})", "application/json; charset=utf-8");
        return;
    }
    std::string kind = safeString(body, "kind");
    std::string text = safeString(body, "text");
    std::string system = safeString(body, "system");
    if (kind != "content" && kind != "prompt") kind = "content";
    if (text.empty()) {
        res.status = 400;
        res.set_content(R"({"error":"text is empty"})", "application/json; charset=utf-8");
        return;
    }

    json messages;
    std::string err;
    if (!nodeContexts_.beginTurn(guid, kind, system, text, &messages, &err)) {
        res.status = 409;
        json body{{"state", "busy"}, {"error", err}};
        res.set_content(body.dump(), "application/json; charset=utf-8");
        return;
    }

    // 子步管线执行（可选背景板分支 ∥ LLM 内容分支 → 合成；body.background 控制）
    std::optional<bool> background;
    if (body.contains("background") && body["background"].is_boolean())
        background = body["background"].get<bool>();

    AssetRunnerConfig rc;
    rc.api_key = config_.llm_api_key;
    rc.base_url = config_.llm_base_url;
    rc.model = config_.llm_model;
    rc.system_prompt = system;
    AssetGraphRunner runner(rc);

    AssetRequest assetReq;
    assetReq.guid = guid;
    assetReq.short_id = guid.size() >= 8 ? guid.substr(0, 8) : guid;
    assetReq.messages = messages;
    assetReq.context = json{{"content", text}, {"style", kind == "prompt" ? text : ""}};
    assetReq.wants_background = wantsBackground(background, text, "");

    auto results = runner.run({std::move(assetReq)}, defaultSubtitlePipeline(), nullptr);
    const auto& r = results.front();
    if (!r.ok || r.final_html.empty()) {
        const std::string fail = r.error.empty() ? "empty pipeline result" : r.error;
        nodeContexts_.failTurn(guid, fail);
        res.status = 502;
        json body{{"state", "failed"}, {"error", fail}};
        res.set_content(body.dump(), "application/json; charset=utf-8");
        return;
    }

    int version = 0;
    {
        json ctx = nodeContexts_.getContext(guid);
        if (ctx.is_object()) version = ctx.value("version", 0);
    }
    nodeContexts_.completeTurn(guid, r.final_html);

    json result{{"state", "done"}, {"html", r.final_html}, {"version", version + 1}, {"guid", guid}};
    res.set_content(result.dump(), "application/json; charset=utf-8");
}

void HttpServer::handleChatStream(const Request& req, Response& res) {
    Logf("[chat] request body_len=%zu", req.body.size());
    json body;
    try {
        body = json::parse(req.body);
    } catch (const std::exception& e) {
        Logf("[chat] parse error: %s", e.what());
        res.status = 400;
        res.set_content(R"({"error":"invalid JSON body"})", "application/json; charset=utf-8");
        return;
    }
    std::string message = safeString(body, "message");
    if (message.empty()) {
        Logf("[chat] message empty");
        res.status = 400;
        res.set_content(R"({"error":"message is empty"})", "application/json; charset=utf-8");
        return;
    }
    std::string chatId = safeString(body, "chat_id");
    {
        json existing = chats_.getChat(chatId);
        if (existing.is_null()) chatId = chats_.createChat();
    }

    // 附件：[{filename, content}]——工程文件(.udrt/.avatar)存 projects\{chat_id}，
    // 普通附件存 uploads\{chat_id}；内容/路径按"按需发送"原则注入用户消息
    std::string attachmentContext;
    {
        json attachArr = body.value("attachments", json::array());
        if (attachArr.is_array() && !attachArr.empty()) {
            for (const auto& a : attachArr) {
                std::string fname = a.value("filename", "");
                std::string content = a.value("content", "");
                if (fname.empty() || content.empty()) continue;
                // 安全：剥路径只取文件名
                size_t slash = fname.find_last_of("\\/");
                if (slash != std::string::npos) fname = fname.substr(slash + 1);
                // V1.5.1: 工程文件（.udrt/.avatar）全量复制到 projects\{chat_id}——
                // 副本即唯一工作文件（源文件与项目解耦）；普通附件仍存 uploads
                bool isProject =
                    fname.size() > 6 && fname.substr(fname.size() - 6) == ".udrt";
                if (fname.size() > 7 && fname.substr(fname.size() - 7) == ".avatar")
                    isProject = true;
                const std::string dir = config_.exe_dir + "\\" +
                                        (isProject ? "projects\\" : "uploads\\") + chatId;
                std::error_code ec2;
                std::filesystem::create_directories(dir, ec2);
                std::string fpath = dir + "\\" + fname;
                std::ofstream ofs(fpath, std::ios::binary | std::ios::trunc);
                if (ofs) { ofs << content; }
                // 告知 LLM 保存路径（工程文件的工具调用按需使用；.avatar 内容不注入）
                bool isAvatar = fname.size() > 7 && fname.substr(fname.size() - 7) == ".avatar";
                if (!isAvatar)
                    attachmentContext += "\n\n--- 附件: " + fname + " ---\n" +
                                         content.substr(0, 12000);
                attachmentContext += "\n\n[附件 " + fname + " 已保存至: " + fpath + "]";
                Logf("[upload] saved %s (%zu bytes) to %s", fname.c_str(), content.size(), fpath.c_str());
            }
        }
    }

    // 聊天中输入 .udrt/.xml/.avatar 路径：自动读取文件内容注入用户消息
    {
        size_t searchPos = 0;
        while (searchPos < message.size()) {
            size_t dot = message.find('.', searchPos);
            if (dot == std::string::npos) break;
            size_t extEnd = dot;
            while (extEnd < message.size() &&
                   (isalnum((unsigned char)message[extEnd]) || message[extEnd] == '.' ||
                    message[extEnd] == '_' || message[extEnd] == '-' ||
                    message[extEnd] == '\\' || message[extEnd] == '/' || message[extEnd] == ':'))
                ++extEnd;
            std::string candidate = message.substr(searchPos, extEnd - searchPos);
            bool isUdrt = candidate.size() > 5 && candidate.substr(candidate.size() - 5) == ".udrt";
            bool isXml  = candidate.size() > 4 && candidate.substr(candidate.size() - 4) == ".xml";
            bool isAvat = candidate.size() > 7 && candidate.substr(candidate.size() - 7) == ".avatar";
            if ((isUdrt || isXml || isAvat) &&
                (candidate.find('\\') != std::string::npos || candidate.find('/') != std::string::npos)) {
                // 尝试读取文件
                int wl = MultiByteToWideChar(CP_UTF8, 0, candidate.c_str(), (int)candidate.size(), nullptr, 0);
                std::wstring wp(wl, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, candidate.c_str(), (int)candidate.size(), &wp[0], wl);
                DWORD attr = GetFileAttributesW(wp.c_str());
                if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                    std::ifstream ifs(wp.c_str(), std::ios::binary);
                    if (ifs) {
                        std::string fileContent((std::istreambuf_iterator<char>(ifs)),
                                                std::istreambuf_iterator<char>());
                        if (!fileContent.empty()) {
                            // V1.5.1: 工程文件(.udrt/.avatar)全量复制到 projects\{chat_id}——
                            // 副本即唯一工作文件（源文件与项目解耦）；.xml 仍存 uploads
                            const std::string baseDir = config_.exe_dir + "\\" +
                                (isAvat || isUdrt ? "projects\\" : "uploads\\") + chatId;
                            std::error_code fsec;
                            std::filesystem::create_directories(baseDir, fsec);
                            size_t slash = candidate.find_last_of("\\/");
                            std::string baseName = (slash != std::string::npos) ? candidate.substr(slash + 1) : candidate;
                            std::string copyPath = baseDir + "\\" + baseName;
                            std::ofstream cp(copyPath, std::ios::binary | std::ios::trunc);
                            if (cp) { cp << fileContent; }
                            // 按需发送：.avatar 内容不注入（解析在工具内完成，只给副本路径）；
                            // .udrt/.xml 内容注入（截断 12000 字节，供分析类回复）
                            if (!isAvat)
                                attachmentContext += "\n\n--- 文件: " + candidate + " ---\n" + fileContent.substr(0, 12000);
                            attachmentContext += "\n\n[文件 " + baseName + " 已复制到工作目录: " + copyPath + "]";
                            Logf("[path_input] auto-read %s (%zu bytes) -> %s",
                                 candidate.c_str(), fileContent.size(), copyPath.c_str());
                        }
                    }
                }
            }
            searchPos = extEnd;
        }
    }

    if (!attachmentContext.empty()) {
        message += "\n\n[以下是用户附加的文件内容，请分析并据此回答：]\n" + attachmentContext;
    }

    res.set_header("Cache-Control", "no-cache");
    res.set_header("X-Accel-Buffering", "no");
    res.set_chunked_content_provider(
        "text/event-stream; charset=utf-8",
        [this, message, chatId](size_t /*offset*/, DataSink& sink) {
            // this lambda runs once: execute the agent and stream events
            std::vector<ChatMessage> history = chats_.historyFor(chatId);
            chats_.appendUserMessage(chatId, message);

            AgentGraph::EventSink eventSink;
            eventSink.onStatus = [&sink](const std::string& description) {
                writeFrame(sink, "status", json{{"description", description}});
            };
            eventSink.onToken = [&sink](const std::string& delta) {
                writeFrame(sink, "token", json{{"delta", delta}});
            };
            eventSink.onUdrt = [&sink](const json& payload) {
                writeFrame(sink, "udrt", payload);
            };
            eventSink.onAsset = [&sink](const json& payload) {
                writeFrame(sink, "asset", payload);
            };
            eventSink.onNodeState = [&sink](const json& payload) {
                writeFrame(sink, "node_state", payload);
            };

            std::string err;
            std::string reply = agent_.run(message, history, eventSink, &err, chatId);

            if (reply.empty()) {
                if (err.empty()) err = "empty reply";
                writeFrame(sink, "error", json{{"message", err}});
            } else {
                chats_.appendAssistantMessage(chatId, reply);
                writeFrame(sink, "done", json{{"chat_id", chatId}, {"reply", reply}});
            }
            sink.done();       // emit the chunked terminator
            return true;       // finish the chunked response
        });
    // expose the resolved chat id early via header for the frontend
    res.set_header("X-Chat-Id", chatId);
}

} // namespace deepagent
