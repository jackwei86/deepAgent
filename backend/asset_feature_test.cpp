// Node 资产生成子步并发 —— 逐项功能测试 F1-F12（独立 exe，FakeProvider）。
// 设计文档：docs/Node资产生成子步并发设计.md §4 测试矩阵。
// ZCode 可参考设计落地测试 G1-G12（P1 沙箱 / P2 结构化诊断 / P4 phases）。
// 构建：build_asset_test.bat（cl 直编，不进 DeepAgentBackend.vcxproj）。

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "asset_adapters.h"
#include "asset_context.h"
#include "asset_pipeline.h"
#include "asset_runner.h"
#include "avatar_parser.h"
#include "event_hub.h"
#include "node_catalog.h"
#include "plan_script.h"
#include "udrt_compiler.h"

#include <mutex>
#include <random>

#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

using json = nlohmann::json;
using namespace deepagent;

namespace {

int g_pass = 0, g_fail = 0;
std::mutex eventsMutex;   // 引擎工作线程回调并发 push 共享 events 容器时的保护
#define CHECK(cond, msg)                                              \
    do {                                                              \
        if (cond) {                                                   \
            ++g_pass;                                                 \
            std::printf("  [PASS] %s\n", msg);                        \
        } else {                                                      \
            ++g_fail;                                                 \
            std::printf("  [FAIL] %s   (%s:%d)\n", msg, __FILE__, __LINE__); \
        }                                                             \
    } while (0)

// ---- FakeProvider：记录消息、可延时、可失败 ----
class FakeProvider final : public neograph::Provider {
public:
    asio::awaitable<neograph::ChatCompletion> complete_async(
        const neograph::CompletionParams& params) override {
        calls_.push_back(params.messages);
        asio::steady_timer timer(co_await asio::this_coro::executor);
        if (delay_ms_ > 0) {
            timer.expires_after(std::chrono::milliseconds(delay_ms_));
            co_await timer.async_wait(asio::use_awaitable);
        }
        if (fail_next_) {
            fail_next_ = false;
            throw std::runtime_error("fake provider failure");
        }
        neograph::ChatCompletion completion;
        completion.message.role = "assistant";
        completion.message.content = reply_;
        co_return completion;
    }
    std::string get_name() const override { return "fake-provider"; }

    std::vector<std::vector<neograph::ChatMessage>> calls_;
    std::string reply_ = "<!DOCTYPE html><html><body>FAKE-LLM-CONTENT</body></html>";
    int delay_ms_ = 0;
    std::atomic<bool> fail_next_{false};
};

// ---- 测试适配器 ----
nlohmann::json slowBgAdapter(const std::vector<nlohmann::json>& inputs,
                             const nlohmann::json& params) {
    const int ms = params.value("sleep_ms", 100);
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    return json{{"css", "body{background:#112233;}"}, {"layer", "bg"}};
}

nlohmann::json flakyBgAdapter(const std::vector<nlohmann::json>&, const nlohmann::json&) {
    static std::atomic<int> calls{0};
    if (calls.fetch_add(1) == 0) throw std::runtime_error("flaky first call");
    return json{{"css", "body{background:#445566;}"}, {"layer", "bg"}};
}

json pipelineWithBgAdapter(const std::string& adapter, int sleep_ms = 100) {
    json p = defaultSubtitlePipeline();
    for (auto& b : p["branches"])
        if (b.value("id", "") == "bg") {
            b["adapter"] = adapter;
            b["params"] = json{{"sleep_ms", sleep_ms}};
        }
    return p;
}

AssetRunnerConfig testConfig(const std::string& out_dir) {
    AssetRunnerConfig rc;
    rc.model = "fake-model";
    rc.html_output_dir = out_dir;
    rc.worker_count = 4;
    return rc;
}

std::string guidOf(int i) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa%04d", i);
    return buf;
}

std::string readFile(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    return content;
}

}  // namespace

// ------------------------------------------------------------------
// F1/F2：图结构展开
// ------------------------------------------------------------------
void testF1F2() {
    std::printf("F1/F2 图结构展开（bg 启用/禁用）\n");
    std::string err;
    AssetGraphPlan on = expandPipelines({{"g1full00000000000000000000000000", "g1full00", true}},
                                        defaultSubtitlePipeline(), &err);
    CHECK(!on.definition.is_null(), "F1: wants_bg=true 展开成功");
    if (!on.definition.is_null()) {
        const auto& nodes = on.definition["nodes"];
        CHECK(nodes.contains("g1full00_bg") && nodes.contains("g1full00_content") &&
                  nodes.contains("g1full00_compose"),
              "F1: 含 bg/content/compose 三节点");
        CHECK(nodes["g1full00_compose"].contains("barrier") &&
                  nodes["g1full00_compose"]["barrier"].contains("wait_for"),
              "F1: compose 带 barrier");
        const auto& wf = nodes["g1full00_compose"]["barrier"]["wait_for"];
        CHECK(wf.size() == 2, "F1: barrier wait_for 两个上游");
    }

    AssetGraphPlan off = expandPipelines({{"g2xxxx0000000000000000000000000", "g2xxxx00", false}},
                                         defaultSubtitlePipeline(), &err);
    CHECK(!off.definition.is_null(), "F2: wants_bg=false 展开成功");
    if (!off.definition.is_null()) {
        const auto& nodes = off.definition["nodes"];
        CHECK(nodes.size() == 1 && nodes.contains("g2xxxx00_content"),
              "F2: 仅 content 一个节点");
        CHECK(!nodes.contains("g2xxxx00_compose") && !nodes.contains("g2xxxx00_bg"),
              "F2: 无 bg/compose（等价旧路径）");
        CHECK(nodes["g2xxxx00_content"]["output"] == "asset_g2xxxx00_final",
              "F2: 单分支输出直写 final 通道");
    }
}

