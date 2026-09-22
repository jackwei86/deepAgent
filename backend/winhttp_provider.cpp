#include "winhttp_provider.h"

#include <stdexcept>

#include "third_party/json.hpp"

using nlohmann::json;

namespace deepagent {
namespace {

// neograph::json(yyjson 封装) → nlohmann::json 的桥接：dump/parse 往返
json toNlohmann(const neograph::json& v) {
    try {
        return json::parse(v.dump());
    } catch (const std::exception&) {
        return json::object();
    }
}

} // namespace

json WinHttpProvider::buildBody(const neograph::CompletionParams& params, bool stream) const {
    json arr = json::array();
    for (const auto& m : params.messages) {
        json jm{{"role", m.role}, {"content", m.content}};
        if (m.role == "assistant" && !m.tool_calls.empty()) {
            json tcs = json::array();
            for (const auto& tc : m.tool_calls) {
                tcs.push_back({{"id", tc.id},
                               {"type", "function"},
                               {"function", {{"name", tc.name}, {"arguments", tc.arguments}}}});
            }
            jm["tool_calls"] = tcs;
            if (m.content.empty()) jm["content"] = nullptr;  // 协议要求：带 tool_calls 时 content 可空
        }
        if (m.role == "tool") {
            if (!m.tool_call_id.empty()) jm["tool_call_id"] = m.tool_call_id;
            if (jm["content"].is_string() && jm["content"].get<std::string>().empty())
                jm["content"] = "{}";  // DeepSeek 拒绝空 content 的 tool 消息，给最小 JSON 兜底
        }
        arr.push_back(std::move(jm));
    }

    json body{{"model", params.model.empty() ? config_.llm_model : params.model},
              {"messages", arr},
              {"stream", stream},
              {"max_tokens", config_.llm_max_tokens},
              {"temperature", params.temperature > 0 ? params.temperature
                                                     : config_.llm_temperature}};
    if (!params.tools.empty()) {
        json tools = json::array();
        for (const auto& t : params.tools) {
            tools.push_back({{"type", "function"},
                             {"function",
                              {{"name", t.name},
                               {"description", t.description},
                               {"parameters", toNlohmann(t.parameters)}}}});
        }
        body["tools"] = tools;
        body["tool_choice"] = "auto";
    }
    return body;
}

neograph::ChatCompletion WinHttpProvider::complete(const neograph::CompletionParams& params) {
    std::string err;
    json resp = llm_.postChatJson(buildBody(params, /*stream=*/false), &err);
    if (resp.is_null() || !resp.is_object())
        throw std::runtime_error(err.empty() ? "LLM request failed" : err);
    if (!resp.contains("choices") || !resp["choices"].is_array() || resp["choices"].empty())
        throw std::runtime_error("LLM response missing choices: " + resp.dump().substr(0, 200));
    const json& choice = resp["choices"][0];
    if (!choice.contains("message") || !choice["message"].is_object())
        throw std::runtime_error("LLM response missing message");

    const json& message = choice["message"];
    neograph::ChatCompletion completion;
    completion.message.role = message.value("role", "assistant");
    if (message.contains("content") && message["content"].is_string())
        completion.message.content = message["content"].get<std::string>();
    if (message.contains("tool_calls") && message["tool_calls"].is_array()) {
        for (const auto& tc : message["tool_calls"]) {
            if (!tc.is_object()) continue;
            neograph::ToolCall call;
            call.id = tc.value("id", "");
            const json fn = tc.contains("function") ? tc["function"] : json::object();
            call.name = fn.value("name", "");
            call.arguments = fn.value("arguments", "");
            completion.message.tool_calls.push_back(std::move(call));
        }
    }

    const std::string fr = choice.value("finish_reason", "");
    if (fr == "tool_calls" || fr == "function_call")
        completion.stop_reason = "tool_use";
    else if (fr == "length")
        completion.stop_reason = "max_tokens";
    else if (fr == "content_filter")
        completion.stop_reason = "content_filter";
    else if (fr == "stop")
        completion.stop_reason = "end_turn";
    else
        completion.stop_reason =
            completion.message.tool_calls.empty() ? "end_turn" : "tool_use";
    return completion;
}

neograph::ChatCompletion WinHttpProvider::complete_stream(
    const neograph::CompletionParams& params, const neograph::StreamCallback& on_chunk) {
    std::string err;
    json streamedCalls = json::array();
    std::string finish;
    std::string content =
        llm_.postChatStream(buildBody(params, /*stream=*/true), on_chunk, &err,
                            &streamedCalls, &finish);
    if (content.empty() && streamedCalls.empty() && !err.empty())
        throw std::runtime_error(err);

    neograph::ChatCompletion completion;
    completion.message.role = "assistant";
    completion.message.content = content;

    // 模型可能在流式轮直接发起工具调用（finish_reason=tool_calls）：
    // 必须回传 tool_calls 让 Agent 继续循环，否则会被误判为空回复。
    if (streamedCalls.is_array()) {
        for (const auto& tc : streamedCalls) {
            if (!tc.is_object()) continue;
            neograph::ToolCall call;
            call.id = tc.value("id", "");
            const json fn = tc.contains("function") ? json(tc["function"]) : json::object();
            call.name = fn.value("name", "");
            call.arguments = fn.value("arguments", "");
            completion.message.tool_calls.push_back(std::move(call));
        }
    }
    if (!completion.message.tool_calls.empty() || finish == "tool_calls")
        completion.stop_reason = "tool_use";
    else
        completion.stop_reason = "end_turn";
    return completion;
}

} // namespace deepagent
