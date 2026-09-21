#include "agent_graph.h"

#include <fstream>
#include <string.h>
#include <sstream>

#include "asset_pipeline.h"
#include "asset_runner.h"

#include <neograph/llm/agent.h>
#include <neograph/llm/openai_provider.h>

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

std::string statusText(const json& entry) {
    return entry.value("name", "") + " (" + entry.value("module_id", "") + ")";
}

// 编排者 system 提示：工具使用策略 + 知识库清单（工具定义由 SDK 注入）
std::string buildOrchestratorInstructions(const KnowledgeBase& kb) {
    std::ostringstream sys;
    sys << "你是 DeepAgent，运行在 Graph_Saturn 图形工作区（U-DeepRT/DeepRT 引擎）的 AI 助手。\n"
        << "你可以调用工具完成用户的图工程与资产生成需求。使用工具的策略：\n"
        << "1. 用户询问系统能力或闲聊：直接回答（可介绍知识库中的工具 Node），不要调用工具。\n"
        << "2. 用户需要生成图工程/资产：先调 list_knowledge_nodes 了解可用工具 Node，\n"
        << "   再调 create_udrt（字幕类条目必须带 subtitle_text 与 style_prompt）。\n"
        << "3. 用户要求修改已生成资产：调 generate_node_asset（kind=prompt）。\n"
        << "4. 用户询问某节点状态：调 get_node_context。\n"
        << "外部知识库当前条目：\n" << kb.toPromptText() << "\n"
        << "始终用中文回答。生成类工具调用完成后，在最终回答中报告产出文件路径。\n";
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
            "background=是否生成背景板并合成（可空，缺省按文本内容自动判断）。",
            neograph::json{
                {"type", "object"},
                {"properties", {
                    {"guid", {{"type", "string"}, {"description", "节点 GUID"}}},
                    {"kind", {{"type", "string"}, {"description", "content=新建; prompt=按指令修改"}}},
                    {"text", {{"type", "string"}, {"description", "字幕源文本或修改指令"}}},
                    {"background", {{"type", "boolean"}, {"description", "是否启用背景板分支（可空=自动判断）"}}}
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
            background);
    }

    std::string get_name() const override { return "generate_node_asset"; }
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
                    {"known_ids", knownIds}}.dump();
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
    if (!compiler_.compile(plan, &udrt, &cerr)) {
        return json{{"error", "compile failed: " + cerr}}.dump();
    }
    if (!compiler_.validate(udrt, &cerr)) {
        return json{{"error", "validate failed: " + cerr}}.dump();
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
        AssetGraphRunner runner(rc);

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
    return ok.dump();
}

std::string AgentGraph::toolGenerateNodeAsset(const std::string& guid,
                                              const std::string& kind,
                                              const std::string& text,
                                              std::optional<bool> background) {
    if (guid.empty() || text.empty())
        return json{{"error", "guid and text are required"}}.dump();

    json messages;
    std::string err;
    if (!nodeContexts_.beginTurn(guid, kind, kSubtitleSystemPrompt, text, &messages, &err))
        return json{{"error", err}}.dump();

    // 子步管线：完整多轮上下文作为 content 分支输入；可选背景板分支并发合成
    AssetRunnerConfig rc;
    rc.api_key = config_.llm_api_key;
    rc.base_url = config_.llm_base_url;
    rc.model = config_.llm_model;
    rc.system_prompt = kSubtitleSystemPrompt;
    AssetGraphRunner runner(rc);

    AssetRequest req;
    req.guid = guid;
    req.short_id = guid.size() >= 8 ? guid.substr(0, 8) : guid;
    req.messages = messages;
    req.context = json{{"content", text}, {"style", kind == "prompt" ? text : ""}};
    req.wants_background = wantsBackground(background, text, "");

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
        return json{{"error", fail}}.dump();
    }
    nodeContexts_.completeTurn(guid, r.final_html);

    return json{{"state", "done"}, {"html", r.final_html}, {"guid", guid}}.dump();
}

