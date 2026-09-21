// Node 资产生成子步并发 —— 逐项功能测试 F1-F12（独立 exe，FakeProvider）。
// 设计文档：docs/Node资产生成子步并发设计.md §4 测试矩阵。
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
#include "asset_pipeline.h"
#include "asset_runner.h"

#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

using json = nlohmann::json;
using namespace deepagent;

namespace {

int g_pass = 0, g_fail = 0;
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
                                  const std::string& state) { events.emplace_back(step, state); });
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

int main() {
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

    std::printf("\n===== 结果：%d 通过 / %d 失败 =====\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
