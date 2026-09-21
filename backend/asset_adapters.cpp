#include "asset_adapters.h"

#include <mutex>
#include <stdexcept>

#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

namespace deepagent {

// ------------------------------------------------------------------
// 工具：剥 ```html 围栏
// ------------------------------------------------------------------

std::string stripHtmlFence(const std::string& raw) {
    std::string s = raw;
    auto ltrim = [](std::string& v) {
        std::size_t p = v.find_first_not_of(" \t\r\n");
        v.erase(0, p == std::string::npos ? 0 : p);
    };
    auto rtrim = [](std::string& v) {
        std::size_t p = v.find_last_not_of(" \t\r\n");
        v.erase(p == std::string::npos ? 0 : p + 1);
    };
    ltrim(s);
    rtrim(s);
    if (s.rfind("```", 0) == 0) {
        std::size_t nl = s.find('\n');
        if (nl != std::string::npos) s.erase(0, nl + 1);
        std::size_t fence = s.rfind("```");
        if (fence != std::string::npos) s.erase(fence);
        ltrim(s);
        rtrim(s);
    }
    return s;
}

// ------------------------------------------------------------------
// AdapterRegistry
// ------------------------------------------------------------------

AdapterRegistry& AdapterRegistry::instance() {
    static AdapterRegistry registry;
    return registry;
}

void AdapterRegistry::registerAdapter(const std::string& name, AssetAdapterFn fn) {
    adapters_[name] = std::move(fn);
}

bool AdapterRegistry::has(const std::string& name) const {
    return adapters_.count(name) != 0;
}

nlohmann::json AdapterRegistry::invoke(const std::string& name,
                                       const std::vector<nlohmann::json>& inputs,
                                       const nlohmann::json& params) const {
    auto found = adapters_.find(name);
    if (found == adapters_.end())
        throw std::runtime_error("unknown asset adapter: " + name);
    return found->second(inputs, params);
}

// ------------------------------------------------------------------
// 内置适配器 v1（C++；QuickJS 脚本化接口预留：name → script 映射扩展位）
// ------------------------------------------------------------------

namespace {

// bg_plate：由节点上下文（content/style）产出背景板 CSS 片段。
// 输入 inputs[0] = {content, style, ...}；返回 {css, layer}。
nlohmann::json bgPlateAdapter(const std::vector<nlohmann::json>& inputs,
                              const nlohmann::json& params) {
    nlohmann::json ctx = nlohmann::json::object();
    if (!inputs.empty() && inputs[0].is_object()) ctx = inputs[0];
    const std::string style = ctx.value("style", "");

    // v1：样式关键词 → 参数化 CSS（纯确定性，无 LLM）。
    std::string bg = "linear-gradient(135deg, #1e3a5f 0%, #2d5f8a 100%)";
    std::string color = "#ffffff";
    if (style.find("暗") != std::string::npos || style.find("dark") != std::string::npos) {
        bg = "linear-gradient(135deg, #0b0f14 0%, #1c2733 100%)";
        color = "#e8eef4";
    } else if (style.find("暖") != std::string::npos || style.find("warm") != std::string::npos) {
        bg = "linear-gradient(135deg, #5f3a1e 0%, #8a6a2d 100%)";
        color = "#fff6e8";
    }
    if (!params.is_object()) return nlohmann::json{{"css", ""}, {"layer", ""}};
    const std::string extra = params.value("css", "");

    std::string css = "body{background:" + bg + ";color:" + color + ";margin:0;}";
    if (!extra.empty()) css += extra;
    return nlohmann::json{{"css", css}, {"layer", "bg_plate"}};
}

// html_compose：背景板片段 + LLM 内容 → 最终 HTML。
// inputs[0] = bg 片段（{css,...} 或 null），inputs[1] = LLM 内容字符串。
// bg 缺失时退化透传（防御式，为后续运行期动态裁剪留余地）。
nlohmann::json htmlComposeAdapter(const std::vector<nlohmann::json>& inputs,
                                  const nlohmann::json& params) {
    (void)params;
    std::string content;
    if (inputs.size() > 1 && inputs[1].is_string()) content = inputs[1].get<std::string>();

    nlohmann::json bg = nlohmann::json(nullptr);
    if (!inputs.empty() && inputs[0].is_object()) bg = inputs[0];
    if (bg.is_null() || !bg.contains("css") || bg["css"].get<std::string>().empty())
        return content;  // 无背景板：透传 LLM 内容

    const std::string css = bg["css"].get<std::string>();
    return "<!--deepagent:bg_plate-->\n<style>" + css + "</style>\n" + content;
}

// ------------------------------------------------------------------
// 节点类型：da.adapter / da.llm
// ------------------------------------------------------------------

class DaAdapterNode final : public neograph::graph::GraphNode {
public:
    DaAdapterNode(const std::string& name, const neograph::json& config)
        : name_(name),
          adapter_(config.value("adapter", std::string())),
          output_(config.value("output", std::string())) {
        if (config.contains("inputs") && config["inputs"].is_array())
            for (const auto& ch : config["inputs"])
                if (ch.is_string()) inputs_.push_back(ch.get<std::string>());
        if (config.contains("params")) params_ = config["params"];
    }

