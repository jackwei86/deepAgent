#include "asset_runner.h"

#include <chrono>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>

#include "asset_adapters.h"
#include "asset_pipeline.h"

#include <neograph/llm/openai_provider.h>

namespace deepagent {

namespace {

std::string shortIdOf(const std::string& guid) {
    return guid.size() >= 8 ? guid.substr(0, 8) : guid;
}

std::string effectiveShortId(const AssetRequest& req) {
    return req.short_id.empty() ? shortIdOf(req.guid) : req.short_id;
}

}  // namespace

AssetGraphRunner::AssetGraphRunner(AssetRunnerConfig config,
                                   std::shared_ptr<neograph::Provider> providerOverride)
    : config_(std::move(config)), provider_override_(std::move(providerOverride)) {
    ensureAssetNodesRegistered();
}

neograph::json AssetGraphRunner::buildInput(const std::vector<AssetRequest>& requests) const {
    neograph::json input = neograph::json::object();
    for (const auto& req : requests) {
        const std::string s = effectiveShortId(req);

        // content 分支的对话输入：完整 messages 优先，否则 system + user_text
        neograph::json messages = neograph::json::array();
        if (req.messages.is_array() && !req.messages.empty()) {
            messages = neograph::json::parse(req.messages.dump());
        } else {
            if (!config_.system_prompt.empty())
                messages.push_back({{"role", "system"}, {"content", config_.system_prompt}});
            std::string text = req.user_text.is_string() ? req.user_text.get<std::string>() : "";
            // V1.5.1: 样式要求拼入用户消息（AssetContext.style_prompt 经此进入生成，
            // notify 重生成才能保持原始样式；与 legacy 管线"样式要求："惯例一致）
            std::string style;
            if (req.context.is_object() && req.context.contains("style") &&
                req.context["style"].is_string())
                style = req.context["style"].get<std::string>();
            if (!style.empty() && !text.empty()) text += "\n\n样式要求：" + style;
            if (!text.empty()) messages.push_back({{"role", "user"}, {"content", text}});
        }
        input[assetPromptChannel(s)] = std::move(messages);

        // 适配器分支的上下文输入
        neograph::json ctx = neograph::json::object();
        if (req.context.is_object()) ctx = neograph::json::parse(req.context.dump());
        input[assetCtxChannel(s)] = std::move(ctx);
    }
    return input;
}

std::vector<AssetResult> AssetGraphRunner::run(const std::vector<AssetRequest>& requests,
                                               const nlohmann::json& pipelineSchema,
                                               NodeStateCb cb) {
    std::vector<AssetResult> results;
    results.reserve(requests.size());
    for (const auto& req : requests) {
        AssetResult r;
        r.guid = req.guid;
        results.push_back(std::move(r));
    }
    if (requests.empty()) return results;

    // ---- 展开 ----
    std::vector<AssetNodeInput> nodes;
    nodes.reserve(requests.size());
    for (const auto& req : requests)
        nodes.push_back({req.guid, effectiveShortId(req), req.wants_background});
    std::string expand_error;
    AssetGraphPlan plan = expandPipelines(nodes, pipelineSchema, &expand_error);
    if (plan.definition.is_null()) {
        for (auto& r : results) r.error = expand_error;
        return results;
    }

    // guid 前缀 → 子步节点名映射（事件回调定位用）
    std::map<std::string, std::vector<std::string>> steps_of;
    for (std::size_t i = 0; i < requests.size(); ++i)
        for (const auto& name : plan.node_names)
            if (name.rfind(effectiveShortId(requests[i]) + "_", 0) == 0)
                steps_of[requests[i].guid].push_back(name);
    std::map<std::string, std::string> step_owner;  // 节点名 → guid
    for (const auto& [guid, names] : steps_of)
        for (const auto& n : names) step_owner[n] = guid;

    // ---- Provider ----
    std::shared_ptr<neograph::Provider> provider = provider_override_;
    if (!provider) {
        neograph::llm::OpenAIProvider::Config pc;
        pc.api_key = config_.api_key;
        pc.base_url = config_.base_url;
        pc.default_model = config_.model;
        pc.timeout_seconds = config_.timeout_seconds;
        provider = neograph::llm::OpenAIProvider::create(pc);
    }

    // ---- 装配引擎 ----
    neograph::graph::NodeContext node_ctx;
    node_ctx.provider = provider;
    node_ctx.model = config_.model;

    neograph::graph::EngineConfig engine_config;
    engine_config.node_context = node_ctx;
    engine_config.checkpoint_store = std::make_shared<neograph::graph::InMemoryCheckpointStore>();
    engine_config.worker_count = static_cast<std::size_t>(config_.worker_count);
    // 适配器子步默认重试一次（确定性过程，瞬时失败可救；LLM 分支不重试、快速失败）
    neograph::graph::RetryPolicy adapter_retry;
    adapter_retry.max_retries = 1;
    adapter_retry.initial_delay_ms = 50;
    for (const auto& name : plan.adapter_nodes)
        engine_config.node_retry_policies[name] = adapter_retry;

    auto engine = neograph::graph::GraphEngine::build_strict(plan.definition,
                                                             std::move(engine_config));

    // ---- 执行（事件流回调转子步状态）----
    neograph::graph::RunConfig run_config;
    run_config.thread_id = "deepagent-assets";
    run_config.input = buildInput(requests);
    run_config.stream_mode = neograph::graph::StreamMode::EVENTS;

    std::set<std::string> done_steps;
    neograph::graph::GraphStreamCallback stream_cb =
        [&](const neograph::graph::GraphEvent& event) {
            if (event.node_name.empty()) return;
            const std::string step = event.node_name;
            const auto owner = step_owner.find(step);
            if (owner == step_owner.end()) return;  // __routing__ 等系统事件
            if (event.type == neograph::graph::GraphEvent::Type::NODE_START) {
                if (cb) cb(owner->second, step, "running");
            } else if (event.type == neograph::graph::GraphEvent::Type::NODE_END) {
                done_steps.insert(step);
                if (cb) cb(owner->second, step, "done");
            }
        };

    neograph::graph::RunResult run_result;
    try {
        run_result = engine->run_stream(run_config, stream_cb);
    } catch (const std::exception& e) {
        for (auto& r : results) {
            r.error = std::string("asset pipeline failed: ") + e.what();
            if (cb)
                for (const auto& step : steps_of[r.guid])
                    if (!done_steps.count(step)) cb(r.guid, step, "failed");
        }
        return results;
    }

    if (run_result.status() != neograph::graph::RunStatus::Completed) {
        for (auto& r : results)
            r.error = "asset pipeline did not complete (status=" +
                      std::to_string(static_cast<int>(run_result.status())) + ")";
        return results;
    }

    // ---- 收集结果 ----
    for (std::size_t i = 0; i < requests.size(); ++i) {
        auto& r = results[i];
        const std::string s = effectiveShortId(requests[i]);
        const auto& channels = run_result.output["channels"];
        if (channels.contains(assetFinalChannel(s))) {
            const auto& value = channels[assetFinalChannel(s)]["value"];
            r.final_html = value.is_string() ? value.get<std::string>() : value.dump();
            r.ok = true;
        } else {
            r.error = "final channel missing: " + assetFinalChannel(s);
            continue;
        }
        for (const auto& step : run_result.execution_trace)
            if (step.rfind(s + "_", 0) == 0) r.executed_steps.push_back(step);

        if (!config_.html_output_dir.empty()) {
            const std::string path = config_.html_output_dir + "\\" + requests[i].guid + ".html";
            std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
            if (ofs) {
                ofs.write(r.final_html.data(), (std::streamsize)r.final_html.size());
                r.html_file = path;
            }
        }
    }
    return results;
}

}  // namespace deepagent
