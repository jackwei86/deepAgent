#include "llm_client.h"

#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <vector>

#include <windows.h>
#include <winhttp.h>

#include "third_party/json.hpp"

#pragma comment(lib, "winhttp.lib")

using json = nlohmann::json;

namespace deepagent {
namespace {

std::wstring toWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
    std::wstring w(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), &w[0], size);
    return w;
}

// Split "https://api.deepseek.com" into (https, host, port).
bool splitBaseUrl(const std::string& base, bool& https, std::wstring& host, int& port) {
    std::string s = base;
    std::string scheme = "https";
    auto pos = s.find("://");
    if (pos != std::string::npos) {
        scheme = s.substr(0, pos);
        s = s.substr(pos + 3);
    }
    https = (scheme != "http");
    port = https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    auto slash = s.find('/');
    if (slash != std::string::npos) s = s.substr(0, slash);
    auto colon = s.rfind(':');
    if (colon != std::string::npos) {
        port = std::stoi(s.substr(colon + 1));
        s = s.substr(0, colon);
    }
    if (s.empty()) return false;
    host = toWide(s);
    return true;
}

// Incremental SSE line parser: feeds raw bytes, emits complete data: payloads.
class SseParser {
public:
    void feed(const char* data, size_t len, std::vector<std::string>& out) {
        buffer_.append(data, len);
        size_t pos = 0;
        while (true) {
            size_t nl = buffer_.find('\n', pos);
            if (nl == std::string::npos) break;
            std::string line = buffer_.substr(pos, nl - pos);
            pos = nl + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("data:", 0) == 0) {
                std::string payload = line.substr(5);
                if (!payload.empty() && payload.front() == ' ') payload.erase(0, 1);
                if (!payload.empty()) out.push_back(std::move(payload));
            }
            // non-data lines (event:, id:, comments) are irrelevant for OpenAI SSE
        }
        buffer_.erase(0, pos);
    }

    std::string remainder(std::vector<std::string>& out) {
        // flush a trailing line without newline
        std::string line = buffer_;
        buffer_.clear();
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("data:", 0) == 0) {
            std::string payload = line.substr(5);
            if (!payload.empty() && payload.front() == ' ') payload.erase(0, 1);
            if (!payload.empty()) out.push_back(std::move(payload));
        }
        return line;
    }

private:
    std::string buffer_;
};

json buildBody(const Config& cfg, const std::vector<ChatMessage>& messages, bool stream,
               double temperature) {
    json arr = json::array();
    for (const auto& m : messages) {
        arr.push_back({{"role", m.role}, {"content", m.content}});
    }
    json body;
    body["model"] = cfg.llm_model;
    body["messages"] = arr;
    body["stream"] = stream;
    body["max_tokens"] = cfg.llm_max_tokens;
    body["temperature"] = temperature >= 0.0 ? temperature : cfg.llm_temperature;
    return body;
}

// Extract incremental content from one SSE payload, empty if none.
std::string deltaContent(const json& j) {
    if (!j.is_object()) return {};
    auto choices = j.find("choices");
    if (choices == j.end() || !choices->is_array() || choices->empty()) return {};
    const auto& c0 = (*choices)[0];
    auto delta = c0.find("delta");
    if (delta == c0.end()) return {};
    auto content = delta->find("content");
    if (content == delta->end() || !content->is_string()) return {};
    return content->get<std::string>();
}

// 流式 tool_calls 累积器：按 index 拼接 id/name/arguments 分片
struct StreamedToolCalls {
    struct Entry {
        std::string id, name, arguments;
    };
    std::map<int, Entry> byIndex;

    void feed(const json& payload) {
        if (!payload.is_object()) return;
        auto choices = payload.find("choices");
        if (choices == payload.end() || !choices->is_array() || choices->empty()) return;
        const json& c0 = (*choices)[0];
        auto delta = c0.find("delta");
        if (delta == c0.end() || !delta->is_object()) return;
        auto tcs = delta->find("tool_calls");
        if (tcs == delta->end() || !tcs->is_array()) return;
        for (const auto& tc : *tcs) {
            if (!tc.is_object()) continue;
            int index = 0;
            if (tc.contains("index") && tc["index"].is_number_integer())
                index = tc["index"].get<int>();
            Entry& e = byIndex[index];
            if (tc.contains("id") && tc["id"].is_string() && !tc["id"].get<std::string>().empty())
                e.id = tc["id"].get<std::string>();
            if (tc.contains("function") && tc["function"].is_object()) {
                const json& fn = tc["function"];
                if (fn.contains("name") && fn["name"].is_string() &&
                    !fn["name"].get<std::string>().empty())
                    e.name = fn["name"].get<std::string>();
                if (fn.contains("arguments") && fn["arguments"].is_string())
                    e.arguments += fn["arguments"].get<std::string>();
            }
        }
    }

