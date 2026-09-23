#include "asset_context.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <windows.h>

namespace deepagent {
namespace {

using json = nlohmann::json;

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string stemOf(const std::string& path) {
    return std::filesystem::path(path).stem().string();
}

// AssetContext → manifest assets[] 条目（含 history 的完整形态）
json ctxToJson(const AssetContext& ctx, const json* existing) {
    json e = existing && existing->is_object() ? *existing : json::object();
    e["guid"] = ctx.guid;
    if (!ctx.nodePtr.empty()) e["node_ptr"] = ctx.nodePtr;
    if (!ctx.nodeId.empty()) e["node_id"] = ctx.nodeId;
    e["kind"] = ctx.kind;
    e["entry_id"] = ctx.entryId;
    e["style_prompt"] = ctx.stylePrompt;
    e["background"] = ctx.background;
    e["text"] = ctx.text;
    e["html_file"] = ctx.htmlFile;
    e["version"] = ctx.version;
    if (!e.contains("history") || !e["history"].is_array()) e["history"] = json::array();
    return e;
}

// manifest 条目 → AssetContext
AssetContext jsonToCtx(const json& e, const std::string& manifestPath,
                       const std::string& chatId) {
    AssetContext c;
    c.guid = e.value("guid", "");
    c.nodePtr = e.value("node_ptr", "");
    c.nodeId = e.value("node_id", "");
    c.kind = e.value("kind", "");
    c.entryId = e.value("entry_id", "");
    c.stylePrompt = e.value("style_prompt", "");
    c.background = e.value("background", false);
    c.text = e.value("text", "");
    c.htmlFile = e.value("html_file", "");
    c.version = e.value("version", 0);
    c.chatId = chatId;
    if (e.contains("history") && e["history"].is_array()) c.history = e["history"];
    return c;
}

} // namespace

std::string AssetContextStore::manifestPathFor(const std::string& projectFile) const {
    return (std::filesystem::path(udrtDir_) / (stemOf(projectFile) + ".assets.json")).string();
}

bool AssetContextStore::readJson(const std::string& path, json* out, std::string* error) const {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        if (error) *error = "cannot open " + path;
        return false;
    }
    std::ostringstream ss;
    ss << ifs.rdbuf();
    try {
        *out = json::parse(ss.str());
    } catch (const std::exception& e) {
        if (error) *error = std::string("parse error in ") + path + ": " + e.what();
        return false;
    }
    return true;
}

bool AssetContextStore::writeJson(const std::string& path, const json& j, std::string* error) const {
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        if (error) *error = "cannot write " + path;
        return false;
    }
    ofs << j.dump(2);
    return true;
}

bool AssetContextStore::record(const AssetContext& ctx, std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };
    if (ctx.guid.empty() || ctx.sourceFile.empty() || ctx.htmlFile.empty())
        return fail("AssetContext record: guid/sourceFile/htmlFile 均必填");

    const std::string manifestFile = manifestPathFor(ctx.sourceFile);
    json m;
    // 兼容旧 manifest：读旧键 avatar_file 作为工程文件（V1.5.1 起语义=副本）
    if (!readJson(manifestFile, &m, nullptr)) m = json::object();
    if (!m.contains("assets") || !m["assets"].is_array()) m["assets"] = json::array();
    if (!m.contains("project_file")) m["project_file"] = m.value("avatar_file", ctx.sourceFile);
    m["project_file"] = ctx.sourceFile;
    m["stem"] = stemOf(ctx.sourceFile);
    if (!ctx.chatId.empty() || !m.value("chat_id", "").empty())
        m["chat_id"] = ctx.chatId.empty() ? m.value("chat_id", "") : ctx.chatId;
    m["updated_at"] = nowMs();

    json* target = nullptr;
    for (auto& it : m["assets"]) {
        if (it.value("guid", "") == ctx.guid) { target = &it; break; }
    }
    json entry = ctxToJson(ctx, target);
    if (!target) m["assets"].push_back(json::object());
    json& slot = target ? *target : m["assets"].back();
    slot = std::move(entry);
    // file（相对路径展示键）由调用方按需补；此处保证存在
    if (!slot.contains("file")) slot["file"] = slot.value("html_file", "");
    if (!writeJson(manifestFile, m, error)) return fail(*error);

    // 全局索引
    const std::string indexFile =
        (std::filesystem::path(udrtDir_) / "asset_index.json").string();
    json idx;
    if (!readJson(indexFile, &idx, nullptr)) idx = json::object();
    idx[ctx.guid] = {{"manifest", manifestFile},
                     {"project", ctx.sourceFile},
                     {"html_file", ctx.htmlFile},
                     {"chat_id", ctx.chatId}};
    return writeJson(indexFile, idx, error);
}

