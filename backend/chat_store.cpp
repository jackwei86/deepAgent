#include "chat_store.h"

#include <algorithm>
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

// 原子替换写：先写 .tmp 再 MoveFileEx 替换，避免写一半崩溃损坏正式文件
bool atomicWriteFile(const std::string& path, const std::string& content) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), (int)path.size(), nullptr, 0);
    std::wstring wpath(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), (int)path.size(), &wpath[0], wlen);
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

ChatStore::ChatStore(std::string filePath) : file_path_(std::move(filePath)) {}

bool ChatStore::load(std::string* error) {
    std::ifstream in(file_path_, std::ios::binary);
    if (!in.is_open()) return true;   // 首次运行：无文件即空开始
    json j;
    try {
        j = json::parse(in);
    } catch (const std::exception& e) {
        // 备份损坏文件后空开始，不阻塞服务启动；
        // Windows 移动打开中的文件会失败，必须先释放句柄
        in.close();
        int wlen = MultiByteToWideChar(CP_UTF8, 0, file_path_.c_str(),
                                       (int)file_path_.size(), nullptr, 0);
        std::wstring wpath(wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, file_path_.c_str(), (int)file_path_.size(),
                            &wpath[0], wlen);
        std::string detail = std::string("chats file corrupted: ") + e.what();
        if (!MoveFileExW(wpath.c_str(), (wpath + L".bad").c_str(),
                         MOVEFILE_REPLACE_EXISTING)) {
            detail += " (backup to .bad failed, file left in place)";
        }
        if (error) *error = detail;
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& item : j.value("chats", json::array())) {
        Chat chat;
        chat.id = item.value("id", "");
        chat.title = item.value("title", "新会话");
        chat.created_at = item.value("created_at", 0LL);
        chat.archived = item.value("archived", false);
        chat.archivedAtMs = item.value("archivedAtMs", 0LL);
        chat.messages = item.value("messages", json::array());
        if (chat.id.empty()) continue;
        chats_.emplace(chat.id, std::move(chat));
    }
    // 恢复 next_seq_：取现存最大序号 + 1，避免重启后新 id 与旧 id 冲突
    for (const auto& [id, chat] : chats_) {
        try {
            size_t pos = id.find('_');
            if (pos != std::string::npos) {
                long long seq = std::stoll(id.substr(1, pos - 1));
                if (seq >= next_seq_) next_seq_ = seq + 1;
            }
        } catch (const std::exception&) {
            // 非 "c<seq>_<ts>" 形态的 id 跳过
        }
    }
    return true;
}

void ChatStore::saveLocked() const {
    json arr = json::array();
    for (const auto& [id, chat] : chats_) {
        arr.push_back({{"id", chat.id},
                       {"title", chat.title},
                       {"created_at", chat.created_at},
                       {"archived", chat.archived},
                       {"archivedAtMs", chat.archivedAtMs},
                       {"messages", chat.messages}});
    }
    json doc;
    doc["version"] = 1;
    doc["saved_at"] = nowMs();
    doc["chats"] = arr;
    atomicWriteFile(file_path_, doc.dump());
}

std::string ChatStore::createChat() {
    std::lock_guard<std::mutex> lock(mutex_);
    Chat chat;
    chat.id = "c" + std::to_string(next_seq_) + "_" + std::to_string(nowMs());
    chat.title = "新会话";
    chat.created_at = nowMs();
    std::string id = chat.id;
    chats_.emplace(id, std::move(chat));
    ++next_seq_;
    saveLocked();
    return id;
}

ChatStore::Chat* ChatStore::findLocked(const std::string& id) {
    auto it = chats_.find(id);
    return it == chats_.end() ? nullptr : &it->second;
}

nlohmann::json ChatStore::listChats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    json arr = json::array();
    for (const auto& [id, chat] : chats_) {
        arr.push_back({{"id", chat.id},
                       {"title", chat.title},
                       {"created_at", chat.created_at},
                       {"archived", chat.archived},
                       {"archivedAtMs", chat.archivedAtMs}});
    }
    // 稳定呈现顺序：按创建时间降序（最近会话在上）
    std::sort(arr.begin(), arr.end(), [](const json& a, const json& b) {
        return a["created_at"].get<long long>() > b["created_at"].get<long long>();
    });
    return arr;
}

nlohmann::json ChatStore::getChat(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = chats_.find(id);
    if (it == chats_.end()) return nullptr;
    const Chat& chat = it->second;
    return {{"id", chat.id},
            {"title", chat.title},
            {"created_at", chat.created_at},
            {"archived", chat.archived},
            {"archivedAtMs", chat.archivedAtMs},
            {"messages", chat.messages}};
}

bool ChatStore::removeChat(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool removed = chats_.erase(id) > 0;
    if (removed) saveLocked();
    return removed;
}

bool ChatStore::archiveChat(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    Chat* chat = findLocked(id);
    if (!chat || chat->archived) return false;
    chat->archived = true;
    chat->archivedAtMs = nowMs();
    saveLocked();
    return true;
}

bool ChatStore::unarchiveChat(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    Chat* chat = findLocked(id);
    if (!chat || !chat->archived) return false;
    chat->archived = false;
    chat->archivedAtMs = 0;
    saveLocked();
    return true;
}

void ChatStore::appendUserMessage(const std::string& id, const std::string& content) {
    std::lock_guard<std::mutex> lock(mutex_);
    Chat* chat = findLocked(id);
    if (!chat) return;
    if (chat->archived) {
        // 归档会话继续对话 = 自动取消归档，回到主列表
        chat->archived = false;
        chat->archivedAtMs = 0;
    }
    chat->messages.push_back({{"role", "user"}, {"content", content}});
    if (chat->title == "新会话" && !content.empty()) {
        chat->title = content.substr(0, content.size() > 24 ? 24 : content.size());
    }
    saveLocked();
}

void ChatStore::appendAssistantMessage(const std::string& id, const std::string& content) {
    std::lock_guard<std::mutex> lock(mutex_);
    Chat* chat = findLocked(id);
    if (!chat) return;
    chat->messages.push_back({{"role", "assistant"}, {"content", content}});
    saveLocked();
}

std::vector<ChatMessage> ChatStore::historyFor(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ChatMessage> out;
    auto it = chats_.find(id);
    if (it == chats_.end()) return out;
    for (const auto& m : it->second.messages) {
        out.push_back({m.value("role", ""), m.value("content", "")});
    }
    return out;
}

} // namespace deepagent
