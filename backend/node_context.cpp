#include "node_context.h"

#include <chrono>
#include <fstream>
#include <utility>

#include <windows.h>

using json = nlohmann::json;

namespace deepagent {
namespace {

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], size);
    return w;
}

// 原子替换写：先写 .tmp 再 MoveFileEx 替换，避免写一半崩溃损坏正式文件
bool atomicWriteFile(const std::string& path, const std::string& content) {
    std::wstring wpath = utf8ToWide(path);
    std::wstring tmp = wpath + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return false;
        out.write(content.data(), (std::streamsize)content.size());
        out.close();
        if (!out.good()) return false;
    }
    return MoveFileExW(tmp.c_str(), wpath.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

} // namespace

NodeContextStore::NodeContextStore(std::string filePath)
    : file_path_(std::move(filePath)) {}

bool NodeContextStore::load(std::string* error) {
    std::ifstream in(file_path_, std::ios::binary);
    if (!in.is_open()) return true;   // 首次运行：无文件即空开始
    json j;
    try {
        j = json::parse(in);
    } catch (const std::exception& e) {
        // 备份损坏文件后空开始，不阻塞服务启动；Windows 移动打开中的文件
        // 会失败，必须先关闭句柄
        in.close();
        std::wstring wpath = utf8ToWide(file_path_);
        MoveFileExW(wpath.c_str(), (wpath + L".bad").c_str(), MOVEFILE_REPLACE_EXISTING);
        if (error) *error = std::string("node contexts file corrupted (moved to .bad): ") + e.what();
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& item : j.value("contexts", json::array())) {
        Context ctx;
        ctx.guid = item.value("guid", "");
        if (ctx.guid.empty()) continue;
        ctx.state = item.value("state", "idle");
        ctx.systemPrompt = item.value("system", "");
        ctx.messages = item.value("messages", json::array());
        ctx.version = item.value("version", 0);
        ctx.discarded = item.value("discarded", false);
        ctx.discardedAtMs = item.value("discardedAtMs", 0LL);
        ctx.lastActivityMs = item.value("lastActivityMs", 0LL);
        ctx.lastError = item.value("lastError", "");
        contexts_.emplace(ctx.guid, std::move(ctx));
    }
    return true;
}

void NodeContextStore::saveLocked() const {
    json arr = json::array();
    for (const auto& [guid, ctx] : contexts_) {
        arr.push_back({{"guid", ctx.guid},
                       {"state", ctx.state},
                       {"system", ctx.systemPrompt},
                       {"messages", ctx.messages},
                       {"version", ctx.version},
                       {"discarded", ctx.discarded},
                       {"discardedAtMs", ctx.discardedAtMs},
                       {"lastActivityMs", ctx.lastActivityMs},
                       {"lastError", ctx.lastError}});
    }
    json doc;
    doc["version"] = 1;
    doc["saved_at"] = nowMs();
    doc["contexts"] = arr;
    atomicWriteFile(file_path_, doc.dump());
}

NodeContextStore::Context* NodeContextStore::findLocked(const std::string& guid) {
    auto it = contexts_.find(guid);
    return it == contexts_.end() ? nullptr : &it->second;
}

NodeContextStore::Context& NodeContextStore::getOrCreateLocked(const std::string& guid,
                                                               const std::string& system) {
    Context* ctx = findLocked(guid);
    if (!ctx) {
        Context fresh;
        fresh.guid = guid;
        fresh.systemPrompt = system;
        fresh.lastActivityMs = nowMs();
        return contexts_.emplace(guid, std::move(fresh)).first->second;
    }
    if (!system.empty() && ctx->systemPrompt.empty()) ctx->systemPrompt = system;
    return *ctx;
}

void NodeContextStore::touchLocked(Context& ctx) {
    ctx.lastActivityMs = nowMs();
    saveLocked();
}

bool NodeContextStore::beginTurn(const std::string& guid, const std::string& kind,
                                 const std::string& system, const std::string& text,
                                 json* messages, std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    Context& ctx = getOrCreateLocked(guid, system);

    if (ctx.discarded) {
        // 撤销删除场景：同 guid 重新 generate，恢复上下文继续有效
        ctx.discarded = false;
        ctx.discardedAtMs = 0;
    }
    if (ctx.state == "requesting") {
        if (error) *error = "a generation turn is already in progress for this guid";
        return false;
    }

    if (kind == "content") {
        // 新建/重建会话（新字幕文本）
        ctx.messages = json::array();
        ctx.version = 0;
        if (!system.empty()) ctx.systemPrompt = system;
    }

    ctx.messages.push_back({{"role", "user"}, {"content", text}});
    ctx.state = "requesting";
    ctx.lastError.clear();
    touchLocked(ctx);
    if (messages) {
        // 返回可直接发送的完整序列：system 常驻首位 + 全部历史
        json seq = json::array();
        if (!ctx.systemPrompt.empty())
            seq.push_back({{"role", "system"}, {"content", ctx.systemPrompt}});
        for (const auto& m : ctx.messages) seq.push_back(m);
        *messages = std::move(seq);
    }
    return true;
}

void NodeContextStore::completeTurn(const std::string& guid, const std::string& assistantContent) {
    std::lock_guard<std::mutex> lock(mutex_);
    Context* ctx = findLocked(guid);
    if (!ctx) return;
    ctx->messages.push_back({{"role", "assistant"}, {"content", assistantContent}});
    ++ctx->version;
    ctx->state = "done";
    touchLocked(*ctx);
}

void NodeContextStore::failTurn(const std::string& guid, const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    Context* ctx = findLocked(guid);
    if (!ctx) return;
    ctx->state = "failed";
    ctx->lastError = error;
    touchLocked(*ctx);
}

void NodeContextStore::discard(const std::string& guid) {
    std::lock_guard<std::mutex> lock(mutex_);
    Context& ctx = getOrCreateLocked(guid, "");
    if (!ctx.discarded) {
        ctx.discarded = true;
        ctx.discardedAtMs = nowMs();
    }
    touchLocked(ctx);
}

json NodeContextStore::listContexts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    json arr = json::array();
    for (const auto& [guid, ctx] : contexts_) {
        arr.push_back({{"guid", ctx.guid},
                       {"state", ctx.state},
                       {"version", ctx.version},
                       {"discarded", ctx.discarded},
                       {"message_count", ctx.messages.size()},
                       {"lastActivityMs", ctx.lastActivityMs},
                       {"lastError", ctx.lastError}});
    }
    return arr;
}

json NodeContextStore::getContext(const std::string& guid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = contexts_.find(guid);
    if (it == contexts_.end()) return nullptr;
    const Context& ctx = it->second;
    return {{"guid", ctx.guid},
            {"state", ctx.state},
            {"system", ctx.systemPrompt},
            {"messages", ctx.messages},
            {"version", ctx.version},
            {"discarded", ctx.discarded},
            {"discardedAtMs", ctx.discardedAtMs},
            {"lastActivityMs", ctx.lastActivityMs},
            {"lastError", ctx.lastError}};
}

int NodeContextStore::purgeExpired(int retentionDays) {
    long long cutoff = nowMs() - (long long)retentionDays * 24LL * 3600LL * 1000LL;
    std::lock_guard<std::mutex> lock(mutex_);
    int removed = 0;
    for (auto it = contexts_.begin(); it != contexts_.end();) {
        if (it->second.discarded && it->second.discardedAtMs < cutoff) {
            it = contexts_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    if (removed > 0) saveLocked();
    return removed;
}

} // namespace deepagent
