#include "agent_graph.h"

#include <bcrypt.h>
#include <fstream>
#include <string.h>
#include <sstream>

#include "asset_pipeline.h"
#include "asset_runner.h"
#include "log.h"
#include "plan_script.h"
#include "winhttp_provider.h"

#include <filesystem>

#include <neograph/llm/agent.h>
#include <neograph/llm/openai_provider.h>

#pragma comment(lib, "bcrypt.lib")

using json = nlohmann::json;

namespace deepagent {
namespace {

// 字幕 HTML 生成的 system 提示词（对话中直接生成 HTML 资产时使用）
const char* const kSubtitleSystemPrompt =
    "你是字幕排版助手。将用户提供的 markdown 字幕文本转换为可直接在浏览器打开的完整 HTML 文件："
    "输出以 <!DOCTYPE html> 开头的完整文档，样式全部内嵌在 <style> 中，"
    "默认白底黑字、水平垂直居中、字号适中、行距舒适。"
    "多轮对话时用户会提出字体大小、颜色等修改要求，请在最近一版 HTML 基础上修改后输出完整 HTML。"
    "只输出 HTML 代码本身，不要任何解释，不要 markdown 代码围栏。";

// LLM 可能用 ```html ... ``` 围栏包裹结果：剥围栏，无围栏则原样返回
std::string StripHtmlFence(const std::string& strContent)
{
    const size_t nFenceHead = strContent.find("```");
    if (nFenceHead == std::string::npos)
        return strContent;
    size_t nStart = strContent.find('\n', nFenceHead);
    if (nStart == std::string::npos)
        return strContent;
    ++nStart;
    size_t nEnd = strContent.find("```", nStart);
    if (nEnd == std::string::npos)
        nEnd = strContent.size();
    return strContent.substr(nStart, nEnd - nStart);
}

// Robustly extract a JSON object from LLM text (skips prose/markdown fences).
bool extractJsonObject(const std::string& text, json* out) {
    size_t begin = text.find('{');
    if (begin == std::string::npos) return false;
    size_t end = text.rfind('}');
    if (end == std::string::npos || end <= begin) return false;
    try {
        *out = json::parse(text.substr(begin, end - begin + 1));
        return out->is_object();
    } catch (const std::exception&) {
        return false;
    }
}

// P1: 校验脚本产出的 Plan IR 形态与 catalog 引用（编译器做全量校验，此处提前拦截
// 明显错误以便走自修复轮次）
bool planIrShapeOk(const json& plan, const NodeCatalog& catalog) {
    if (!plan.is_object() || !plan.contains("nodes") || !plan.contains("links"))
        return false;
    if (!plan["nodes"].is_array() || plan["nodes"].empty() || !plan["links"].is_array())
        return false;
    for (const auto& pn : plan["nodes"]) {
        if (!pn.is_object()) return false;
        if (pn.value("key", "").empty()) return false;
        if (!catalog.node(pn.value("catalog", ""))) return false;
    }
    return true;
}

// 资产路径解析与安全校验（与 /api/asset/content 同规则）：接受相对/绝对路径。
// 相对路径按协议以部署目录（exe_dir）为基准（如 "udrt\xxx.html"）；若该候选不存在
// 而按 udrt 输出目录拼接存在（裸文件名情形），则采用后者。
// 规范化后必须位于 udrt 输出目录内；非法/越权返回空串。
std::string resolveAssetPath(const std::string& input, const std::string& udrtDir,
                             const std::string& exeDir) {
    if (input.empty() || udrtDir.empty()) return {};
    namespace fs = std::filesystem;
    try {
        std::string p = input;
        for (auto& ch : p) if (ch == '/') ch = '\\';
        if (fs::path(p).is_absolute()) {
            std::error_code ec;
            fs::path canon = fs::weakly_canonical(fs::path(p), ec);
            if (ec) canon = fs::path(p);
            std::string a = canon.string();
            for (auto& ch : a) if (ch == '/') ch = '\\';
            fs::path base = fs::weakly_canonical(fs::path(udrtDir), ec);
            std::string b = base.string();
            for (auto& ch : b) if (ch == '/') ch = '\\';
            if (!b.empty() && b.back() != '\\') b += '\\';
            if (a.rfind(b, 0) != 0) return {};   // 越权：不在 udrt 输出目录内
            return a;
        }
        // 相对路径：两个候选基准（exe_dir 协议基准优先，udrt_dir 兜底裸文件名）
        fs::path byExe = fs::path(exeDir.empty() ? "." : exeDir) / p;
        fs::path byUdrt = fs::path(udrtDir) / p;
        std::error_code ec;
        fs::path pick = fs::exists(byExe, ec) ? byExe
                      : (fs::exists(byUdrt, ec) ? byUdrt : byExe);
        fs::path canon = fs::weakly_canonical(pick, ec);
        if (ec) canon = pick;
        std::string a = canon.string();
        for (auto& ch : a) if (ch == '/') ch = '\\';
        fs::path base = fs::weakly_canonical(fs::path(udrtDir), ec);
        std::string b = base.string();
        for (auto& ch : b) if (ch == '/') ch = '\\';
        if (!b.empty() && b.back() != '\\') b += '\\';
        if (a.rfind(b, 0) != 0) return {};       // 越权：不在 udrt 输出目录内
        return a;
    } catch (const std::exception&) {
        return {};
    }
}

std::string statusText(const json& entry) {
    return entry.value("name", "") + " (" + entry.value("module_id", "") + ")";
}

// SHA-256 hex digest（用于 inputHash 缓存判重）
std::string sha256Hex(const std::string& data) {
    NTSTATUS st;
    BCRYPT_ALG_HANDLE alg = nullptr;
    st = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (st != 0) return {};
    UCHAR hash[32] = {};
    st = BCryptHashData(alg, (PUCHAR)data.data(), (ULONG)data.size(), 0);
    if (st == 0)
        st = BCryptFinishHash(alg, hash, sizeof(hash), 0);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (st != 0) return {};
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < 32; ++i) {
        out += hex[hash[i] >> 4];
        out += hex[hash[i] & 0x0F];
    }
    return out;
}

// 编排者 system 提示：工具使用策略 + 知识库清单（工具定义由 SDK 注入）
std::string buildOrchestratorInstructions(const KnowledgeBase& kb) {
    std::ostringstream sys;
    sys << "你是 DeepAgent，运行在 Graph_Saturn 图形工作区（U-DeepRT/DeepRT 引擎）的 AI 助手。\n"
        << "你可以调用工具完成用户的图工程与资产生成需求。使用工具的策略：\n"
        << "1. 用户询问系统能力或闲聊：直接回答（可介绍知识库中的工具 Node），不要调用工具。\n"
        << "2. 用户需要生成图工程/资产：先调 list_knowledge_nodes，然后【紧接着】调 create_udrt\n"
        << "   （字幕类条目必须带 subtitle_text 与 style_prompt；用户明确说不要背景板时 "
        << "background=false）。两步在同一任务中连续完成，中间不要停。\n"
        << "3. 用户要求修改已生成资产：调 generate_node_asset（kind=prompt）。\n"
        << "4. 用户询问某节点状态：调 get_node_context。\n"
        << "5. 用户要求质检/优化/优化某资产（如点击\"优化\"按钮）：按以下流程执行——\n"
        << "   a) 先调 review_asset 读取当前 HTML 内容；\n"
        << "   b) 从四个维度评估：HTML 结构完整性（DOCTYPE/标签闭合）、字幕文本保真（内容\n"
        << "      与用户提供的字幕一致）、排版与样式（居中/字体/行距/配色合理性）、\n"
        << "      是否符合用户提出的样式要求；\n"
        << "   c) 存在问题时：调 generate_node_asset（kind=prompt，text=具体明确的修改指令，\n"
        << "      output_file=review_asset 返回的 path）覆盖优化，然后汇报修改点；\n"
        << "   d) 质量已良好时：不调用优化，简要说明评估结论即可。\n"
        << "重要：工具调用过程中禁止输出过渡性文字（例如\"我将要调用…\"\"现在我来创建…\"），\n"
        << "每一轮要么发起工具调用，要么在全部工具执行完毕后输出最终中文汇报；\n"
        << "生成类工具调用完成后，在最终回答中报告产出文件路径；\n"
        << "最终汇报直接以结论开头，不要以叙述过程开头。始终用中文回答。\n"
        << "外部知识库当前条目：\n" << kb.toPromptText() << "\n";
    return sys.str();
}

} // namespace

