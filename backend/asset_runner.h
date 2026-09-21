#pragma once

// 资产管线执行器：把 expandPipelines 的图定义装配为 GraphEngine 并执行。
// 设计文档：docs/Node资产生成子步并发设计.md §3。
//
// 并发语义：同一批请求的全部子步（含可选 bg 分支）在同一个引擎的超步里
// 统一调度（worker_count 线程池 + parallel_group）；compose 经 barrier
// AND-join 等齐全部分支。provider 可注入（测试用 FakeProvider）。

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "third_party/json.hpp"

#include <neograph/neograph.h>
#include <neograph/provider.h>

namespace deepagent {

struct AssetRunnerConfig {
    std::string api_key;
    std::string base_url;
    std::string model;
    std::string system_prompt;    // content 分支的 system 消息（空=不加）
    std::string html_output_dir;  // {guid}.html 落盘目录（空=不落盘）
    int worker_count = 4;
    int timeout_seconds = 180;
};

struct AssetRequest {
    std::string guid;
    std::string short_id;
    nlohmann::json user_text;         // string：本轮用户输入（与 messages 二选一）
    nlohmann::json messages;          // [{role,content}]：完整对话（优先于 user_text）
    nlohmann::json context;           // {content,style,...}：适配器分支输入
    bool wants_background = false;
};

struct AssetResult {
    std::string guid;
    bool ok = false;
    std::string final_html;     // final 通道值
    std::string html_file;      // 落盘路径（未落盘为空）
    std::string error;
    std::vector<std::string> executed_steps;  // 按执行顺序的子步节点名
};

class AssetGraphRunner {
public:
    // 子步状态回调：step 为图节点名（{short}_{id}），state ∈ running/done/failed
    using NodeStateCb =
        std::function<void(const std::string& guid, const std::string& step, const std::string& state)>;

    explicit AssetGraphRunner(AssetRunnerConfig config,
                              std::shared_ptr<neograph::Provider> providerOverride = nullptr);

    // 一批节点的管线一次执行（多节点多分支并发）。
    std::vector<AssetResult> run(const std::vector<AssetRequest>& requests,
                                 const nlohmann::json& pipelineSchema, NodeStateCb cb = nullptr);

private:
    neograph::json buildInput(const std::vector<AssetRequest>& requests) const;

    AssetRunnerConfig config_;
    std::shared_ptr<neograph::Provider> provider_override_;
};

}  // namespace deepagent
