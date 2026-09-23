#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "third_party/json.hpp"

namespace deepagent {

// 工程资产上下文（V1.5.1）：GUID → 重生成所需的完整操作参数。
// 约定：sourceFile 是 projects\{chat_id}\ 下的工程副本——唯一工作文件
// （源文件已与项目解耦，不记录原始路径）。
//
// 存储：
//   - 按工程 manifest：udrt\{stem}.assets.json
//     顶层 {project_file(副本), stem, chat_id, updated_at, assets[]}
//   - 全局索引：udrt\asset_index.json
//     guid → {manifest, project, html_file, chat_id}
// 全部接口线程安全（内部互斥；文件写入各自完整覆盖）。
struct AssetContext {
    std::string guid;
    std::string nodePtr;
    std::string nodeId;
    std::string kind;          // avatar_text | udrt_subtitle
    std::string sourceFile;    // 工程副本路径（唯一工作文件）
    std::string entryId;       // 知识库条目（决定管线）
    std::string stylePrompt;
    bool background = false;
    std::string text;          // 当前字幕文本
    std::string htmlFile;      // 产物绝对路径
    int version = 0;
    std::string chatId;
    nlohmann::json history = nlohmann::json::array();  // [{version, text, reason, source, at}]
};

class AssetContextStore {
public:
    explicit AssetContextStore(std::string udrtDir) : udrtDir_(std::move(udrtDir)) {}

    // 记录新上下文或按 guid 更新既有条目（写 manifest + 索引）。
    // manifest 顶层记录 chat_id / project_file。
    bool record(const AssetContext& ctx, std::string* error);

    // 按 GUID 查（索引 → manifest）
    bool findByGuid(const std::string& guid, AssetContext* out, std::string* error) const;

    // 按产物 html 路径查（质检场景：review_asset 附带样式切片）
    bool findByHtml(const std::string& htmlAbs, AssetContext* out, std::string* error) const;

    // 追加历史并 version+1、更新当前文本（重生成/优化后调用）。
    // source 可空（如 "avatar.exe"）。
    bool appendHistory(const std::string& guid, const std::string& reason,
                       const std::string& text, const std::string& source,
                       std::string* error);

    // 删除某会话的全部上下文：清索引条目、删关联 manifest 文件，
    // 返回该会话生成的资产产物文件路径列表（供调用方删除产物）。
    std::vector<std::string> removeByChat(const std::string& chatId, std::string* error);

private:
    std::string manifestPathFor(const std::string& projectFile) const;  // stem → manifest 路径
    bool readJson(const std::string& path, nlohmann::json* out, std::string* error) const;
    bool writeJson(const std::string& path, const nlohmann::json& j, std::string* error) const;

    std::string udrtDir_;
    mutable std::mutex mutex_;
};

} // namespace deepagent
