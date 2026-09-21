#include "asset_pipeline.h"

#include <algorithm>
#include <cctype>

namespace deepagent {

namespace {

// 背景板关键词词表（小写匹配；中英文）。
// 命中后做"就近否定"判定：同一小句内（标点截断、回看 ≤8 个汉字宽度）出现
// 否定词（不需要/不要/no/without…）则该次命中作废；全部命中都被否定才视为
// 不启用——"不要白色，要加背景板"这类混合表述仍能正确启用。

const char* const kBgWords[] = {
    "背景板", "背景图", "背景色", "加背景", "带背景", "需要背景", "要背景",
    "配背景", "背景效果", "background", "backdrop"};

const char* const kNegZh[] = {
    "不需要", "不用", "不要", "不需", "无需", "不加", "不带", "没有", "去掉",
    "去除", "移除", "别加", "别用", "不想", "不使用", "不要用", "无"};

const char* const kNegEn[] = {
    "no ", "without ", "not ", "don't ", "dont ", "never "};

const char* const kSentenceBreaks[] = {
    "，", "。", "；", "！", "？", "、", ":", ",", ";", "!", "?", ".", "\r", "\n"};

bool isAlnumByte(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

// 关键词出现位置之前（同一小句内）是否有否定词。
// 允许否定词与关键词首字重叠（"不要背景"= 否定"不要"+正词"要背景"共用"要"）。
bool negatedNearby(const std::string& lower, size_t keywordPos) {
    const size_t maxBack = 24;  // 约 8 个汉字的回看宽度
    const size_t extend = 6;    // 重叠容许：否定词尾可探入关键词前 2 个汉字
    const size_t begin = keywordPos > maxBack ? keywordPos - maxBack : 0;
    const size_t end = std::min(lower.size(), keywordPos + extend);
    const size_t kwRel = keywordPos - begin;  // 关键词在窗口内的相对位置
    const std::string window = lower.substr(begin, end - begin);
    // 标点截断：只看关键词所在的最后一个小句（截断点须在关键词之前）
    size_t lastBreak = std::string::npos;
    for (const char* b : kSentenceBreaks) {
        const size_t p = window.rfind(b);
        if (p != std::string::npos && p < kwRel &&
            (lastBreak == std::string::npos || p > lastBreak))
            lastBreak = p;
    }
    const size_t segBegin = (lastBreak == std::string::npos) ? 0 : lastBreak + 1;
    const std::string segment = window.substr(segBegin);
    for (const char* neg : kNegZh) {
        size_t p = segment.find(neg);
        while (p != std::string::npos) {
            if (segBegin + p < kwRel) return true;  // 否定词起点在关键词之前
            p = segment.find(neg, p + 1);
        }
    }
    for (const char* neg : kNegEn) {
        size_t p = segment.rfind(neg);
        if (p == std::string::npos) continue;
        if (segBegin + p >= kwRel) continue;
        // 英文 token 要求左边界（前一字节非字母数字），避免 studio 内含 no 之类误报
        const size_t absPos = begin + segBegin + p;
        if (absPos == 0 || !isAlnumByte(static_cast<unsigned char>(lower[absPos - 1])))
            return true;
    }
    return false;
}

bool containsKeyword(const std::string& haystack) {
    std::string lower;
    lower.reserve(haystack.size());
    for (char c : haystack) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    for (const char* w : kBgWords) {
        size_t pos = 0;
        while ((pos = lower.find(w, pos)) != std::string::npos) {
            if (!negatedNearby(lower, pos)) return true;  // 存在任一未被否定的命中即启用
            pos += 1;
        }
    }
    return false;
}

}  // namespace

bool wantsBackground(std::optional<bool> explicitFlag, const std::string& text,
                     const std::string& stylePrompt) {
    if (explicitFlag.has_value()) return *explicitFlag;
    return containsKeyword(text) || containsKeyword(stylePrompt);
}

nlohmann::json defaultSubtitlePipeline() {
    return nlohmann::json::parse(R"({
        "branches": [
            {"id": "bg", "kind": "adapter", "adapter": "bg_plate", "optional": true},
            {"id": "content", "kind": "llm_call"}
        ],
        "compose": {"adapter": "html_compose"}
    })");
}

std::string assetPromptChannel(const std::string& shortId) { return "asset_" + shortId + "_prompt"; }
std::string assetCtxChannel(const std::string& shortId) { return "asset_" + shortId + "_ctx"; }
std::string assetFinalChannel(const std::string& shortId) { return "asset_" + shortId + "_final"; }

AssetGraphPlan expandPipelines(const std::vector<AssetNodeInput>& nodes,
                               const nlohmann::json& pipelineSchema, std::string* error) {
    AssetGraphPlan plan;
    if (nodes.empty()) {
        if (error) *error = "expandPipelines: no asset nodes";
        return plan;
    }

    // ---- 解析 schema：分支清单 + compose ----
    struct BranchSpec {
        std::string id;
        std::string kind;     // "adapter" | "llm_call"
        std::string adapter;  // kind=adapter 时
        bool optional = false;
    };
    std::vector<BranchSpec> branches;
    if (pipelineSchema.is_object() && pipelineSchema.contains("branches") &&
        pipelineSchema["branches"].is_array()) {
        for (const auto& b : pipelineSchema["branches"]) {
            BranchSpec spec;
            spec.id = b.value("id", "");
            spec.kind = b.value("kind", "");
            spec.adapter = b.value("adapter", "");
            spec.optional = b.value("optional", false);
            if (spec.id.empty() || (spec.kind != "adapter" && spec.kind != "llm_call")) {
                if (error) *error = "asset_pipeline.branches: invalid branch (id/kind)";
                return plan;
            }
            if (spec.kind == "adapter" && spec.adapter.empty()) {
                if (error) *error = "asset_pipeline.branches: adapter branch missing adapter name";
                return plan;
            }
            branches.push_back(std::move(spec));
        }
    }
    if (branches.empty()) {
        // 隐式：单 content 分支（向后兼容，等价旧单步管线）
        branches.push_back({"content", "llm_call", "", false});
    }
    const bool has_compose =
        branches.size() > 1 ||
        (pipelineSchema.is_object() && pipelineSchema.contains("compose") && branches.size() > 1);
    std::string compose_adapter = "html_compose";
    if (pipelineSchema.is_object() && pipelineSchema.contains("compose"))
        compose_adapter = pipelineSchema["compose"].value("adapter", compose_adapter);

    // ---- 展开为 NeoGraph 图定义 ----
    neograph::json channels = neograph::json::object();
    neograph::json nodes_json = neograph::json::object();
    neograph::json edges = neograph::json::array();

    for (const auto& in : nodes) {
        const std::string& s = in.short_id;
        const std::string prompt_ch = assetPromptChannel(s);
        const std::string ctx_ch = assetCtxChannel(s);
        const std::string final_ch = assetFinalChannel(s);

        // 公共输入通道（runner 经 RunConfig.input 种入初值）
        channels[prompt_ch] = {{"reducer", "overwrite"}, {"initial", neograph::json::array()}};
        channels[ctx_ch] = {{"reducer", "overwrite"}, {"initial", neograph::json::object()}};

        // 纳入本节点的分支（optional 的 adapter 分支按 wants_background 裁剪）
        std::vector<const BranchSpec*> active;
        for (const auto& b : branches) {
            if (b.optional && b.kind == "adapter" && !in.wants_background) continue;
            active.push_back(&b);
        }
        if (active.empty()) {
            if (error) *error = "pipeline has no active branches for node " + in.guid;
            return plan;
        }

        std::vector<std::string> fragment_channels;
        std::vector<std::string> branch_node_names;
        for (const auto* b : active) {
            const std::string node_name = s + "_" + b->id;
            const std::string frag_ch = "asset_" + s + "_" + b->id + "_fragment";
            channels[frag_ch] = {{"reducer", "overwrite"}};

            if (b->kind == "llm_call") {
                nodes_json[node_name] = {{"type", "da.llm"},
                                         {"prompt_channel", prompt_ch},
                                         {"output", frag_ch}};
            } else {
                nodes_json[node_name] = {{"type", "da.adapter"},
                                         {"adapter", b->adapter},
                                         {"inputs", neograph::json::array({ctx_ch})},
                                         {"output", frag_ch}};
                plan.adapter_nodes.push_back(node_name);
            }
            plan.node_names.push_back(node_name);
            fragment_channels.push_back(frag_ch);
            branch_node_names.push_back(node_name);
            edges.push_back({{"from", "__start__"}, {"to", node_name}});
        }

        if (active.size() > 1 && has_compose) {
            // 多分支：compose 汇聚（AND-join barrier）→ final 通道
            const std::string compose_name = s + "_compose";
            neograph::json inputs_arr = neograph::json::array();
            neograph::json wait_for = neograph::json::array();
            for (std::size_t i = 0; i < branch_node_names.size(); ++i) {
                inputs_arr.push_back(fragment_channels[i]);
                wait_for.push_back(branch_node_names[i]);
            }
            nodes_json[compose_name] = {{"type", "da.adapter"},
                                        {"adapter", compose_adapter},
                                        {"inputs", inputs_arr},
                                        {"output", final_ch},
                                        {"barrier", {{"wait_for", wait_for}}}};
            channels[final_ch] = {{"reducer", "overwrite"}};
            plan.adapter_nodes.push_back(compose_name);
            plan.node_names.push_back(compose_name);
            for (const auto& bn : branch_node_names)
                edges.push_back({{"from", bn}, {"to", compose_name}});
            edges.push_back({{"from", compose_name}, {"to", "__end__"}});
        } else {
            // 单分支：输出直写 final 通道（与旧单步行为等价）
            const std::string only = branch_node_names.front();
            if (nodes_json[only].value("type", std::string()) == "da.llm") {
                nodes_json[only]["output"] = final_ch;
            } else {
                nodes_json[only]["output"] = final_ch;
            }
            channels[final_ch] = {{"reducer", "overwrite"}};
            edges.push_back({{"from", only}, {"to", "__end__"}});
            // 单 adapter 分支也读 ctx（inputs 已是 ctx）；llm 分支读 prompt。
        }
        (void)ctx_ch;
    }

    neograph::json definition = neograph::json::object();
    definition["schema_version"] = neograph::graph::TOPOLOGY_SCHEMA_VERSION;
    definition["name"] = "deepagent-asset-pipeline";
    definition["channels"] = std::move(channels);
    definition["nodes"] = std::move(nodes_json);
    definition["edges"] = std::move(edges);
    plan.definition = std::move(definition);
    return plan;
}

}  // namespace deepagent