// ------------------------------------------------------------------
// Agent Loop 工具（持 AgentGraph 指针，转调公共执行器）
// ------------------------------------------------------------------
namespace {

class ToolBase : public neograph::Tool {
public:
    explicit ToolBase(AgentGraph* owner) : owner_(owner) {}

protected:
    AgentGraph* owner_;
};

class ListKnowledgeNodesTool final : public ToolBase {
public:
    using ToolBase::ToolBase;

    neograph::ChatTool get_definition() const override {
        return {"list_knowledge_nodes",
            "列出外部知识库中已登记的工具 Node 条目（id/名称/能力/输入输出）。"
            "在决定创建图工程前应先调用本工具了解可用能力。",
            neograph::json{{"type", "object"},
                           {"properties", neograph::json::object()},
                           {"required", neograph::json::array()}}};
    }

    std::string execute(const neograph::json& args) override {
        (void)args;
        return owner_->toolListKnowledgeNodes();
    }

    std::string get_name() const override { return "list_knowledge_nodes"; }
};

class CreateUdrtTool final : public ToolBase {
public:
    using ToolBase::ToolBase;

    neograph::ChatTool get_definition() const override {
        return {"create_udrt",
            "根据知识库条目生成 .udrt 图工程文件并同时生成 HTML 资产。"
            "参数：entry_id=知识库条目id；subtitle_text=字幕源文本（字幕类条目必填）；"
            "style_prompt=样式要求（字体/字号/颜色等，可空）；"
            "background=是否生成背景板并与字幕内容合成最终效果（可空，缺省按文本内容自动判断）。",
            neograph::json{
                {"type", "object"},
                {"properties", {
                    {"entry_id",      {{"type", "string"}, {"description", "知识库条目 id"}}},
                    {"subtitle_text", {{"type", "string"}, {"description", "字幕源文本（markdown）"}}},
                    {"style_prompt",  {{"type", "string"}, {"description", "样式要求（字体/字号/颜色等）"}}},
                    {"background",    {{"type", "boolean"}, {"description", "是否启用背景板分支（可空=自动判断）"}}}
                }},
                {"required", neograph::json::array({"entry_id"})}}};
    }

    std::string execute(const neograph::json& args) override {
        std::optional<bool> background;
        if (args.contains("background") && args["background"].is_boolean())
            background = args["background"].get<bool>();
        return owner_->toolCreateUdrt(
            args.value("entry_id", ""),
            args.value("subtitle_text", ""),
            args.value("style_prompt", ""),
            background,
            nullptr, nullptr, nullptr);
    }

    std::string get_name() const override { return "create_udrt"; }
};

class GenerateNodeAssetTool final : public ToolBase {
public:
    using ToolBase::ToolBase;

    neograph::ChatTool get_definition() const override {
        return {"generate_node_asset",
            "为指定 GUID 的节点生成/更新资产。"
            "kind=content 新建资产；kind=prompt 按修改指令调整（字体/字号/颜色等）。"
            "background=是否生成背景板并合成（可空，缺省按文本内容自动判断）。"
            "output_file=质检优化场景下覆盖写回的原 HTML 文件路径（可空；提供时优化结果落盘并推送新版本资产卡片）。",
            neograph::json{
                {"type", "object"},
                {"properties", {
                    {"guid", {{"type", "string"}, {"description", "节点 GUID"}}},
                    {"kind", {{"type", "string"}, {"description", "content=新建; prompt=按指令修改"}}},
                    {"text", {{"type", "string"}, {"description", "字幕源文本或修改指令"}}},
                    {"background", {{"type", "boolean"}, {"description", "是否启用背景板分支（可空=自动判断）"}}},
                    {"output_file", {{"type", "string"}, {"description", "优化场景：覆盖写回的原 HTML 文件路径（可空）"}}}
                }},
                {"required", neograph::json::array({"guid", "kind", "text"})}}};
    }

