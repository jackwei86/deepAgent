#pragma once

#include <map>
#include <mutex>
#include <string>

#include "third_party/json.hpp"

namespace deepagent {

// 按 Node GUID 管理的 LLM 对话上下文（多轮）。
// - content 轮次：新建/重建会话（system + user(text)）
// - prompt  轮次：向既有会话追加 user(text)（多轮修改指令）
// - discard：仅标记（撤销删除后同 guid 再次 generate 自动恢复，历史继续有效）
// - 过期清理：discarded 超过 retentionDays 的记录物理删除
// JSON 落盘：变更即写 + 临时文件原子替换；损坏自动备份 .bad。
class NodeContextStore {
public:
    explicit NodeContextStore(std::string filePath);

    bool load(std::string* error);

    // 开始一轮：kind=="content" 重建会话，kind=="prompt" 追加指令。
    // 自动恢复 discarded 上下文；轮次进行中（requesting）返回 false（busy）。
    // 成功时返回完整消息序列（system + 历史 + 新 user），调用方据此调 LLM。
    bool beginTurn(const std::string& guid, const std::string& kind,
                   const std::string& system, const std::string& text,
                   nlohmann::json* messages, std::string* error);

    // LLM 完成：assistant 回复入上下文，version+1，state=done。
    void completeTurn(const std::string& guid, const std::string& assistantContent);

    // LLM 失败：state=failed，上下文保留（错误信息记录）。
    void failTurn(const std::string& guid, const std::string& error);

    // 标记 discard（不物理删除）；guid 不存在时也创建一条 discarded 空记录。
    void discard(const std::string& guid);

    nlohmann::json listContexts() const;             // 概要列表（不含消息全文）
    nlohmann::json getContext(const std::string& guid) const; // 完整上下文或 null

    // 物理删除 discarded 且超过 retentionDays 的记录；返回删除数量。
    int purgeExpired(int retentionDays);

private:
    struct Context {
        std::string guid;
        std::string state = "idle";          // idle/requesting/done/failed
        std::string systemPrompt;
        nlohmann::json messages = nlohmann::json::array(); // {role, content} 交替
        int version = 0;
        bool discarded = false;
        long long discardedAtMs = 0;
        long long lastActivityMs = 0;
        std::string lastError;
    };

    Context* findLocked(const std::string& guid);
    Context& getOrCreateLocked(const std::string& guid, const std::string& system);
    void touchLocked(Context& ctx);
    void saveLocked() const;

    std::string file_path_;
    mutable std::mutex mutex_;
    std::map<std::string, Context> contexts_;
};

} // namespace deepagent