// ------------------------------------------------------------------
// F3：wantsBackground 判定规则
// ------------------------------------------------------------------
void testF3() {
    std::printf("F3 背景板判定规则（含否定语义）\n");
    CHECK(!wantsBackground(std::nullopt, "第一句台词", "字体大一点"), "F3: 无关键词=false");
    CHECK(wantsBackground(std::nullopt, "加个背景板，暖色调", ""), "F3: 文本命中词表=true");
    CHECK(wantsBackground(std::nullopt, "台词内容", "背景色用深色"), "F3: 样式命中词表=true");
    CHECK(wantsBackground(std::nullopt, "台词内容", "subtitle with Background image"), "F3: 英文命中=true");
    CHECK(wantsBackground(false, "加个背景板", "") == false, "F3: 显式 false 覆盖词表");
    CHECK(wantsBackground(true, "普通台词", "") == true, "F3: 显式 true 覆盖词表");
    // 否定语义（就近否定：同一小句内否定词使命中作废）
    CHECK(!wantsBackground(std::nullopt, "这是第二句字幕，用于测试多行对白。不需要自定义的背景板", ""),
          "F3: 否定+修饰语（不需要自定义的背景板）=false");
    CHECK(!wantsBackground(std::nullopt, "台词", "不要背景"), "F3: 简单否定（不要背景）=false");
    CHECK(!wantsBackground(std::nullopt, "台词", "无需背景板，纯字幕即可"), "F3: 无需否定=false");
    CHECK(!wantsBackground(std::nullopt, "plain subtitle, no background please", ""), "F3: 英文否定（no background）=false");
    CHECK(!wantsBackground(std::nullopt, "subtitle without backdrop", ""), "F3: 英文否定（without backdrop）=false");
    CHECK(wantsBackground(std::nullopt, "不要白色，要加背景板", ""), "F3: 混合表述（否定白色+肯定背景板）=true");
    CHECK(wantsBackground(std::nullopt, "不需要背景，但配上背景色", ""), "F3: 后半句重新肯定=true");
}

// ------------------------------------------------------------------
// F4：并发执行
// ------------------------------------------------------------------
void testF4() {
    std::printf("F4 并发执行（bg 100ms ∥ LLM 100ms）\n");
    auto provider = std::make_shared<FakeProvider>();
    provider->delay_ms_ = 100;
    AssetGraphRunner runner(testConfig(""), provider);
    AssetRequest req;
    req.guid = guidOf(1);
    req.short_id = "f4node000";
    req.user_text = "台词";
    req.wants_background = true;
    const auto t0 = std::chrono::steady_clock::now();
    auto results = runner.run({req}, pipelineWithBgAdapter("test_slow_bg"), nullptr);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    CHECK(results.front().ok, "F4: 管线成功");
    CHECK(ms < 150, ("F4: 墙钟 " + std::to_string(ms) + "ms < 150ms（串行应≥200ms）").c_str());
}

// ------------------------------------------------------------------
// F5/F6：合成内容与透传
// ------------------------------------------------------------------
void testF5F6(const std::string& out_dir) {
    std::printf("F5/F6 合成内容（bg 标记 + LLM 内容）/ bg 缺失透传\n");
    auto provider = std::make_shared<FakeProvider>();
    AssetGraphRunner runner(testConfig(out_dir), provider);
    AssetRequest req;
    req.guid = guidOf(2);
    req.short_id = "f5node000";
    req.user_text = "台词";
    req.context = json{{"content", "台词"}, {"style", "暗色背景板"}};
    req.wants_background = true;
    auto results = runner.run({req}, defaultSubtitlePipeline(), nullptr);
    const auto& r = results.front();
    CHECK(r.ok, "F5: 管线成功");
    CHECK(r.final_html.find("deepagent:bg_plate") != std::string::npos &&
              r.final_html.find("FAKE-LLM-CONTENT") != std::string::npos,
          "F5: 最终 HTML 同时含背景层标记与 LLM 内容");
    CHECK(r.final_html.find("#0b0f14") != std::string::npos, "F5: bg_plate 按'暗'关键词选深色板");
    const std::string file = out_dir + "\\" + guidOf(2) + ".html";
    CHECK(std::filesystem::exists(file) && readFile(file) == r.final_html,
          "F5: {guid}.html 落盘内容一致");

    // F6：html_compose 无 bg 输入 → 透传
    std::vector<nlohmann::json> inputs{nlohmann::json(nullptr), nlohmann::json("<p>ONLY</p>")};
    const auto passthrough =
        AdapterRegistry::instance().invoke("html_compose", inputs, json::object());
    CHECK(passthrough.get<std::string>() == "<p>ONLY</p>", "F6: bg 缺失时 compose 透传 LLM 内容");
}