    std::string execute(const neograph::json& args) override {
        std::optional<bool> background;
        if (args.contains("background") && args["background"].is_boolean())
            background = args["background"].get<bool>();
        return owner_->toolGenerateNodeAsset(
            args.value("guid", ""),
            args.value("kind", "content"),
            args.value("text", ""),
            background,
            args.value("output_file", ""));
    }

    std::string get_name() const override { return "generate_node_asset"; }
};

// 质检优化：LLM 读回已生成 HTML 资产内容做质量评估（配合 generate_node_asset 回写）
class ReviewAssetTool final : public ToolBase {
public:
    using ToolBase::ToolBase;

    neograph::ChatTool get_definition() const override {
        return {"review_asset",
            "读取已生成的 HTML 资产文件内容（仅限 udrt 输出目录内），用于质检评估："
            "排版完整性、字幕文本保真、样式是否符合要求等。",
            neograph::json{
                {"type", "object"},
                {"properties", {
                    {"path", {{"type", "string"}, {"description", "HTML 文件路径（相对或绝对）"}}}
                }},
                {"required", neograph::json::array({"path"})}}};
    }

    std::string execute(const neograph::json& args) override {
        return owner_->toolReviewAsset(args.value("path", ""));
    }

    std::string get_name() const override { return "review_asset"; }
};

class GetNodeContextTool final : public ToolBase {
public:
    using ToolBase::ToolBase;

    neograph::ChatTool get_definition() const override {
        return {"get_node_context",
            "查看指定 GUID 节点的多轮上下文与执行状态。",
            neograph::json{
                {"type", "object"},
                {"properties", {
                    {"guid", {{"type", "string"}, {"description", "节点 GUID"}}}
                }},
                {"required", neograph::json::array({"guid"})}}};
    }

    std::string execute(const neograph::json& args) override {
        return owner_->toolGetNodeContext(args.value("guid", ""));
    }

    std::string get_name() const override { return "get_node_context"; }
};

} // namespace

// ------------------------------------------------------------------
// 构造
// ------------------------------------------------------------------

AgentGraph::AgentGraph(const Config& config, LlmClient& llm, const KnowledgeBase& kb,
                       const NodeCatalog& catalog, const UdrtCompiler& compiler,
                       NodeContextStore& nodeContexts)
    : config_(config), llm_(llm), kb_(kb), catalog_(catalog), compiler_(compiler),
      nodeContexts_(nodeContexts) {}

// ------------------------------------------------------------------
// 工具执行器实现（Agent Loop 的 tool_calls 落到这里）
// ------------------------------------------------------------------

std::string AgentGraph::toolListKnowledgeNodes() const {
    return kb_.entries().dump();
}