    asio::awaitable<neograph::graph::NodeOutput> run(
        neograph::graph::NodeInput input) override {
        std::vector<nlohmann::json> values;
        values.reserve(inputs_.size());
        for (const auto& ch : inputs_) {
            const auto v = input.state.get(ch);
            values.push_back(nlohmann::json::parse(v.dump()));
        }
        const nlohmann::json result =
            AdapterRegistry::instance().invoke(adapter_, values,
                                               nlohmann::json::parse(params_.dump()));
        co_return neograph::graph::NodeOutput{
            {neograph::graph::ChannelWrite{output_, neograph::json::parse(result.dump())}}};
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
    std::string adapter_;
    std::string output_;
    std::vector<std::string> inputs_;
    neograph::json params_ = neograph::json::object();
};

class DaLlmNode final : public neograph::graph::GraphNode {
public:
    DaLlmNode(const std::string& name, const neograph::json& config,
              const neograph::graph::NodeContext& ctx)
        : name_(name),
          provider_(ctx.provider),
          prompt_channel_(config.value("prompt_channel", std::string())),
          output_(config.value("output", std::string())),
          model_(config.value("model", std::string())) {}

    asio::awaitable<neograph::graph::NodeOutput> run(
        neograph::graph::NodeInput input) override {
        const auto messages = input.state.get(prompt_channel_);  // [{role,content}]
        neograph::CompletionParams params;
        params.model = model_;
        for (const auto& m : messages)
            params.messages.push_back({m.value("role", "user"), m.value("content", "")});

        const auto completion = co_await provider_->complete_async(params);
        const std::string html = stripHtmlFence(completion.message.content);
        co_return neograph::graph::NodeOutput{
            {neograph::graph::ChannelWrite{output_, neograph::json(html)}}};
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
    std::shared_ptr<neograph::Provider> provider_;
    std::string prompt_channel_;
    std::string output_;
    std::string model_;
};

}  // namespace

void ensureAssetNodesRegistered() {
    static bool done = false;
    static std::once_flag flag;
    std::call_once(flag, [] {
        AdapterRegistry::instance().registerAdapter("bg_plate", bgPlateAdapter);
        AdapterRegistry::instance().registerAdapter("html_compose", htmlComposeAdapter);

        auto& factory = neograph::graph::NodeFactory::instance();
        // config schema 声明节点全部 config 键（strict consumed-key 校验要求）
        factory.register_type(
            "da.adapter",
            [](const std::string& name, const neograph::json& config,
               const neograph::graph::NodeContext&) {
                return std::make_unique<DaAdapterNode>(name, config);
            },
            neograph::json{
                {"type", "object"},
                {"properties",
                 {{"adapter", {{"type", "string"}}},
                  {"inputs", {{"type", "array"}}},
                  {"output", {{"type", "string"}}},
                  {"params", {{"type", "object"}}},
                  {"barrier", {{"type", "object"}}}}}},
            neograph::json::object());
        factory.register_type(
            "da.llm",
            [](const std::string& name, const neograph::json& config,
               const neograph::graph::NodeContext& ctx) {
                return std::make_unique<DaLlmNode>(name, config, ctx);
            },
            neograph::json{
                {"type", "object"},
                {"properties",
                 {{"prompt_channel", {{"type", "string"}}},
                  {"output", {{"type", "string"}}},
                  {"model", {{"type", "string"}}}}}},
            neograph::json::object());
        (void)done;
    });
}

}  // namespace deepagent