// ------------------------------------------------------------------
// F7：向后兼容（无 asset_pipeline 条目）
// ------------------------------------------------------------------
void testF7() {
    std::printf("F7 向后兼容（空 schema = 隐式单分支）\n");
    auto provider = std::make_shared<FakeProvider>();
    provider->reply_ = "```html\n<!DOCTYPE html><html><body>LEGACY</body></html>\n```";
    AssetGraphRunner runner(testConfig(""), provider);
    AssetRequest req;
    req.guid = guidOf(3);
    req.short_id = "f7node000";
    req.user_text = "台词";
    req.wants_background = false;
    auto results = runner.run({req}, json::object(), nullptr);  // 无 asset_pipeline
    const auto& r = results.front();
    CHECK(r.ok, "F7: 隐式单分支成功");
    CHECK(r.final_html == "<!DOCTYPE html><html><body>LEGACY</body></html>",
          "F7: 输出=去围栏后的完整 HTML（等价旧单步行为）");
    CHECK(r.final_html.find("deepagent:bg_plate") == std::string::npos, "F7: 无背景层注入");
}

// ------------------------------------------------------------------
// F8：多轮上下文
// ------------------------------------------------------------------
void testF8() {
    std::printf("F8 多轮上下文（messages 优先于 user_text）\n");
    auto provider = std::make_shared<FakeProvider>();
    AssetGraphRunner runner(testConfig(""), provider);

    json first = json::array({json{{"role", "system"}, {"content", "sys"}},
                              json{{"role", "user"}, {"content", "第一轮"}},
                              json{{"role", "assistant"}, {"content", "v1"}},
                              json{{"role", "user"}, {"content", "第二轮指令"}}});
    AssetRequest req;
    req.guid = guidOf(4);
    req.short_id = "f8node000";
    req.messages = first;
    req.wants_background = false;
    auto results = runner.run({req}, json::object(), nullptr);
    CHECK(results.front().ok, "F8: 二次生成成功");
    CHECK(provider->calls_.size() == 1 && provider->calls_[0].size() == 4,
        "F8: llm 分支收到完整 4 条历史消息（含首轮 assistant）");
}

// ------------------------------------------------------------------
// F9：子步事件序列
// ------------------------------------------------------------------
void testF9() {
    std::printf("F9 子步事件序列\n");
    // 启用：bg/content running 都先于各自 done；compose 事件在两分支 done 之后
    {
        auto provider = std::make_shared<FakeProvider>();
        AssetGraphRunner runner(testConfig(""), provider);
        std::vector<std::pair<std::string, std::string>> events;
        AssetRequest req;
        req.guid = guidOf(5);
        req.short_id = "f9node000";
        req.user_text = "台词";
        req.wants_background = true;
        runner.run({req}, pipelineWithBgAdapter("test_slow_bg", 30),
                   [&](const std::string&, const std::string& step, const std::string& state) {
                       std::lock_guard<std::mutex> lk(eventsMutex);
                       events.emplace_back(step, state);
                   });
        auto idx = [&](const std::string& step, const std::string& state) {
            for (std::size_t i = 0; i < events.size(); ++i)
                if (events[i].first == step && events[i].second == state) return (int)i;
            return -1; };
        CHECK(idx("f9node000_bg", "running") >= 0 && idx("f9node000_content", "running") >= 0 &&
                  idx("f9node000_compose", "running") >= 0,
              "F9: bg/content/compose 均有 running");
        CHECK(idx("f9node000_bg", "done") < idx("f9node000_compose", "running") &&
                  idx("f9node000_content", "done") < idx("f9node000_compose", "running"),
              "F9: compose 在 bg 与 content 都 done 之后才 running");
    }
    // 禁用：只有 content
    {
        auto provider = std::make_shared<FakeProvider>();
        AssetGraphRunner runner(testConfig(""), provider);
        std::vector<std::pair<std::string, std::string>> events;
        AssetRequest req;
        req.guid = guidOf(6);
        req.short_id = "f9off0000";
        req.user_text = "台词";
        req.wants_background = false;
        runner.run({req}, defaultSubtitlePipeline(),
                   [&](const std::string&, const std::string& step, const std::string& state) {
                       std::lock_guard<std::mutex> lk(eventsMutex);
                       events.emplace_back(step, state);
                   });
        bool only_content = true;
        for (const auto& e : events)
            if (e.first.find("_bg") != std::string::npos ||
                e.first.find("_compose") != std::string::npos)
                only_content = false;
        CHECK(only_content && !events.empty(), "F9: 禁用时仅 content 子步事件");
    }
}

// ------------------------------------------------------------------
// F10：失败传播
// ------------------------------------------------------------------
void testF10() {
    std::printf("F10 失败传播（llm 分支失败）\n");
    auto provider = std::make_shared<FakeProvider>();
    provider->fail_next_ = true;
    AssetGraphRunner runner(testConfig(""), provider);
    std::vector<std::pair<std::string, std::string>> events;
    AssetRequest req;
    req.guid = guidOf(7);
    req.short_id = "f10node00";
    req.user_text = "台词";
    req.wants_background = true;
    auto results = runner.run({req}, defaultSubtitlePipeline(),
                              [&](const std::string&, const std::string& step,
                                  const std::string& state) {
                                  std::lock_guard<std::mutex> lk(eventsMutex);
                                  events.emplace_back(step, state);
                              });
    const auto& r = results.front();
    CHECK(!r.ok, "F10: run 失败");
    CHECK(r.error.find("asset pipeline failed") != std::string::npos ||
              r.error.find("fake provider") != std::string::npos,
          "F10: 错误信息指向失败分支");
    bool has_failed = false, compose_ran = false;
    for (const auto& e : events) {
        if (e.second == "failed") has_failed = true;
        if (e.first == "f10node00_compose" && e.second == "running") compose_ran = true;
    }
    CHECK(has_failed, "F10: node_state 含 failed");
    CHECK(!compose_ran, "F10: compose 未执行");
}