    // OpenAI 非流式协议形态（供回传给请求方）
    json toJsonArray() const {
        json arr = json::array();
        for (const auto& [idx, e] : byIndex) {
            arr.push_back({{"id", e.id},
                           {"type", "function"},
                           {"function", {{"name", e.name}, {"arguments", e.arguments}}}});
        }
        return arr;
    }
};

// choices[0].finish_reason（可能为 null / 缺失）
std::string streamFinishReason(const json& payload) {
    if (!payload.is_object()) return {};
    auto choices = payload.find("choices");
    if (choices == payload.end() || !choices->is_array() || choices->empty()) return {};
    const json& c0 = (*choices)[0];
    auto fr = c0.find("finish_reason");
    if (fr == c0.end() || !fr->is_string()) return {};
    return fr->get<std::string>();
}

} // namespace
LlmClient::LlmClient(const Config& config) : config_(config) {}

std::string LlmClient::invoke(const std::vector<ChatMessage>& messages, std::string* error,
                              double temperature) {
    return request(messages, /*stream=*/false, nullptr, error, temperature);
}

std::string LlmClient::invokeStream(const std::vector<ChatMessage>& messages,
                                    const std::function<void(const std::string&)>& onToken,
                                    std::string* error) {
    std::string acc;
    auto sink = [&](const std::string& delta) {
        acc += delta;
        if (onToken) onToken(delta);
    };
    std::string result = request(messages, /*stream=*/true, sink, error, -1.0);
    return result.empty() ? acc : result;
}

std::string LlmClient::request(const std::vector<ChatMessage>& messages, bool stream,
                               const std::function<void(const std::string&)>& onToken,
                               std::string* error, double temperature) {
    if (!config_.llmConfigured()) {
        if (error) *error = "DeepSeek API key not configured (config/.env DEEPSEEK_API_KEY)";
        return {};
    }
    json body = buildBody(config_, messages, stream, temperature);
    std::string rawBody;
    if (!sendChat(body, stream, onToken, &rawBody, error)) return {};

    std::string full;
    if (!stream) {
        try {
            json j = json::parse(rawBody);
            auto choices = j.find("choices");
            if (choices != j.end() && choices->is_array() && !choices->empty()) {
                auto message = (*choices)[0].find("message");
                if (message != (*choices)[0].end()) {
                    auto content = message->find("content");
                    if (content != message->end() && content->is_string()) {
                        full = content->get<std::string>();
                    }
                }
            }
        } catch (const std::exception&) {
            if (error) *error = "LLM returned non-JSON body";
        }
    } else {
        full = rawBody;  // 流式：rawBody 承载累积 content
    }
    if (full.empty() && error) error->clear();
    return full;
}

nlohmann::json LlmClient::postChatJson(const nlohmann::json& body, std::string* error) {
    if (!config_.llmConfigured()) {
        if (error) *error = "DeepSeek API key not configured (config/.env DEEPSEEK_API_KEY)";
        return nullptr;
    }
    std::string rawBody;
    if (!sendChat(body, /*stream=*/false, nullptr, &rawBody, error)) return nullptr;
    try {
        return json::parse(rawBody);
    } catch (const std::exception&) {
        if (error) *error = "LLM returned non-JSON body";
        return nullptr;
    }
}

std::string LlmClient::postChatStream(const nlohmann::json& body,
                                      const std::function<void(const std::string&)>& onDelta,
                                      std::string* error,
                                      nlohmann::json* toolCallsOut,
                                      std::string* finishReasonOut) {
    if (!config_.llmConfigured()) {
        if (error) *error = "DeepSeek API key not configured (config/.env DEEPSEEK_API_KEY)";
        return {};
    }
    std::string accumulated;
    auto sink = [&](const std::string& delta) {
        accumulated += delta;
        if (onDelta) onDelta(delta);
    };
    std::string rawBody;
    if (!sendChat(body, /*stream=*/true, sink, &rawBody, error, toolCallsOut, finishReasonOut))
        return {};
    // rawBody 承载累积 content（与 request 流式路径一致）
    return rawBody.empty() ? accumulated : rawBody;
}