std::string AgentGraph::toolCreateUdrt(const std::string& entryId,
                                       const std::string& subtitleText,
                                       const std::string& stylePrompt,
                                       std::optional<bool> background,
                                       std::string* guidOut,
                                       std::string* udrtFileOut,
                                       std::string* htmlFileOut) {
    const json* entry = kb_.find(entryId);
    if (!entry) {
        json knownIds = json::array();
        for (const auto& e : kb_.entries()) knownIds.push_back(e.value("id", ""));
        return json{{"error", "unknown entry_id: " + entryId},
                    {"expected_one_of", knownIds},
                    {"repairable", true},
                    {"hint", "请从 expected_one_of 中选择一个条目重试，"
                             "或先调 list_knowledge_nodes 查看完整清单"}}.dump();
    }

    // P3: inputHash 进程内缓存——相同参数直接返回上次结果，避免重复 LLM 调用
    {
        std::string inputHash = sha256Hex(entryId + "|" + subtitleText + "|" +
                                          stylePrompt + "|" + config_.llm_model);
        if (!m_lastCreateUdrtHash.empty() && inputHash == m_lastCreateUdrtHash) {
            json cached = json::parse(m_lastCreateUdrtResult, nullptr, false);
            if (cached.is_object()) {
                cached["cached"] = true;
                cached["input_hash"] = inputHash;
                if (guidOut) *guidOut = cached.value("guid", "");
                if (udrtFileOut) *udrtFileOut = cached.value("udrt_file", "");
                if (htmlFileOut) *htmlFileOut = cached.value("html_file", "");
                return cached.dump();
            }
        }
    }

    const json& tpl = entry->at("udrt_template");

    // Plan：模板为准（确定性），保证连线/端口合法
    json plan;
    plan["nodes"] = tpl.value("nodes", json::array());
    plan["links"] = tpl.value("links", json::array());

    // props 覆盖：字幕文本/样式/API 配置（GUID 由编译器生成后回填）
    for (auto& pn : plan["nodes"]) {
        const json* cat = catalog_.node(pn.value("catalog", ""));
        if (!cat) continue;
        json props = cat->value("props", json::object());
        auto merge = [&](const char* key, const std::string& v) {
            if (v.empty() || !props.contains(key)) return;
            props[key] = v;
        };
        merge("strContent", subtitleText);
        merge("strPrompt", stylePrompt);
        merge("strApiKey", config_.llm_api_key);
        merge("strModel", config_.llm_model);
        pn["props"] = props;
    }

    json udrt;
    std::string cerr;
    json detailed;
    if (m_activeSink && m_activeSink->onStatus)
        m_activeSink->onStatus("正在编译并校验 udrt 工程…");
    if (!compiler_.compile(plan, &udrt, &cerr, &detailed)) {
        // P2: 结构化编译诊断回填给 LLM（path/expected/actual/hint）实现自修复
        detailed["repairable"] = true;
        return detailed.dump();
    }
    if (!compiler_.validate(udrt, &cerr, &detailed)) {
        detailed["repairable"] = true;
        return detailed.dump();
    }

    std::string guid;
    if (!udrt["nodes"].empty())
        guid = udrt["nodes"][0]["internal-data"].value("guid", std::string());
    if (guidOut) *guidOut = guid;

    std::string filePrefix = tpl.value("file_prefix", std::string("deepagent"));
    std::string udrtPath =
        UdrtCompiler::makeFilePath(config_.udrt_output_dir, filePrefix + "_" + guid.substr(0, 8));
    if (!compiler_.writeFile(udrt, udrtPath, &cerr)) {
        return json{{"error", cerr}}.dump();
    }

    // HTML 资产：子步管线化生成（可选背景板分支 ∥ LLM 内容分支 → 合成）。
    // 见 docs/Node资产生成子步并发设计.md
    std::string htmlFile;
    if (!subtitleText.empty()) {
        std::vector<std::string> toolGuids;
        std::size_t ui = 0;
        for (const auto& pn : plan["nodes"]) {
            const json& un = ui < udrt["nodes"].size() ? udrt["nodes"][ui] : json();
            ++ui;
            if (pn.value("catalog", "") != entryId) continue;
            std::string g = un.value("internal-data", json::object()).value("guid", std::string());
            if (!g.empty()) toolGuids.push_back(g);
        }
        if (toolGuids.empty() && !guid.empty()) toolGuids.push_back(guid);

        AssetRunnerConfig rc;
        rc.api_key = config_.llm_api_key;
        rc.base_url = config_.llm_base_url;
        rc.model = config_.llm_model;
        rc.system_prompt = kSubtitleSystemPrompt;
        auto assetProvider = std::make_shared<WinHttpProvider>(llm_, config_);
        AssetGraphRunner runner(rc, assetProvider);

        const bool wants_bg = wantsBackground(background, subtitleText, stylePrompt);
        std::vector<AssetRequest> reqs;
        for (const auto& g : toolGuids) {
            AssetRequest req;
            req.guid = g;
            req.short_id = g.size() >= 8 ? g.substr(0, 8) : g;
            req.user_text = subtitleText;
            req.context = json{{"content", subtitleText}, {"style", stylePrompt}};
            req.wants_background = wants_bg;
            reqs.push_back(std::move(req));
        }

        json pipelineSchema = entry->value("asset_pipeline", json::object());
        if (!pipelineSchema.contains("branches")) pipelineSchema = defaultSubtitlePipeline();

        if (m_activeSink && m_activeSink->onStatus)
            m_activeSink->onStatus("正在生成 HTML 资产（LLM 子步管线，约需十几秒）…");

        auto results = runner.run(
            reqs, pipelineSchema,
            [this](const std::string& g, const std::string& step, const std::string& state) {
                if (m_activeSink && m_activeSink->onNodeState)
                    m_activeSink->onNodeState(
                        json{{"guid", g}, {"step", step}, {"state", state}});
            });

        for (const auto& r : results) {
            if (r.ok && !r.final_html.empty()) {
                htmlFile = udrtPath.substr(0, udrtPath.rfind('.')) + ".html";
                std::ofstream ofs(htmlFile, std::ios::binary | std::ios::trunc);
                if (ofs) ofs.write(r.final_html.data(), (std::streamsize)r.final_html.size());
                break;
            }
        }
    }

    json ok{{"udrt_file", udrtPath},
            {"guid", guid},
            {"nodes", udrt["nodes"].size()},
            {"connections", udrt["connections"].size()}};
    if (!htmlFile.empty()) ok["html_file"] = htmlFile;
    // P4: phases 分组透传（Plan IR 预留字段；不写入 udrt 文件）
    if (plan.contains("phases")) ok["phases"] = plan["phases"];

    // Agent Loop 路径的产物卡片：udrt / HTML 资产事件发给前端
    // （legacy 管线在 runLegacyPipeline 内自发，工具执行器只有这里能拿到产物路径）
    if (m_activeSink) {
        if (m_activeSink->onUdrt) {
            json payload;
            payload["type"] = "udrt";
            payload["file"] = makeRelative(udrtPath);
            payload["absolute_path"] = udrtPath;
            payload["name"] = entry->value("name", "");
            payload["module_id"] = entry->value("module_id", "");
            payload["node_count"] = udrt["nodes"].size();
            payload["connection_count"] = udrt["connections"].size();
            if (plan.contains("phases")) payload["phases"] = plan["phases"];
            payload["content"] = udrt;
            m_activeSink->onUdrt(payload);
        }
        if (!htmlFile.empty() && m_activeSink->onAsset) {
            json asset{{"type", "html"},
                       {"file", makeRelative(htmlFile)},
                       {"absolute_path", htmlFile},
                       {"guid", guid},
                       {"version", 1}};
            m_activeSink->onAsset(asset);
        }
    }

    // P3: inputHash 进程内缓存
    m_lastCreateUdrtHash = sha256Hex(
        entryId + "|" + subtitleText + "|" + stylePrompt + "|" + config_.llm_model);
    m_lastCreateUdrtResult = ok.dump();
    return ok.dump();
}