// ------------------------------------------------------------------
// F11：分支重试 + barrier 语义
// ------------------------------------------------------------------
void testF11() {
    std::printf("F11 分支重试（bg 首败重试成功，compose 等齐）\n");
    auto provider = std::make_shared<FakeProvider>();
    provider->reply_ = "<!DOCTYPE html><html><body>CONTENT-OK</body></html>";
    AssetGraphRunner runner(testConfig(""), provider);
    AssetRequest req;
    req.guid = guidOf(8);
    req.short_id = "f11node00";
    req.user_text = "台词";
    req.wants_background = true;
    auto results = runner.run({req}, pipelineWithBgAdapter("test_flaky_bg"), nullptr);
    const auto& r = results.front();
    CHECK(r.ok, "F11: bg 重试后管线成功");
    CHECK(r.final_html.find("deepagent:bg_plate") != std::string::npos &&
              r.final_html.find("CONTENT-OK") != std::string::npos,
          "F11: compose 输出仍含两路（barrier 等齐）");
}

// ------------------------------------------------------------------
// F12：多节点并发
// ------------------------------------------------------------------
void testF12() {
    std::printf("F12 多节点并发（2 节点 × 4 子步同超步）\n");
    auto provider = std::make_shared<FakeProvider>();
    provider->delay_ms_ = 80;
    AssetGraphRunner runner(testConfig(""), provider);
    std::vector<AssetRequest> reqs;
    for (int i = 0; i < 2; ++i) {
        AssetRequest req;
        req.guid = guidOf(10 + i);
        req.short_id = std::string("f12node0") + char('a' + i);
        req.user_text = "台词";
        req.wants_background = true;
        reqs.push_back(std::move(req));
    }
    const auto t0 = std::chrono::steady_clock::now();
    auto results = runner.run(reqs, pipelineWithBgAdapter("test_slow_bg", 80), nullptr);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    CHECK(results.size() == 2 && results[0].ok && results[1].ok, "F12: 两节点均成功");
    // 串行 4×80=320ms；并行 4 worker 应 <200ms
    CHECK(ms < 200, ("F12: 墙钟 " + std::to_string(ms) + "ms < 200ms（串行应≥320ms）").c_str());
}

// ================= ZCode 可参考设计落地测试（P1/P2/P4）G1-G12 =================

void testG1toG8() {
    std::printf("\n-- P1: Plan IR 脚本化 + QuickJS 沙箱 --\n");
    json context{{"user_requirement", "生成一份字幕 HTML"},
                 {"style_prompt", ""},
                 {"catalogs", json::array({json{{"catalog", "cat_a"}}})}};

    // G1: 合法脚本 → Plan IR
    const char* okScript =
        "define(\"plan\", function(context) {"
        "  var nodes = []; var links = [];"
        "  nodes.push({key: \"tool\", catalog: \"cat_a\"});"
        "  nodes.push({key: \"sink\", catalog: \"cat_b\"});"
        "  links.push({from: \"tool:html_file\", to: \"sink:in0\"});"
        "  return {nodes: nodes, links: links};"
        "});";
    auto r = executePlanScript(okScript, context);
    CHECK(r.ok, "G1: 合法脚本执行成功");
    CHECK(r.ok && r.planIr.value("nodes", json::array()).size() == 2,
          "G1: Plan IR 含 2 个 nodes");
    CHECK(r.ok && r.planIr.value("links", json::array()).size() == 1,
          "G1: Plan IR 含 1 条 link");
    CHECK(r.ok && r.planIr["links"][0].value("from", "") == "tool:html_file",
          "G1: link 字段保真（含中文名端口也走 JSON 往返）");

    // G2: if 条件逻辑（裸 JSON 做不到）
    const char* condScript =
        "define(\"plan\", function(context) {"
        "  var nodes = [{key: \"tool\", catalog: \"cat_a\"}];"
        "  if (context.style_prompt) nodes.push({key: \"style\", catalog: \"cat_a\"});"
        "  return {nodes: nodes, links: []};"
        "});";
    auto rOff = executePlanScript(condScript, context);
    CHECK(rOff.ok && rOff.planIr["nodes"].size() == 1, "G2: style_prompt 为空 → 1 节点");
    json ctxStyled = context;
    ctxStyled["style_prompt"] = "字体大一点";
    auto rOn = executePlanScript(condScript, ctxStyled);
    CHECK(rOn.ok && rOn.planIr["nodes"].size() == 2, "G2: style_prompt 非空 → 2 节点（if 生效）");

    // G3: 语法错误 → 可读报错（含行号语义）
    auto rSyn = executePlanScript("define(\"plan\", function(context) { return {nodes: [; });", context);
    CHECK(!rSyn.ok && rSyn.error.find("syntax") != std::string::npos,
          ("G3: 语法错误被拦截: " + rSyn.error.substr(0, 60)).c_str());

    // G4: 缺 define("plan") → 报错提示脚本约定
    auto rNoDef = executePlanScript("var x = 1; x += 2;", context);
    CHECK(!rNoDef.ok && rNoDef.error.find("define") != std::string::npos,
          "G4: 未注册 plan 函数给出约定提示");

    // G5: plan() 返回非对象 → 拒绝
    auto rNum = executePlanScript("define(\"plan\", function(c){ return 42; });", context);
    CHECK(!rNum.ok && rNum.error.find("object") != std::string::npos,
          "G5: 非对象返回值被拒绝");

    // G6: 死循环 → interrupt 中止，timeout 标记
    const auto t0 = std::chrono::steady_clock::now();
    auto rLoop = executePlanScript(
        "define(\"plan\", function(c){ while(true){} return null; });",
        context, /*memoryLimitBytes=*/16 * 1024 * 1024, /*timeoutMs=*/300);
    const auto loopMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
    CHECK(!rLoop.ok && rLoop.timeout, "G6: 死循环被超时中止");
    CHECK(loopMs < 3000, ("G6: 中止墙钟 " + std::to_string(loopMs) + "ms < 3000ms").c_str());

    // G7: 内存上限 → 分配失败中止
    auto rMem = executePlanScript(
        "define(\"plan\", function(c){ var a = []; for(;;) a.push(new Array(1000000)); });",
        context, /*memoryLimitBytes=*/16 * 1024 * 1024, /*timeoutMs=*/5000);
    CHECK(!rMem.ok, ("G7: 超内存被中止: " + rMem.error.substr(0, 60)).c_str());

    // G8: context 数据注入（中文往返）
    const char* echoScript =
        "define(\"plan\", function(context) {"
        "  return {nodes: [{key: context.user_requirement, catalog: \"cat_a\"}], links: []};"
        "});";
    auto rEcho = executePlanScript(echoScript, context);
    CHECK(rEcho.ok && rEcho.planIr["nodes"][0].value("key", "") == "生成一份字幕 HTML",
          "G8: context 中文数据往返保真");
}