std::string AgentGraph::toolGetNodeContext(const std::string& guid) const {
    json ctx = nodeContexts_.getContext(guid);
    if (ctx.is_null()) return json{{"error", "context not found"}}.dump();
    return ctx.dump();
}

// ------------------------------------------------------------------
// run()：Agent Loop 主路径（LLM 自主工具调用），异常回退固定管线
// ------------------------------------------------------------------

std::string AgentGraph::run(const std::string& userMessage,
                            const std::vector<ChatMessage>& history,
                            const EventSink& sink, std::string* error) {
    m_activeSink = &sink;
    std::string reply;
    try {
        // 1. OpenAI 兼容 provider（DeepSeek）
        neograph::llm::OpenAIProvider::Config pc;
        pc.api_key = config_.llm_api_key;
        pc.base_url = config_.llm_base_url;
        pc.default_model = config_.llm_model;
        pc.timeout_seconds = 180;
        auto provider = neograph::llm::OpenAIProvider::create(pc);

        // 2. 工具集（Agent Loop 中 LLM 自主调用）
        std::vector<std::unique_ptr<neograph::Tool>> tools;
        tools.push_back(std::make_unique<ListKnowledgeNodesTool>(this));
        tools.push_back(std::make_unique<CreateUdrtTool>(this));
        tools.push_back(std::make_unique<GenerateNodeAssetTool>(this));
        tools.push_back(std::make_unique<GetNodeContextTool>(this));

        // 3. Agent Loop（LLM 自主决定调用工具的次数与顺序，max_iterations 止损）
        neograph::llm::Agent agent(std::move(provider), std::move(tools),
                                   buildOrchestratorInstructions(kb_),
                                   config_.llm_model);
        agent.set_tool_detection_timeout_seconds(120);

        std::vector<neograph::ChatMessage> messages;
        for (const auto& h : history)
            messages.push_back({h.role, h.content});
        messages.push_back({"user", userMessage});

        reply = agent.run_stream(messages,
            [&](const std::string& token) {
                if (sink.onToken) sink.onToken(token);
            }, /*max_iterations=*/8);

        if (reply.empty() && error) *error = "empty agent reply";
    } catch (const std::exception& e) {
        // Agent Loop 异常：回退旧固定管线
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
    return plan;
}

bool AgentGraph::planGraph(const json& entry, const json& intent,
                           const std::string& userMessage, json* planIr,
                           bool* usedTemplate) {
    (void)intent;
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
    std::ostringstream sys;
    sys << "你是图形计划助手。请为用户需求生成 Graph Plan IR（语义层，不是 udrt）。\n"
        << "可用节点目录（catalog key 与语义端口）：\n" << catalogBrief.dump() << "\n"
        << "参考连线模板：\n" << tpl.dump() << "\n"
        << "只输出 JSON：{\"nodes\": [{\"key\": \"实例名\", \"catalog\": \"目录key\"}], "
        << "\"links\": [{\"from\": \"实例名:输出端口\", \"to\": \"实例名:输入端口\"}]}\n"
        << "要求：与模板等价即可，不得引入目录之外的节点或端口。";
    std::vector<ChatMessage> messages;
    messages.push_back({"system", sys.str()});
    messages.push_back({"user", userMessage});
    std::string err;
    std::string raw = llm_.invoke(messages, &err, /*temperature=*/0.2);
    json plan;
    bool ok = false;
    if (!raw.empty() && extractJsonObject(raw, &plan)) {
        if (plan.contains("nodes") && plan.contains("links")) {
            ok = true;
            for (const auto& pn : plan["nodes"]) {
                if (!catalog_.node(pn.value("catalog", ""))) {
                    ok = false;
                    break;
                }
            }
        }
    }
    if (!ok) {
        *planIr = templatePlan(entry);
        *usedTemplate = true;
        return true;
    }
    *planIr = plan;
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