bool AssetContextStore::findByGuid(const std::string& guid, AssetContext* out,
                                   std::string* error) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };
    if (guid.empty()) return fail("guid is empty");
    const std::string indexFile =
        (std::filesystem::path(udrtDir_) / "asset_index.json").string();
    json idx;
    if (!readJson(indexFile, &idx, nullptr) || !idx.contains(guid))
        return fail("asset context not found for guid: " + guid);
    const json& ref = idx[guid];
    const std::string manifestFile = ref.value("manifest", "");
    json m;
    if (!readJson(manifestFile, &m, nullptr))
        return fail("manifest missing: " + manifestFile);
    for (const auto& it : m["assets"]) {
        if (it.value("guid", "") == guid) {
            if (out) *out = jsonToCtx(it, manifestFile, m.value("chat_id", ""));
            return true;
        }
    }
    return fail("manifest entry missing for guid: " + guid);
}

bool AssetContextStore::findByHtml(const std::string& htmlAbs, AssetContext* out,
                                   std::string* error) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };
    auto norm = [](std::string s) {
        for (auto& ch : s) if (ch == '/') ch = '\\';
        return s;
    };
    const std::string want = norm(htmlAbs);
    const std::string indexFile =
        (std::filesystem::path(udrtDir_) / "asset_index.json").string();
    json idx;
    if (!readJson(indexFile, &idx, nullptr))
        return fail("asset index missing");
    for (auto it = idx.begin(); it != idx.end(); ++it) {
        if (norm(it.value().value("html_file", "")) != want) continue;
        const std::string manifestFile = it.value().value("manifest", "");
        json m;
        if (!readJson(manifestFile, &m, nullptr))
            return fail("manifest missing: " + manifestFile);
        for (const auto& e : m["assets"]) {
            if (e.value("guid", "") != it.key()) continue;
            if (out) *out = jsonToCtx(e, manifestFile, m.value("chat_id", ""));
            return true;
        }
        return fail("manifest entry missing for guid: " + it.key());
    }
    return fail("asset context not found for html: " + htmlAbs);
}

bool AssetContextStore::appendHistory(const std::string& guid, const std::string& reason,
                                      const std::string& text, const std::string& source,
                                      std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };
    const std::string indexFile =
        (std::filesystem::path(udrtDir_) / "asset_index.json").string();
    json idx;
    if (!readJson(indexFile, &idx, nullptr) || !idx.contains(guid))
        return fail("asset context not found for guid: " + guid);
    const std::string manifestFile = idx[guid].value("manifest", "");
    json m;
    if (!readJson(manifestFile, &m, nullptr))
        return fail("manifest missing: " + manifestFile);
    for (auto& it : m["assets"]) {
        if (it.value("guid", "") != guid) continue;
        const int newVersion = it.value("version", 0) + 1;
        it["version"] = newVersion;
        it["text"] = text;
        if (!it.contains("history") || !it["history"].is_array()) it["history"] = json::array();
        it["history"].push_back({{"version", newVersion},
                                 {"text", text},
                                 {"reason", reason},
                                 {"source", source},
                                 {"at", nowMs()}});
        m["updated_at"] = nowMs();
        return writeJson(manifestFile, m, error);
    }
    return fail("manifest entry missing for guid: " + guid);
}

std::vector<std::string> AssetContextStore::removeByChat(const std::string& chatId,
                                                         std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> artifactFiles;
    if (chatId.empty()) return artifactFiles;
    const std::string indexFile =
        (std::filesystem::path(udrtDir_) / "asset_index.json").string();
    json idx;
    if (!readJson(indexFile, &idx, nullptr)) return artifactFiles;

    // 1. 收集该会话的产物文件与关联 manifest（去重）
    std::vector<std::string> manifests;
    for (auto it = idx.begin(); it != idx.end();) {
        if (it.value().value("chat_id", "") != chatId) { ++it; continue; }
        artifactFiles.push_back(it.value().value("html_file", ""));
        const std::string mf = it.value().value("manifest", "");
        if (!mf.empty() &&
            std::find(manifests.begin(), manifests.end(), mf) == manifests.end())
            manifests.push_back(mf);
        it = idx.erase(it);
    }
    writeJson(indexFile, idx, nullptr);

    // 2. 删除归属该会话的 manifest 文件（chat_id 匹配才删，避免误删混合清单）
    std::error_code ec;
    for (const auto& mf : manifests) {
        json m;
        if (readJson(mf, &m, nullptr) && m.value("chat_id", "") == chatId)
            std::filesystem::remove(mf, ec);
    }
    return artifactFiles;
}

} // namespace deepagent