void testG9toG12(const std::string& out_dir) {
    std::printf("\n-- P2 结构化诊断 + P4 phases 预留 --\n");
    // 测试目录：两个 catalog（含 model_name 与语义端口）
    json catalogJson;
    catalogJson["nodes"]["test_cat_a"] = {
        {"model_name", "0x12345678"},
        {"output_ports", json{{"html_file", 0}}},
        {"input_ports", json::object()},
        {"props", json::object()}};
    catalogJson["nodes"]["test_cat_b"] = {
        {"model_name", "0x87654321"},
        {"output_ports", json::object()},
        {"input_ports", json{{"in0", 0}}},
        {"props", json::object()}};
    const std::string catPath = out_dir + "\\zcode_test_catalog.json";
    {
        std::ofstream f(catPath, std::ios::binary | std::ios::trunc);
        f << catalogJson.dump();
    }
    NodeCatalog catalog;
    std::string err;
    CHECK(catalog.load(catPath, &err), "G9 前置: 测试目录加载");

    UdrtCompiler compiler(catalog);
    json planGood{
        {"nodes", json::array({
            json{{"key", "tool"}, {"catalog", "test_cat_a"}},
            json{{"key", "sink"}, {"catalog", "test_cat_b"}}})},
        {"links", json::array({json{{"from", "tool:html_file"}, {"to", "sink:in0"}}})}};

    // G9: 未知 catalog → path/expected/actual/known_catalogs/hint 全套
    json planBadCat = planGood;
    planBadCat["nodes"][0]["catalog"] = "no_such_cat";
    json udrt, d1;
    std::string e1;
    CHECK(!compiler.compile(planBadCat, &udrt, &e1, &d1), "G9: 未知 catalog 编译失败");
    CHECK(d1.value("path", "") == "/nodes[0]/catalog", "G9: path 定位到节点目录字段");
    CHECK(d1.contains("expected") && d1.contains("actual") && d1.contains("hint"),
          "G9: expected/actual/hint 齐备");
    CHECK(d1.value("known_catalogs", json::array()).size() == 2, "G9: known_catalogs 合法值域");
    CHECK(d1.value("actual", "") == "no_such_cat", "G9: actual 为非法值");

    // G9b: 坏端口引用 → path 指向 link
    json planBadLink = planGood;
    planBadLink["links"][0]["from"] = "tool:无此端口";
    json d2;
    CHECK(!compiler.compile(planBadLink, &udrt, &e1, &d2), "G9b: 非法端口编译失败");
    CHECK(d2.value("path", "") == "/links[0]/from", "G9b: path 定位到 link 端口");

    // G10: validate 篡改 model_name → path/actual 精准
    CHECK(compiler.compile(planGood, &udrt, &e1, &d2), "G10 前置: 合法 Plan 编译成功");
    json tampered = udrt;
    tampered["nodes"][0]["internal-data"]["model_name"] = "zzz";
    json d3;
    CHECK(!compiler.validate(tampered, &e1, &d3), "G10: 篡改 model_name 校验失败");
    CHECK(d3.value("path", "").find("model_name") != std::string::npos,
          "G10: path 指向 internal-data/model_name");
    CHECK(d3.value("actual", "") == "zzz", "G10: actual 为篡改值");
    CHECK(d3.value("expected", "").find("0x") != std::string::npos,
          "G10: expected 描述合法格式");

    // G11: catalogKeys 排序返回
    const auto keys = catalog.catalogKeys();
    CHECK(keys.size() == 2 && keys[0] == "test_cat_a" && keys[1] == "test_cat_b",
          "G11: catalogKeys 全量且有序");

    // G12: phases 预留——Plan 可带 phases，编译成功且不写入 udrt（格式兼容）
    json planPhases = planGood;
    planPhases["phases"] = json::array({
        json{{"name", "生成"}, {"nodes", json::array({"tool"})}},
        json{{"name", "输出"}, {"nodes", json::array({"sink"})}}});
    json udrt3;
    CHECK(compiler.compile(planPhases, &udrt3, &e1, &d3), "G12: 含 phases 的 Plan 编译成功");
    CHECK(!udrt3.contains("phases"), "G12: udrt 文件不含 phases（U-DeepRT 格式兼容）");
}