bool LlmClient::sendChat(const nlohmann::json& body, bool stream,
                         const std::function<void(const std::string&)>& onDelta,
                         std::string* rawBody, std::string* error,
                         nlohmann::json* toolCallsOut, std::string* finishReasonOut) {
    bool https = false;
    std::wstring host;
    int port = 0;
    if (!splitBaseUrl(config_.llm_base_url, https, host, port)) {
        if (error) *error = "invalid DEEPSEEK_BASE_URL";
        return false;
    }

    std::string wire = body.dump();

    std::wstring wpath = toWide(config_.llm_path);
    std::wstring headers = toWide(
        "Content-Type: application/json\r\n"
        "Accept: " + std::string(stream ? "text/event-stream" : "application/json") + "\r\n"
        "Authorization: Bearer " + config_.llm_api_key + "\r\n");

    HINTERNET session = nullptr, connectH = nullptr, requestH = nullptr;
    bool ok = false;
    auto fail = [&](const std::string& msg) {
        if (error && error->empty()) *error = msg;
        if (requestH) WinHttpCloseHandle(requestH);
        if (connectH) WinHttpCloseHandle(connectH);
        if (session) WinHttpCloseHandle(session);
        return false;
    };

    session = WinHttpOpen(L"DeepAgent/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return fail("WinHttpOpen failed");
    WinHttpSetTimeouts(session, 15000, 30000, 30000, 300000);

    connectH = WinHttpConnect(session, host.c_str(), (INTERNET_PORT)port, 0);
    if (!connectH) return fail("WinHttpConnect failed");

    DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
    requestH = WinHttpOpenRequest(connectH, L"POST", wpath.c_str(), nullptr,
                                  WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!requestH) return fail("WinHttpOpenRequest failed");

    // LLM APIs may present certificates signed by CAs not in the legacy store
    // (e.g. cross-signed roots); keep TLS on but do not fail on revocation checks.
    DWORD secFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                     SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                     SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                     SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
    WinHttpSetOption(requestH, WINHTTP_OPTION_SECURITY_FLAGS, &secFlags, sizeof(secFlags));

    // WinHTTP sends the buffer verbatim: pass UTF-8 JSON bytes directly.
    BOOL sent = WinHttpSendRequest(requestH, headers.c_str(), (DWORD)-1,
                                   (LPVOID)wire.data(), (DWORD)wire.size(),
                                   (DWORD)wire.size(), 0);
    if (!sent) return fail("WinHttpSendRequest failed: " + std::to_string(GetLastError()));
    if (!WinHttpReceiveResponse(requestH, nullptr)) {
        return fail("WinHttpReceiveResponse failed: " + std::to_string(GetLastError()));
    }

    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(requestH,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        // read error body for diagnostics
        std::string errBody;
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(requestH, &avail) && avail > 0) {
            std::vector<char> buf(avail);
            DWORD read = 0;
            if (!WinHttpReadData(requestH, buf.data(), avail, &read) || read == 0) break;
            errBody.append(buf.data(), read);
            if (errBody.size() > 4096) break;
        }
        return fail("LLM HTTP " + std::to_string(status) + ": " +
                    errBody.substr(0, errBody.size() > 512 ? 512 : errBody.size()));
    }

    SseParser parser;
    std::string full;      // 流式：累积 content
    std::string raw;       // 非流式：完整响应体
    StreamedToolCalls streamedTools;
    std::string lastFinish;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(requestH, &avail)) break;
        if (avail == 0) break;
        std::vector<char> buf(avail);
        DWORD read = 0;
        if (!WinHttpReadData(requestH, buf.data(), avail, &read) || read == 0) break;

        if (!stream) {
            raw.append(buf.data(), read);
            continue;
        }

        std::vector<std::string> payloads;
        parser.feed(buf.data(), read, payloads);
        for (const auto& p : payloads) {
            if (p == "[DONE]") continue;
            try {
                json j = json::parse(p);
                std::string delta = deltaContent(j);
                if (!delta.empty()) {
                    full += delta;
                    if (onDelta) onDelta(delta);
                }
                if (toolCallsOut || finishReasonOut) {
                    streamedTools.feed(j);
                    std::string fr = streamFinishReason(j);
                    if (!fr.empty()) lastFinish = fr;
                }
            } catch (const std::exception&) {
                // ignore malformed keep-alive fragments
            }
        }
    }

    WinHttpCloseHandle(requestH);
    WinHttpCloseHandle(connectH);
    WinHttpCloseHandle(session);

    if (stream) *rawBody = full;
    else *rawBody = raw;
    if (toolCallsOut) *toolCallsOut = streamedTools.toJsonArray();
    if (finishReasonOut) *finishReasonOut = lastFinish;
    ok = true;
    if (error) error->clear();
    return ok;
}

} // namespace deepagent
