#pragma once

#include <string>

#include "agent_graph.h"
#include "chat_store.h"
#include "config.h"
#include "knowledge_base.h"
#include "node_context.h"

namespace httplib {
struct Server;
struct Request;
struct Response;
} // namespace httplib

namespace deepagent {

class LlmClient;

// HTTP frontend: static web UI + REST/SSE API (cpp-httplib).
class HttpServer {
public:
    HttpServer(const Config& config, ChatStore& chats, const KnowledgeBase& kb,
               AgentGraph& agent, NodeContextStore& nodeContexts, LlmClient& llm);
    ~HttpServer();

    // Binds and serves forever; returns false on bind failure.
    bool run();

    // Stops a running server (safe to call from another thread).
    void stop();

private:
    void registerHandlers();
    void handleChatStream(const httplib::Request& req, httplib::Response& res);
    void handleNodeGenerate(const std::string& guid, const httplib::Request& req,
                            httplib::Response& res);

    const Config& config_;
    ChatStore& chats_;
    const KnowledgeBase& kb_;
    AgentGraph& agent_;
    NodeContextStore& nodeContexts_;
    LlmClient& llm_;
    httplib::Server* server_ = nullptr;
};

} // namespace deepagent
