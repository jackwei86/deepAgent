#pragma once

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "llm_client.h"
#include "third_party/json.hpp"

namespace deepagent {

// Chat session store, persisted to a JSON file:
// loaded at startup, saved atomically (temp file + replace) after every change.
class ChatStore {
public:
    explicit ChatStore(std::string filePath);

    // Loads persisted chats. Missing file = clean start (true).
    // Corrupted file = backs it up as .bad and starts clean (false).
    bool load(std::string* error);

    // Creates a new chat and returns its id.
    std::string createChat();

    nlohmann::json listChats() const;          // [{id,title,created_at}] oldest first
    nlohmann::json getChat(const std::string& id) const; // full record or null

    // Removes a chat; returns true when the chat existed.
    bool removeChat(const std::string& id);

    // 归档/取消归档（软状态）：归档不删除任何数据，仅从主列表移入"已归档"视图。
    bool archiveChat(const std::string& id);
    bool unarchiveChat(const std::string& id);

    void appendUserMessage(const std::string& id, const std::string& content);
    void appendAssistantMessage(const std::string& id, const std::string& content);

    // Prior turns as LLM messages (oldest first, system prompt excluded).
    std::vector<ChatMessage> historyFor(const std::string& id) const;

private:
    struct Chat {
        std::string id;
        std::string title;
        long long created_at = 0;
        bool archived = false;              // 归档软状态：true = 已归档（数据完整保留）
        long long archivedAtMs = 0;
        nlohmann::json messages = nlohmann::json::array();
    };

    Chat* findLocked(const std::string& id);
    // Caller must hold mutex_. Serializes chats_ to file_path_ via temp file + replace.
    void saveLocked() const;

    std::string file_path_;
    mutable std::mutex mutex_;
    std::map<std::string, Chat> chats_;
    long long next_seq_ = 1;
};

} // namespace deepagent