std::string AgentGraph::toolGenerateNodeAsset(const std::string& guid,
                                              const std::string& kind,
                                              const std::string& text,
                                              std::optional<bool> background,
                                              const std::string& outputFile) {
    // P0: 参数缺失时给出模型可读的修复线索（而不是只说"什么失败了"）
    if (guid.empty() || text.empty()) {
        json miss = json::array();
        if (guid.empty()) miss.push_back("guid");
        if (text.empty()) miss.push_back("text");
        return json{{"error", "missing required arguments"},
                    {"missing", miss},
                    {"repairable", true},
                    {"hint", "guid 取自 create_udrt 返回结果；text 为字幕源文本"
                             "（kind=content）或修改指令（kind=prompt）。"
                             "请补全缺失参数后重试"}}.dump();
    }

    json messages;
    std::string err;
    // 质检优化场景：节点尚无上下文时（资产由 create_udrt 直接生成，未登记 node_context），
    // 以"优化指令 + 现有 HTML"播种 content 轮，使 prompt 修改有可依据的基线
    std::string effectiveKind = kind;
    std::string effectiveText = text;
    if (kind == "prompt" && nodeContexts_.getContext(guid).is_null()) {
        std::string existing;
        if (!outputFile.empty()) {
            std::string abs = resolveAssetPath(outputFile, config_.udrt_output_dir,
                                               config_.exe_dir);
            if (!abs.empty()) {
                std::ifstream ifs(abs, std::ios::binary);
                if (ifs) {
                    std::ostringstream ss;
                    ss << ifs.rdbuf();
                    existing = ss.str();
                    if (existing.size() > 64 * 1024) existing.resize(64 * 1024);
                }
            }
        }
        effectiveKind = "content";
        if (!existing.empty())
            effectiveText += "\n\n以下为现有 HTML 资产，请在保持字幕内容与整体结构的基础上"
                             "按上述要求优化，输出完整 HTML：\n" + existing;
    }
    if (!nodeContexts_.beginTurn(guid, effectiveKind, kSubtitleSystemPrompt, effectiveText,
                                 &messages, &err)) {
        // busy（轮次进行中）或参数非法：回填状态与建议，LLM 可据此改调其他工具
        json resp{{"error", err},
                  {"guid", guid},
                  {"repairable", true},
                  {"hint", err.find("busy") != std::string::npos
                               ? "该节点上一轮生成尚未结束，请稍后重试，"
                                 "或改调 get_node_context 查看当前状态"
                               : "kind 仅支持 content（新建资产）与 prompt（按指令修改），"
                                 "请修正后重试"}};
        json ctx = nodeContexts_.getContext(guid);
        if (!ctx.is_null()) resp["state"] = ctx.value("state", "");
        return resp.dump();
    }

    // 子步管线：完整多轮上下文作为 content 分支输入；可选背景板分支并发合成
    AssetRunnerConfig rc;
    rc.api_key = config_.llm_api_key;
    rc.base_url = config_.llm_base_url;
    rc.model = config_.llm_model;
    rc.system_prompt = kSubtitleSystemPrompt;
    auto assetProvider = std::make_shared<WinHttpProvider>(llm_, config_);
        AssetGraphRunner runner(rc, assetProvider);

    AssetRequest req;
    req.guid = guid;
    req.short_id = guid.size() >= 8 ? guid.substr(0, 8) : guid;
    req.messages = messages;
    req.context = json{{"content", text}, {"style", kind == "prompt" ? text : ""}};
    req.wants_background = wantsBackground(background, text, "");

    if (m_activeSink && m_activeSink->onStatus)
        m_activeSink->onStatus("正在生成节点资产（LLM 子步管线，约需十几秒）…");

    auto results = runner.run(
        {std::move(req)}, defaultSubtitlePipeline(),
        [this](const std::string& g, const std::string& step, const std::string& state) {
            if (m_activeSink && m_activeSink->onNodeState)
                m_activeSink->onNodeState(json{{"guid", g}, {"step", step}, {"state", state}});
        });

    const auto& r = results.front();
    if (!r.ok || r.final_html.empty()) {
        const std::string fail = r.error.empty() ? "empty pipeline result" : r.error;
        nodeContexts_.failTurn(guid, fail);
        // P0: 失败细节回填 + 自修复线索（LLM 可改参数重试或向用户解释）
        return json{{"error", fail},
                    {"guid", guid},
                    {"state", "failed"},
                    {"repairable", true},
                    {"hint", "管线执行失败：可修正 text 后重试，"
                             "或改用 create_udrt 重新生成整个图工程"}}.dump();
    }
    nodeContexts_.completeTurn(guid, r.final_html);

    // 质检优化回写：output_file 提供时把优化结果覆盖写回原 HTML，并推送新版本资产卡片
    json ok{{"state", "done"}, {"html", r.final_html}, {"guid", guid}};
    if (!outputFile.empty()) {
        std::string abs = resolveAssetPath(outputFile, config_.udrt_output_dir, config_.exe_dir);
        if (abs.empty()) {
            return json{{"error", "output_file is outside the udrt output directory: " + outputFile},
                        {"expected", "path under " + config_.udrt_output_dir},
                        {"repairable", true},
                        {"hint", "output_file 必须是 create_udrt 返回的 html_file 路径；"
                                 "请改用 review_asset 结果中的 path 重试"}}.dump();
        }
        std::ofstream ofs(abs, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            return json{{"error", "cannot open file for write: " + abs},
                        {"repairable", true}}.dump();
        }
        ofs.write(r.final_html.data(), (std::streamsize)r.final_html.size());
        ok["html_file"] = abs;
        ok["file"] = makeRelative(abs);
        if (m_activeSink && m_activeSink->onAsset) {
            m_activeSink->onAsset(json{{"type", "html"},
                                       {"file", makeRelative(abs)},
                                       {"absolute_path", abs},
                                       {"guid", guid},
                                       {"version", 2}});
        }
    }
    return ok.dump();
}

std::string AgentGraph::toolReviewAsset(const std::string& path) const {
    if (path.empty()) {
        return json{{"error", "path is required"},
                    {"missing", json::array({"path"})},
                    {"repairable", true},
                    {"hint", "path 取自 create_udrt 返回的 html_file 或资产卡片事件中的 file"}}.dump();
    }
    std::string abs = resolveAssetPath(path, config_.udrt_output_dir, config_.exe_dir);
    if (abs.empty()) {
        return json{{"error", "path is outside the udrt output directory: " + path},
                    {"expected", "path under " + config_.udrt_output_dir},
                    {"repairable", true},
                    {"hint", "只能读取 udrt 输出目录内的资产；请使用 create_udrt 返回的 html_file 路径"}}.dump();
    }
    std::ifstream ifs(abs, std::ios::binary);
    if (!ifs) {
        return json{{"error", "asset file not found: " + abs},
                    {"repairable", true},
                    {"hint", "文件不存在：请确认路径来自本轮会话的 create_udrt 结果"}}.dump();
    }
    std::ostringstream ss;
    ss << ifs.rdbuf();
    std::string html = ss.str();
    if (html.size() > 64 * 1024) html.resize(64 * 1024);  // 防超长资产撑爆上下文
    return json{{"path", abs},
                {"bytes", html.size()},
                {"html", html}}.dump();
}

std::string AgentGraph::toolGetNodeContext(const std::string& guid) const {
    json ctx = nodeContexts_.getContext(guid);
    if (ctx.is_null()) {
        // P0: 回填已知 GUID 清单，LLM 可从中挑出正确值自修复
        json known = json::array();
        for (const auto& item : nodeContexts_.listContexts())
            if (item.is_object()) known.push_back(item.value("guid", ""));
        return json{{"error", "context not found: " + guid},
                    {"known_guids", known},
                    {"repairable", true},
                    {"hint", "guid 不存在。请从 known_guids 中选择，"
                             "或先调 create_udrt 生成图工程并使用其返回的 guid"}}.dump();
    }
    return ctx.dump();
}