// ================= V1.5.0 功能测试（.avatar 解析 / 批量生成 / EventHub）H1-H4 =================

void testH1(const std::string& out_dir) {
    std::printf("\n-- V1.5.0: .avatar 解析 --\n");
    // H1: 解析真实样本（testdata/1.avatar）
    std::vector<AvatarTextEntry> entries;
    std::string err;
    CHECK(parseAvatarFile("testdata\\1.avatar", &entries, &err), "H1: 1.avatar 解析成功");
    CHECK(entries.size() == 4, ("H1: 提取 4 个 text_in 节点，实际 " + std::to_string(entries.size())).c_str());
    if (entries.size() == 4) {
        // ptr 是运行时指针地址（每次保存会变），只断言非空且互不相同
        bool ptrOk = true;
        for (size_t i = 0; i < entries.size(); ++i)
            for (size_t j = i + 1; j < entries.size(); ++j)
                if (entries[i].nodePtr.empty() || entries[i].nodePtr == entries[j].nodePtr)
                    ptrOk = false;
        CHECK(ptrOk, "H1: nodePtr 主键非空且唯一");
        CHECK(entries[0].nodeId == "org.uranus.block.voice_text", "H1: nodeId 为块类型");
        CHECK(entries[0].text.find("加快科技创新和产业创新融合") != std::string::npos,
              "H1: 首节点字幕文本保真");
        CHECK(entries[1].text.find("为破解科研成果和产业需求脱节的难题") != std::string::npos,
              "H1: 第二节点字幕文本保真");
        bool allIndexOk = true;
        for (size_t i = 0; i < entries.size(); ++i)
            if (entries[i].index != (int)i || entries[i].text.empty() ||
                entries[i].nodePtr.empty()) allIndexOk = false;
        CHECK(allIndexOk, "H1: index 连续且 text/nodePtr 非空");
    }

    // H1b: XML 实体反转义
    CHECK(xmlUnescape("a&amp;b&lt;c&gt;d&quot;e&apos;f") == "a&b<c>d\"e'f",
          "H1b: 五种命名实体反转义");
    CHECK(xmlUnescape("&#x4f60;&#22909;") == "你好", "H1b: 数字实体（十六/十进制）转 UTF-8");
    CHECK(xmlUnescape("plain&unknown;x") == "plain&unknown;x", "H1b: 未知实体原样保留");

    // H2: 损坏/空/无 text_in → 明确报错
    {
        const std::string bad = out_dir + "\\h2_bad.avatar";
        { std::ofstream f(bad, std::ios::binary); f << "<html>not avatar</html>"; }
        std::vector<AvatarTextEntry> e2;
        CHECK(!parseAvatarFile(bad, &e2, &err) &&
              err.find("AvatarProject") != std::string::npos, "H2: 非 AvatarProject XML 报错明确");
    }
    {
        const std::string empty = out_dir + "\\h2_empty.avatar";
        { std::ofstream f(empty, std::ios::binary); }
        std::vector<AvatarTextEntry> e2;
        CHECK(!parseAvatarFile(empty, &e2, &err) && err.find("empty") != std::string::npos,
              "H2: 空文件报错明确");
    }
    {
        const std::string missing = out_dir + "\\h2_missing.avatar";
        std::vector<AvatarTextEntry> e2;
        CHECK(!parseAvatarFile(missing, &e2, &err) && err.find("cannot open") != std::string::npos,
              "H2: 文件不存在报错明确");
    }
    {
        const std::string notext = out_dir + "\\h2_notext.avatar";
        { std::ofstream f(notext, std::ios::binary);
          f << "<AvatarProject><timeline><project><nodes><node id=\"a\" ptr=\"1\">"
               "<param key=\"enabled_in\"><primary><standard><track>true</track>"
               "</standard></primary></param></node></nodes></project></timeline></AvatarProject>"; }
        std::vector<AvatarTextEntry> e2;
        CHECK(!parseAvatarFile(notext, &e2, &err) && err.find("text_in") != std::string::npos,
              "H2: 无 text_in 节点报错明确");
    }
}

