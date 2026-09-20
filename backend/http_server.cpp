#include "http_server.h"

#include <fstream>
#include <set>
#include <sstream>
#include <iterator>

#include <windows.h>
#include <shellapi.h>

#include "log.h"
#include "third_party/httplib.h"
#include "third_party/json.hpp"

using json = nlohmann::json;
using namespace httplib;

namespace deepagent {
namespace {

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
        if (!chats_.removeChat(req.matches[1].str())) {
            res.status = 404;
            res.set_content(R"({"error":"chat not found"})", "application/json; charset=utf-8");
            return;
        }
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

    std::vector<ChatMessage> llmMessages;
    for (const auto& m : messages) {
        llmMessages.push_back({m.value("role", ""), m.value("content", "")});
    }
    std::string llmErr;
    std::string reply = llm_.invoke(llmMessages, &llmErr);
    if (reply.empty()) {
        nodeContexts_.failTurn(guid, llmErr.empty() ? "empty LLM reply" : llmErr);
        res.status = 502;
        json body{{"state", "failed"}, {"error", llmErr}};
        res.set_content(body.dump(), "application/json; charset=utf-8");
        return;
    }

    int version = 0;
    {
        json ctx = nodeContexts_.getContext(guid);
        if (ctx.is_object()) version = ctx.value("version", 0);
    }
    nodeContexts_.completeTurn(guid, reply);

    json result{{"state", "done"}, {"html", reply}, {"version", version + 1}, {"guid", guid}};
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

            std::string err;
            std::string reply = agent_.run(message, history, eventSink, &err);

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