// ------------------------------------------------------------------
// run()：Agent Loop 主路径（LLM 自主工具调用），异常回退固定管线
// ------------------------------------------------------------------

std::string AgentGraph::run(const std::string& userMessage,
                            const std::vector<ChatMessage>& history,
                            const EventSink& sink, std::string* error) {
    m_activeSink = &sink;
    if (sink.onStatus) sink.onStatus("正在分析需求（Agent Loop 工具编排）…");
    std::string reply;
    try {
        // 1. LLM Provider：WinHTTP 传输。NeoGraph OpenAIProvider 的 ConnPool 异步链路
        //    在本服务进程内挂起不前（同步 asio/WinHTTP 均正常，根因见 CHANGELOG V1.4.0），
        //    故 Agent Loop 改用与 legacy 管线相同的 WinHTTP 通道。
        auto provider = std::make_unique<WinHttpProvider>(llm_, config_);

        // 2. 工具集（Agent Loop 中 LLM 自主调用）
        std::vector<std::unique_ptr<neograph::Tool>> tools;
        tools.push_back(std::make_unique<ListKnowledgeNodesTool>(this));
        tools.push_back(std::make_unique<CreateUdrtTool>(this));
        tools.push_back(std::make_unique<GenerateNodeAssetTool>(this));
        tools.push_back(std::make_unique<ReviewAssetTool>(this));
        tools.push_back(std::make_unique<GetNodeContextTool>(this));

        // 3. Agent Loop（LLM 自主决定调用工具的次数与顺序，max_iterations 止损）
        neograph::llm::Agent agent(std::move(provider), std::move(tools),
                                   buildOrchestratorInstructions(kb_),
                                   config_.llm_model);
        agent.set_tool_detection_timeout_seconds(120);

        // 工具调用观察点：LLM 每发起一次工具调用先向前端推状态事件，
        // 消除多轮工具调用 + 资产生成期间的"准备中"空窗（20s+ 无反馈）
        agent.set_tool_gate(
            [this](neograph::ToolCall call, neograph::ToolGateContext)
                -> asio::awaitable<neograph::ToolDecision> {
                if (m_activeSink && m_activeSink->onStatus) {
                    std::string brief = call.name;
                    if (call.name == "create_udrt") {
                        json args = json::parse(call.arguments, nullptr, false);
                        if (args.is_object()) {
                            const std::string entry = args.value("entry_id", std::string());
                            if (!entry.empty()) brief += "（" + entry + "）";
                        }
                    }
                    m_activeSink->onStatus("正在调用工具 " + brief + " …");
                }
                co_return neograph::ToolDecision::allow();
            });

        std::vector<neograph::ChatMessage> messages;
        for (const auto& h : history)
            messages.push_back({h.role, h.content});
        messages.push_back({"user", userMessage});

        reply = agent.run_stream(messages,
            [&](const std::string& token) {
                if (sink.onToken) sink.onToken(token);
            }, /*max_iterations=*/8);

        if (reply.empty()) {
            if (error) *error = "empty agent reply";
            Logf("[agent_loop] ok-but-empty reply");
        } else {
            Logf("[agent_loop] ok, reply %zu bytes", reply.size());
        }
    } catch (const std::exception& e) {
        // Agent Loop 异常：记录原因（日志），回退旧固定管线
        Logf("[agent_loop] failed, fallback to legacy: %s", e.what());
        if (error) *error = std::string("agent loop failed, fallback: ") + e.what();
        reply.clear();
    }
    m_activeSink = nullptr;

    if (reply.empty())
        reply = runLegacyPipeline(userMessage, history, sink, error);
    return reply;
}

// ------------------------------------------------------------------
// 旧固定管线（fallback：LLM tools 协议异常时回退）
// ------------------------------------------------------------------

json AgentGraph::analyzeIntent(const std::string& userMessage, std::string* llmError) {
    std::ostringstream sys;
    sys << "你是 DeepAgent，一个分析用户需求并为图形工作区选择工具节点的助手。\n"
        << "工作区为 Graph_Saturn（U-DeepRT/DeepRT 图形引擎）。以下是外部知识库中已登记的工具 Node 条目：\n"
        << kb_.toPromptText() << "\n\n"
        << "任务：判断用户的这条消息是否需要调用上述某个工具 Node（是否需要生成 .udrt 图工程文件）。\n"
        << "只输出一个 JSON 对象，不要输出其他文字，格式：\n"
        << "{\"is_tool_task\": true|false, \"entry_id\": \"...\", "
        << "\"subtitle_text\": \"...\", "
        << "\"style_prompt\": \"...\", "
        << "\"reason\": \"一句话理由\"}\n";
    std::vector<ChatMessage> messages;
    messages.push_back({"system", sys.str()});
    messages.push_back({"user", userMessage});
    std::string err;
    std::string raw = llm_.invoke(messages, &err, /*temperature=*/0.2);
    if (raw.empty()) {
        if (llmError) *llmError = err;
        return nullptr;
    }
    json parsed;
    if (!extractJsonObject(raw, &parsed) || !parsed.contains("is_tool_task")) {
        if (llmError) *llmError = "LLM returned non-JSON intent: " + raw.substr(0, 200);
        return nullptr;
    }
    return parsed;
}

json AgentGraph::analyzeIntentByKeywords(const std::string& userMessage) const {
    json scored = kb_.scoreByKeywords(userMessage);
    json result;
    if (!scored.empty() && scored[0]["score"].get<int>() > 0) {
        result["is_tool_task"] = true;
        result["entry_id"] = scored[0]["id"].get<std::string>();
        result["subtitle_text"] = "";
        result["style_prompt"] = "";
        result["reason"] = "关键词匹配回退（LLM 不可用）";
    } else {
        result["is_tool_task"] = false;
        result["entry_id"] = nullptr;
        result["subtitle_text"] = "";
        result["style_prompt"] = "";
        result["reason"] = "无关键词命中";
    }
    return result;
}

json AgentGraph::templatePlan(const json& entry) const {
    const json& tpl = entry.at("udrt_template");
    json plan;
    plan["nodes"] = tpl.value("nodes", json::array());
    plan["links"] = tpl.value("links", json::array());
    // P4: phases 分组预留（模板可声明，编译器不写入 udrt，仅透传给 UI）
    if (tpl.contains("phases")) plan["phases"] = tpl["phases"];
    return plan;
}