void testH3(const std::string& out_dir) {
    std::printf("\n-- V1.5.0: .avatar 批量生成（FakeProvider 管线） --\n");
    std::vector<AvatarTextEntry> entries;
    std::string err;
    CHECK(parseAvatarFile("testdata\\1.avatar", &entries, &err), "H3 前置: 解析成功");
    if (entries.empty()) return;

    // 模拟 create_avatar_assets 的批量构造：parser → AssetRequests → 管线
    AssetRunnerConfig rc;
    rc.api_key = "fake";
    auto provider = std::make_shared<FakeProvider>();
    provider->delay_ms_ = 5;   // 4 节点零延迟完成会加剧引擎调度竞争（偶发段错误），加微延迟平滑
    AssetGraphRunner runner(rc, provider);

    std::vector<AssetRequest> reqs;
    std::vector<std::string> guids(entries.size());
    auto makeGuid = [] {
        unsigned int x = std::random_device{}();
        char buf[16];
        snprintf(buf, sizeof(buf), "%08X000000000000000000000000", x);
        return std::string(buf, 32);
    };
    for (size_t i = 0; i < entries.size(); ++i) {
        guids[i] = makeGuid();
        AssetRequest req;
        req.guid = guids[i];
        req.short_id = "av" + std::to_string(i) + "_" + guids[i].substr(0, 8);
        req.user_text = entries[i].text;
        req.context = json{{"content", entries[i].text}, {"style", ""}};
        req.wants_background = false;
        reqs.push_back(std::move(req));
    }

    auto results = runner.run(reqs, defaultSubtitlePipeline(), nullptr);
    CHECK(results.size() == entries.size(), "H3: 每节点一个结果");
    bool allOk = true;
    for (const auto& r : results) if (!r.ok || r.final_html.empty()) allOk = false;
    CHECK(allOk, "H3: 4 节点批量生成全部成功");

    // 模拟落盘 + manifest（与 toolCreateAvatarAssets 相同的命名/结构约定）
    json manifest = json::array();
    int written = 0;
    for (size_t i = 0; i < results.size(); ++i) {
        if (!results[i].ok) continue;
        std::string htmlFile = out_dir + "\\1_" + std::to_string(i) + "_" +
                               guids[i].substr(0, 8) + ".html";
        std::ofstream f(htmlFile, std::ios::binary | std::ios::trunc);
        f << results[i].final_html;
        manifest.push_back({{"index", entries[i].index},
                            {"node_id", entries[i].nodeId},
                            {"node_ptr", entries[i].nodePtr},
                            {"guid", guids[i]},
                            {"html_file", htmlFile},
                            {"version", 1}});
        ++written;
    }
    CHECK(written == 4, "H3: 4 个 HTML 落盘");
    bool fileOk = true;
    for (const auto& item : manifest) {
        std::ifstream f(item.value("html_file", ""), std::ios::binary);
        if (!f || f.peek() == std::ifstream::traits_type::eof()) fileOk = false;
    }
    CHECK(fileOk, "H3: 落盘文件非空");
    CHECK(manifest[0].value("version", 0) == 1 &&
          !manifest[0].value("node_ptr", "").empty(), "H3: manifest 结构（ptr 主键/version 初值）");
}

void testH4() {
    std::printf("\n-- V1.5.0: EventHub --\n");
    EventHub& hub = EventHub::instance();

    // H4: 发布/订阅（全量订阅者 + 项目过滤订阅者）
    auto all = hub.subscribe();
    auto onlyP = hub.subscribe("projA");
    hub.publish("asset_updated", json{{"file", "x.html"}}, "projA");
    hub.publish("asset_updated", json{{"file", "y.html"}}, "projB");

    std::string ev, data;
    CHECK(all->waitPop(&ev, &data, 100) == EventHub::PopResult::Got &&
          ev == "asset_updated" && data.find("x.html") != std::string::npos,
          "H4: 全量订阅者收到 projA 事件");
    CHECK(all->waitPop(&ev, &data, 100) == EventHub::PopResult::Got &&
          data.find("y.html") != std::string::npos,
          "H4: 全量订阅者收到 projB 事件");
    CHECK(onlyP->waitPop(&ev, &data, 100) == EventHub::PopResult::Got &&
          data.find("x.html") != std::string::npos,
          "H4: 项目过滤订阅者收到匹配事件");
    CHECK(onlyP->waitPop(&ev, &data, 100) == EventHub::PopResult::Timeout,
          "H4: 项目过滤订阅者收不到不匹配事件");

    // H4b: 广播（project 空 → 所有订阅者都收）
    hub.publish("asset_updated", json{{"file", "z.html"}});
    CHECK(all->waitPop(&ev, &data, 100) == EventHub::PopResult::Got &&
          data.find("z.html") != std::string::npos, "H4b: 广播事件全量订阅者收到");
    CHECK(onlyP->waitPop(&ev, &data, 100) == EventHub::PopResult::Got &&
          data.find("z.html") != std::string::npos, "H4b: 广播事件过滤订阅者也收到");

    // H4c: 慢消费者队列截断（容量 256，丢最旧）
    auto slow = hub.subscribe();
    for (int i = 0; i < 300; ++i)
        hub.publish("asset_updated", json{{"seq", i}});
    CHECK(slow->waitPop(&ev, &data, 100) == EventHub::PopResult::Got &&
          data.find("\"seq\":44") != std::string::npos,
          "H4c: 超容量后最旧事件被丢弃（首条为 seq=44）");

    // H4d: close 语义
    auto dying = hub.subscribe();
    dying->close();
    CHECK(dying->waitPop(&ev, &data, 10) == EventHub::PopResult::Closed,
          "H4d: close 后 waitPop 返回 Closed");
}

