#pragma once

// Node 资产生成子步并发：管线 schema 解析 + NeoGraph 图定义展开（纯函数）。
// 设计文档：docs/Node资产生成子步并发设计.md
//
// 职责边界：本模块只做"声明 → 图定义"的确定性翻译，不做执行、不碰 LLM、
// 不碰磁盘。执行见 asset_runner，适配器见 asset_adapters。

#include <optional>
#include <string>
#include <vector>

#include "third_party/json.hpp"

#include <neograph/neograph.h>

namespace deepagent {

// 一个待生成资产的 udrt 节点（展开输入）。
struct AssetNodeInput {
    std::string guid;      // 完整节点 GUID（资产文件名用）
    std::string short_id;  // GUID 前 8 位（图节点/通道名用，调用方保证合法且唯一）
    bool wants_background = false;
};

// 展开结果：图定义 + runner 需要的元信息。
struct AssetGraphPlan {
    neograph::json definition;             // NeoGraph 图定义（build_strict 直接可用）
    std::vector<std::string> adapter_nodes; // 全部 da.adapter 节点名（默认配重试）
    std::vector<std::string> node_names;    // 全部子步节点名（事件映射用）
};

// 背景板启用判定：显式参数 > prompt 关键词规则（纯函数，可单测）。
bool wantsBackground(std::optional<bool> explicitFlag, const std::string& text,
                     const std::string& stylePrompt);

// 字幕类条目的默认子步管线（可选背景板分支 ∥ LLM 内容分支 → html_compose 合成）。
// knowledge_base.json 条目可带同形 asset_pipeline 覆盖；缺省用本定义。
nlohmann::json defaultSubtitlePipeline();

// 通道名约定（expander 与 runner 共用）
std::string assetPromptChannel(const std::string& shortId);   // asset_{id}_prompt
std::string assetCtxChannel(const std::string& shortId);      // asset_{id}_ctx
std::string assetFinalChannel(const std::string& shortId);    // asset_{id}_final

// 把管线展开为 NeoGraph 图定义。
// pipelineSchema：知识库条目的 asset_pipeline 对象；空/缺省 = 隐式单 content 分支
// （与旧单步行为等价）。optional 的 adapter 分支仅当对应节点 wants_background
// 时纳入。多节点时全部子步合并进同一张图（跨节点统一超步调度）。
// 失败返回 plan.definition 为 null，错误写 *error。
AssetGraphPlan expandPipelines(const std::vector<AssetNodeInput>& nodes,
                               const nlohmann::json& pipelineSchema, std::string* error);

}  // namespace deepagent