bool AgentGraph::planGraph(const json& entry, const json& intent,
                           const std::string& userMessage, json* planIr,
                           bool* usedTemplate) {
    *usedTemplate = false;

    const json& tpl = entry.at("udrt_template");
    json catalogBrief = json::array();
    for (const auto& pn : tpl["nodes"]) {
        if (const json* cat = catalog_.node(pn.value("catalog", ""))) {
            json brief;
            brief["catalog"] = pn.value("catalog", "");
            brief["output_ports"] = cat->value("output_ports", json::object());
            brief["input_ports"] = cat->value("input_ports", json::object());
            catalogBrief.push_back(brief);
        }
    }

    // P1: 沙箱上下文（纯数据；脚本只能引用这里的目录与端口）
    json scriptContext;
    scriptContext["user_requirement"] = userMessage;
    scriptContext["catalogs"] = catalogBrief;
    scriptContext["template"] = tpl;
    if (intent.contains("style_prompt"))
        scriptContext["style_prompt"] = intent.value("style_prompt", std::string());

    std::ostringstream sys;
    sys << "你是图形计划助手。请输出一段 JavaScript 脚本（不是 JSON）来描述 Graph "
        << "Plan IR（语义层，不是 udrt）。脚本必须形如：\n"
        << "define(\"plan\", function(context) {\n"
        << "    var nodes = []; var links = [];\n"
        << "    // 可用 if/for 表达条件逻辑；只能引用 context.catalogs 中的 catalog 与端口\n"
        << "    nodes.push({key: \"tool\", catalog: \"...\"});\n"
        << "    links.push({from: \"tool:输出端口\", to: \"...:输入端口\"});\n"
        << "    return {nodes: nodes, links: links};\n"
        << "});\n"
        << "context 字段：user_requirement(用户需求)、catalogs(可用目录与语义端口)、"
        << "template(参考连线模板)、style_prompt(样式要求，可空)。\n"
        << "要求：与模板等价即可；不得引入目录之外的节点或端口；"
        << "禁止 require/import/网络/文件访问。\n"
        << "只输出脚本代码，不要解释，不要 markdown 围栏。\n"
        << "参考模板：\n" << tpl.dump() << "\n"
        << "目录：\n" << catalogBrief.dump() << "\n";

    auto askScript = [this](const std::vector<ChatMessage>& msgs, std::string* out) {
        std::string e;
        std::string raw = llm_.invoke(msgs, &e, /*temperature=*/0.2);
        if (raw.empty()) return false;
        *out = StripHtmlFence(raw);  // 剥可能的 ``` 围栏（与 HTML 资产同一助手）
        return true;
    };

    std::vector<ChatMessage> messages;
    messages.push_back({"system", sys.str()});
    messages.push_back({"user", userMessage});

    std::string script;
    if (askScript(messages, &script)) {
        PlanScriptResult r = executePlanScript(script, scriptContext);
        if (r.ok && planIrShapeOk(r.planIr, catalog_)) {
            *planIr = std::move(r.planIr);
            return true;
        }
        // P0/P1 自修复闭环：把沙箱/形态错误回填给 LLM，允许一轮重写
        json feedback{{"error", r.ok ? "plan 形态或 catalog 引用非法" : r.error},
                      {"expected",
                       "define(\"plan\", function(context){ ... return "
                       "{\"nodes\":[{key,catalog}], \"links\":[{from,to}]}; })"},
                      {"known_catalogs", catalog_.catalogKeys()},
                      {"hint", "请修复脚本后重新输出完整脚本（仍是 define(\"plan\", ...) 形式）"}};
        messages.push_back({"assistant", script});
        messages.push_back({"user", "脚本执行失败：" + feedback.dump()});
        std::string script2;
        if (askScript(messages, &script2)) {
            PlanScriptResult r2 = executePlanScript(script2, scriptContext);
            if (r2.ok && planIrShapeOk(r2.planIr, catalog_)) {
                *planIr = std::move(r2.planIr);
                return true;
            }
        }
    }

    // 脚本不可用：回退知识库内置模板（确定性兜底）
    *planIr = templatePlan(entry);
    *usedTemplate = true;
    return true;
}

// 旧固定管线主体（fallback：LLM tools 协议异常时回退）
std::string AgentGraph::makeRelative(const std::string& absolute) const {
    std::string a = absolute;
    std::string b = config_.exe_dir;
    for (auto& ch : a) if (ch == '/') ch = '\\';
    for (auto& ch : b) if (ch == '/') ch = '\\';
    if (!b.empty() && b.back() != '\\') b += '\\';
    if (a.rfind(b, 0) == 0) return a.substr(b.size());
    return absolute;
}