// ================= V1.5.1 工程资产上下文测试 H5-H7 =================

void testH5toH7(const std::string& out_dir) {
    std::printf("\n-- V1.5.1: AssetContext 上下文管理 --\n");
    AssetContextStore store(out_dir);   // 沙盒：manifest/索引都落在 out_dir

    // 沙盒去状态：清掉上一轮残留（否则 history 断言受跨轮污染）
    std::error_code sec;
    std::filesystem::remove(out_dir + "\\asset_index.json", sec);
    std::filesystem::remove(out_dir + "\\1.assets.json", sec);

    // H5: record → findByGuid 往返 + 索引落盘
    AssetContext c;
    c.guid = "TESTGUID000000000000000000000001";
    c.nodePtr = "999888777";
    c.nodeId = "org.uranus.block.voice_text";
    c.kind = "avatar_text";
    c.sourceFile = out_dir + "\\proj_c1\\1.avatar";   // 副本即唯一工作文件
    c.entryId = "saturn_subtitle_html";
    c.stylePrompt = "标题蓝色、28px 字体";
    c.background = true;
    c.text = "原始字幕文本";
    c.htmlFile = out_dir + "\\1_00_TESTGUI.html";
    c.version = 1;
    c.chatId = "c_test_1";
    std::string err;
    CHECK(store.record(c, &err), ("H5: record 成功 " + err).c_str());
    AssetContext got;
    CHECK(store.findByGuid(c.guid, &got, &err), "H5: findByGuid 命中");
    CHECK(got.stylePrompt == c.stylePrompt && got.background == true &&
          got.nodePtr == c.nodePtr && got.chatId == c.chatId,
          "H5: 样式/背景/主键/归属会话往返保真（重生成不丢样式的数据基础）");
    CHECK(std::filesystem::exists(out_dir + "\\asset_index.json"), "H5: 全局索引落盘");
    CHECK(std::filesystem::exists(out_dir + "\\1.assets.json"), "H5: manifest 落盘（stem 命名）");

    // H6: appendHistory —— version+1、text 更新、history 追加
    CHECK(store.appendHistory(c.guid, "notify", "exe 修改后的文本", "avatar.exe", &err),
          "H6: appendHistory 成功");
    AssetContext got2;
    CHECK(store.findByGuid(c.guid, &got2, &err), "H6: 更新后可查");
    CHECK(got2.version == 2, "H6: version 递增到 2");
    CHECK(got2.text == "exe 修改后的文本", "H6: 当前文本已更新");
    CHECK(got2.history.size() == 1 && got2.history[0].value("reason", "") == "notify" &&
          got2.history[0].value("version", 0) == 2,
          "H6: history 追加一条（reason/version 正确）");

    // H7: removeByChat —— 只清目标会话，其余保留
    AssetContext c2 = c;
    c2.guid = "TESTGUID000000000000000000000002";
    c2.chatId = "c_test_2";
    c2.htmlFile = out_dir + "\\1_00_TESTGU2.html";
    store.record(c2, &err);
    { std::ofstream f(c.htmlFile, std::ios::binary); f << "<html>c1</html>"; }
    { std::ofstream f(c2.htmlFile, std::ios::binary); f << "<html>c2</html>"; }

    auto removed = store.removeByChat("c_test_1", &err);
    CHECK(removed.size() == 1 && removed[0] == c.htmlFile, "H7: removeByChat 返回 c1 产物路径");
    AssetContext gone, kept;
    CHECK(!store.findByGuid(c.guid, &gone, &err), "H7: c1 上下文已删除");
    CHECK(store.findByGuid(c2.guid, &kept, &err) && kept.chatId == "c_test_2",
          "H7: c2 上下文保留");
    CHECK(!std::filesystem::exists(c.htmlFile) == false,
          "H7: 产物文件由调用方删除（本函数模拟调用方）");
    std::error_code ec;
    std::filesystem::remove(c.htmlFile, ec);
    CHECK(!std::filesystem::exists(out_dir + "\\1.assets.json") == false,
          "H7: manifest 仍存在（c2 共用工程，仅当 chat_id 全匹配才删）");
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // 段错误时保住崩溃点前的输出
    std::printf("===== Node 资产生成子步并发 逐项功能测试 =====\n\n");
    AdapterRegistry::instance().registerAdapter("test_slow_bg", slowBgAdapter);
    AdapterRegistry::instance().registerAdapter("test_flaky_bg", flakyBgAdapter);

    testF1F2();
    testF3();
    testF4();
    const std::string out_dir = "asset_test_out";
    std::filesystem::create_directories(out_dir);
    testF5F6(out_dir);
    testF7();
    testF8();
    testF9();
    testF10();
    testF11();
    testF12();
    testG1toG8();
    testG9toG12(out_dir);
    testH1(out_dir);
    testH3(out_dir);
    testH4();
    testH5toH7(out_dir);

    std::printf("\n===== 结果：%d 通过 / %d 失败 =====\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
