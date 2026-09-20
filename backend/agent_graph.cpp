#include "agent_graph.h"

#include <fstream>
#include <string.h>
#include <sstream>

using json = nlohmann::json;

namespace deepagent {
namespace {

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

} // namespace

AgentGraph::AgentGraph(const Config& config, LlmClient& llm, const KnowledgeBase& kb,
                       const NodeCatalog& catalog, const UdrtCompiler& compiler)
    : config_(config), llm_(llm), kb_(kb), catalog_(catalog), compiler_(compiler) {}

json AgentGraph::analyzeIntent(const std::string& userMessage, std::string* llmError) {
    std::ostringstream sys;
    sys << "你是 DeepAgent，一个分析用户需求并为图形工作区选择工具节点的助手。\n"
        << "工作区为 Graph_Saturn（U-DeepRT/DeepRT 图形引擎）。以下是外部知识库中已登记的工具 Node 条目：\n"
        << kb_.toPromptText() << "\n\n"
        << "任务：判断用户的这条消息是否需要调用上述某个工具 Node（是否需要生成 .udrt 图工程文件）。\n"
        << "只输出一个 JSON 对象，不要输出其他文字，格式：\n"
        << "{\"is_tool_task\": true|false, \"entry_id\": \"知识库条目id或null\", "
        << "\"subtitle_text\": \"若消息中包含用户希望使用的字幕对白文本则原样摘出，否则为空字符串\", "
        << "\"style_prompt\": \"若消息中包含对字幕样式/风格的额外要求（如字体、字号、颜色、加粗、描边、动画等）"
        << "则浓缩为一条给字幕生成节点的修改指令（如\\\"字体：宋体；字号：36；颜色：红色\\\"），否则为空字符串\", "
        << "\"reason\": \"一句话理由\"}\n"
        << "规则：\n"
        << "1. 只有当用户明确或隐含地需要知识库中某工具 Node 的能力时 is_tool_task 才为 true；\n"
        << "2. entry_id 必须严格等于知识库条目的 id；普通闲聊/与工具无关的问题 is_tool_task 为 false、entry_id 为 null；\n"
        << "3. 若用户想生成字幕 HTML / 字幕文件 / 网页字幕等，应选择字幕相关的工具 Node；\n"
        << "4. style_prompt 只收集风格/字体/颜色等外观要求，不要混入字幕对白内容本身。";
    std::vector<ChatMessage> messages = {
        {"system", sys.str()},
        {"user", userMessage},
    };
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
    *usedTemplate = false;

    // Ask the LLM for a Plan IR referencing catalog keys / semantic ports only.
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
    std::vector<ChatMessage> messages = {
        {"system", sys.str()},
        {"user", userMessage},
    };
    std::string err;
    std::string raw = llm_.invoke(messages, &err, /*temperature=*/0.2);
    json plan;
    bool ok = false;
    if (!raw.empty() && extractJsonObject(raw, &plan)) {
        if (plan.contains("nodes") && plan.contains("links")) {
            // pre-validate: every catalog key must exist
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

std::string AgentGraph::run(const std::string& userMessage,
                            const std::vector<ChatMessage>& history,
                            const EventSink& sink, std::string* error) {
    auto status = [&sink](const std::string& s) {
        if (sink.onStatus) sink.onStatus(s);
    };
    // 协议中传递相对路径（相对公共部署目录 exe_dir）；UI 显示用 absolute_path
    auto makeRelative = [this](const std::string& absPath) -> std::string {
        std::string base = config_.exe_dir + "\\";
        if (absPath.size() > base.size() &&
            _strnicmp(absPath.c_str(), base.c_str(), base.size()) == 0)
            return absPath.substr(base.size());
        return absPath;
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
        // ---- plain chat path ----
        status("未检测到需要调用的工具 Node，进入普通对话");
        std::ostringstream sys;
        sys << "你是 DeepAgent，运行在 Graph_Saturn 图形工作区的 AI 助手。"
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

    // ---- Step 4: compile_udrt ----
    // props overrides aligned with subtitle_gen::CDyProps:
    //   strContent  <- user subtitle text (intent analysis)
    //   strPrompt   <- style/font requirements extracted from the prompt
    //   strApiKey   <- DeepAgent's own LLM api key (config/.env DEEPSEEK_API_KEY)
    //   strModel    <- DeepAgent's own LLM model   (config/.env DEEPSEEK_MODEL)
    // Only keys declared in the node catalog are set; the compiler merges
    // these overrides on top of the catalog defaults.
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
        payload["file"] = makeRelative(path);              // 协议中传相对路径（相对公共部署目录）
        payload["absolute_path"] = path;                   // UI 显示用绝对路径
        payload["name"] = entry->value("name", "");
        payload["module_id"] = entry->value("module_id", "");
        payload["node_count"] = udrt["nodes"].size();
        payload["connection_count"] = udrt["connections"].size();
        payload["content"] = udrt;
        sink.onUdrt(payload);
    }

    // ---- Step 5.5: 资产生成（字幕场景：对话中直接产出 HTML 资产，无需等 U-DeepRT 运行节点）----
    if (entry->value("id", "") == "saturn_subtitle_html" && !subtitleText.empty()) {
        status("正在生成 HTML 资产…");
        std::vector<ChatMessage> gen;
        gen.push_back({"system", kSubtitleSystemPrompt});
        std::string userText = subtitleText;
        if (!stylePrompt.empty()) userText += "\n\n样式要求：" + stylePrompt;
        gen.push_back({"user", userText});
        std::string htmlErr;
        std::string html = StripHtmlFence(llm_.invoke(gen, &htmlErr, /*temperature=*/0.3));
        if (!html.empty()) {
            // 资产以 GUID 命名，与 udrt 同目录（公共部署目录），U-DeepRT 中的节点
            // 按 guid 即可定位消费（相对路径 udrt\{guid}.html）
            std::string guid = udrt["nodes"][0]["internal-data"].value("guid", std::string());
            std::string assetPath = config_.udrt_output_dir + "\\" + guid + ".html";
            std::ofstream ofs(assetPath, std::ios::binary | std::ios::trunc);
            if (ofs) ofs.write(html.data(), (std::streamsize)html.size());
            ofs.close();

            status("已生成 HTML 资产：" + assetPath);
            if (sink.onAsset) {
                json asset{{"type", "html"},
                           {"file", makeRelative(assetPath)},
                           {"absolute_path", assetPath},
                           {"guid", guid}, {"version", 1}};
                sink.onAsset(asset);
            }
        } else {
            status("HTML 资产生成失败：" + (htmlErr.empty() ? "empty reply" : htmlErr));
        }
    }

    // ---- Step 6: reply (streaming) ----
    status("正在生成回复…");
    std::ostringstream summary;
    summary << "本次任务执行结果：\n"
            << "- 选中的工具 Node: " << statusText(*entry) << "\n"
            << "- 知识库条目: " << entry->value("id", "") << "\n"
            << "- 生成的 udrt 文件: " << path << "\n"
            << "- 节点数: " << udrt["nodes"].size() << ", 连接数: " << udrt["connections"].size() << "\n"
            << "- 节点清单:";
    for (const auto& n : udrt["nodes"]) {
        summary << " " << n["internal-data"]["model_comment"].get<std::string>()
                << "(" << n["internal-data"]["model_name"].get<std::string>() << ";";
    }
    summary << "\n- udrt JSON:\n" << udrt.dump(2) << "\n";

    std::ostringstream sys;
    sys << "你是 DeepAgent，Graph_Saturn 图形工作区的 AI 助手。工具调用已完成，"
        << "请用中文向用户简洁汇报：选择了哪个工具 Node、为什么（结合知识库条目描述）、"
        << "udrt 文件保存到了哪里、包含哪些节点与连接，并提示可以在 U-DeepRT 中打开该工程。"
        << "不要逐字罗列 JSON 全文。\n\n" << summary.str();
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