std::string AgentGraph::runLegacyPipeline(const std::string& userMessage,
                                          const std::vector<ChatMessage>& history,
                                          const EventSink& sink, std::string* error) {
    auto status = [&](const std::string& s) {
        if (sink.onStatus) sink.onStatus(s);
    };

    // ---- Step 1: analyze_intent (LLM + knowledge base) ----
    status("正在分析需求，检索知识库…");
    std::string llmErr;
    json intent = analyzeIntent(userMessage, &llmErr);
    if (intent.is_null()) {
        status("LLM 意图分析不可用，使用知识库关键词匹配回退");
        intent = analyzeIntentByKeywords(userMessage);
    }

    // ---- Step 2: validate_selection ----
    const json* entry = nullptr;
    if (intent.value("is_tool_task", false)) {
        std::string entryId = intent.value("entry_id", std::string());
        entry = kb_.find(entryId);
        if (!entry) status("知识库中不存在条目 " + entryId + "，忽略工具调用");
    }

    if (!entry) {
        // ---- 普通对话路径 ----
        status("未检测到需要调用的工具 Node，进入普通对话");
        std::ostringstream sys;
        sys << "你是 DeepAgent，运行在 Graph_Saturn 图形工作区（U-DeepRT/DeepRT 引擎）的 AI 助手。"
            << "你可以根据需求生成工具 Node 的 .udrt 图工程文件。当前消息未触发工具调用，请自然地回答用户。"
            << "如用户询问你能做什么，可介绍：知识库中已登记的工具 Node 能力（如根据字幕文本调用 LLM 生成字幕 HTML 文件的工具 Node）。";
        std::vector<ChatMessage> messages;
        messages.push_back({"system", sys.str()});
        for (const auto& m : history) messages.push_back(m);
        messages.push_back({"user", userMessage});
        std::string err;
        std::string reply = llm_.invokeStream(messages, sink.onToken, &err);
        if (reply.empty()) {
            if (error) *error = err.empty() ? "LLM reply empty" : err;
            return {};
        }
        return reply;
    }

    status("已选择工具 Node：" + statusText(*entry));

    // ---- Step 3: plan_graph ----
    status("正在生成 Graph Plan（LLM 提案 + 目录校验）…");
    json planIr;
    bool usedTemplate = false;
    planGraph(*entry, intent, userMessage, &planIr, &usedTemplate);
    if (usedTemplate) status("LLM Plan 无效，回退知识库内置模板");

    // ---- Step 4: compile_udrt (props override: subtitle text / style) ----
    json plan = planIr;
    std::string subtitleText = intent.value("subtitle_text", std::string());
    std::string stylePrompt = intent.value("style_prompt", std::string());
    for (auto& pn : plan["nodes"]) {
        const json* cat = catalog_.node(pn.value("catalog", ""));
        if (!cat) continue;
        auto props = cat->find("props");
        if (props == cat->end() || !props->is_object()) continue;
        json overrides = json::object();
        if (!subtitleText.empty() && props->contains("strContent"))
            overrides["strContent"] = subtitleText;
        if (!stylePrompt.empty() && props->contains("strPrompt"))
            overrides["strPrompt"] = stylePrompt;
        if (config_.llmConfigured() && props->contains("strApiKey"))
            overrides["strApiKey"] = config_.llm_api_key;
        if (!config_.llm_model.empty() && props->contains("strModel"))
            overrides["strModel"] = config_.llm_model;
        if (!overrides.empty()) {
            if (pn.contains("props") && pn["props"].is_object()) {
                pn["props"].merge_patch(overrides);
            } else {
                pn["props"] = overrides;
            }
        }
    }

    status("正在编译并校验 udrt…");
    json udrt;
    std::string cerr;
    if (!compiler_.compile(plan, &udrt, &cerr)) {
        if (error) *error = "udrt 编译失败: " + cerr;
        return {};
    }
    if (!compiler_.validate(udrt, &cerr)) {
        if (error) *error = "udrt 校验失败: " + cerr;
        return {};
    }

    // ---- Step 5: write_file ----
    status("正在写入 udrt 文件…");
    std::string prefix = entry->at("udrt_template").value("file_prefix", std::string("deepagent"));
    std::string path = UdrtCompiler::makeFilePath(config_.udrt_output_dir, prefix);
    if (!compiler_.writeFile(udrt, path, &cerr)) {
        if (error) *error = cerr;
        return {};
    }
    status("已生成 udrt 文件：" + path);

    if (sink.onUdrt) {
        json payload;
        payload["type"] = "udrt";
        payload["file"] = makeRelative(path);
        payload["absolute_path"] = path;
        payload["name"] = entry->value("name", "");
        payload["module_id"] = entry->value("module_id", "");
        payload["node_count"] = udrt["nodes"].size();
        payload["connection_count"] = udrt["connections"].size();
        // P4: phases 分组透传（Plan IR 预留字段；不写入 udrt 文件）
        if (planIr.contains("phases")) payload["phases"] = planIr["phases"];
        payload["content"] = udrt;
        sink.onUdrt(payload);
    }

    // HTML 资产：字幕文本 + 样式指令一次性生成（与 udrt 同目录同名关联）
    std::string htmlFile;
    std::string htmlSubtitle = intent.value("subtitle_text", std::string());
    std::string htmlStyle = intent.value("style_prompt", std::string());
    if (!htmlSubtitle.empty()) {
        std::vector<ChatMessage> gen;
        gen.push_back({"system", kSubtitleSystemPrompt});
        std::string userText = htmlSubtitle;
        if (!htmlStyle.empty()) userText += "\n\n样式要求：" + htmlStyle;
        gen.push_back({"user", userText});
        std::string htmlErr;
        std::string html = StripHtmlFence(llm_.invoke(gen, &htmlErr, /*temperature=*/0.3));
        if (!html.empty()) {
            htmlFile = path.substr(0, path.rfind('.')) + ".html";
            std::ofstream ofs(htmlFile, std::ios::binary | std::ios::trunc);
            if (ofs) ofs.write(html.data(), (std::streamsize)html.size());
        }
    }

    status(htmlFile.empty() ? "已生成 udrt 文件：" + path
                            : "已生成 udrt 与 HTML 资产：" + htmlFile);
    if (sink.onAsset) {
        json asset{{"type", "html"},
                   {"file", makeRelative(htmlFile.empty() ? path : htmlFile)},
                   {"absolute_path", htmlFile.empty() ? path : htmlFile},
                   {"guid", std::string()},
                   {"version", 1}};
        sink.onAsset(asset);
    }

    // ---- Step 6: reply (streaming) ----
    std::ostringstream summary;
    summary << "已完成处理。产出：\n"
            << "- udrt 文件: " << path << "\n"
            << "- 节点数: " << udrt["nodes"].size() << ", 连接数: " << udrt["connections"].size() << "\n";
    if (!htmlFile.empty()) summary << "- HTML 资产: " << htmlFile << "\n";

    std::ostringstream sys;
    sys << "你是 DeepAgent，运行在 Graph_Saturn 图形工作区（U-DeepRT/DeepRT 引擎）的 AI 助手。"
        << "当前流程已完成固定管线处理，请自然地回答用户。\n"
        << "处理摘要：\n" << summary.str();

    std::vector<ChatMessage> messages;
    messages.push_back({"system", sys.str()});
    for (const auto& m : history) messages.push_back(m);
    messages.push_back({"user", userMessage});
    std::string err;
    std::string reply = llm_.invokeStream(messages, sink.onToken, &err);
    if (reply.empty()) {
        if (error) *error = err.empty() ? "LLM reply empty" : err;
        return {};
    }
    return reply;
}

} // namespace deepagent
