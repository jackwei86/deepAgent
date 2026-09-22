#include "plan_script.h"

#include <windows.h>

#include <quickjs-msvc-port.h>

using json = nlohmann::json;

namespace deepagent {
namespace {

// 预置脚本：define("name", fn) 收集器（用户脚本之前求值）
const char* const kPrelude =
    "(function(){ globalThis.__registry = {}; "
    "globalThis.define = function(name, fn){ globalThis.__registry[name] = fn; }; })();";

struct ScriptDeadline {
    ULONGLONG start;
    int ms;
};

// 中断回调：超时返回 1，引擎以异常中止当前执行
int interruptHandler(JSRuntime* rt, void* opaque) {
    (void)rt;
    auto* d = static_cast<ScriptDeadline*>(opaque);
    if (d->ms <= 0) return 0;
    return (GetTickCount64() - d->start) > static_cast<ULONGLONG>(d->ms) ? 1 : 0;
}

// 取当前挂起异常的可读描述（Error 带 message+stack，其余 JSON 序列化）
std::string describeException(JSContext* ctx, bool* timeout) {
    *timeout = false;
    JSValue exc = JS_GetException(ctx);
    std::string out;
    if (!JS_IsNull(exc) && !JS_IsUndefined(exc)) {
        if (JS_IsError(ctx, exc)) {
            JSValue msg = JS_GetPropertyStr(ctx, exc, "message");
            size_t len = 0;
            const char* s = JS_ToCStringLen2(ctx, &len, msg, 0);
            if (s) {
                out.assign(s, len);
                JS_FreeCString(ctx, s);
            }
            JS_FreeValue(ctx, msg);
            const std::string lower = out;
            if (lower.find("interrupted") != std::string::npos)
                *timeout = true;
            JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");
            if (!JS_IsUndefined(stack) && !JS_IsNull(stack)) {
                size_t slen = 0;
                const char* ss = JS_ToCStringLen2(ctx, &slen, stack, 0);
                if (ss) {
                    if (!out.empty()) out += "\n";
                    out.append(ss, slen);
                    JS_FreeCString(ctx, ss);
                }
            }
            JS_FreeValue(ctx, stack);
        } else {
            JSValue str = JS_JSONStringify(ctx, exc, JS_UNDEFINED, JS_UNDEFINED);
            if (JS_IsException(str)) {
                JS_GetException(ctx);  // 清掉 stringify 自身的异常
                out = "non-serializable thrown value";
            } else {
                size_t len = 0;
                const char* s = JS_ToCStringLen2(ctx, &len, str, 0);
                if (s) {
                    out.assign(s, len);
                    JS_FreeCString(ctx, s);
                }
            }
            JS_FreeValue(ctx, str);
        }
    }
    JS_FreeValue(ctx, exc);
    if (out.empty()) out = "unknown script error";
    return out;
}

} // namespace

PlanScriptResult executePlanScript(const std::string& jsCode,
                                   const json& context,
                                   size_t memoryLimitBytes,
                                   int timeoutMs) {
    PlanScriptResult result;
    if (jsCode.empty()) {
        result.error = "empty script";
        return result;
    }

    ScriptDeadline deadline{GetTickCount64(), timeoutMs};

    JSRuntime* rt = JS_NewRuntime();
    if (!rt) {
        result.error = "cannot create JS runtime";
        return result;
    }
    JS_SetMemoryLimit(rt, memoryLimitBytes);
    JS_SetMaxStackSize(rt, 1 << 20);  // 1MB 栈上限（默认值依赖宿主线程，显式收紧）
    JS_SetInterruptHandler(rt, interruptHandler, &deadline);

    JSContext* ctx = JS_NewContext(rt);
    if (!ctx) {
        JS_FreeRuntime(rt);
        result.error = "cannot create JS context";
        return result;
    }

    auto fail = [&](const std::string& msg) {
        result.error = msg;
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return result;
    };

    // 1. 注入 context（纯数据，ParseJSON 而非 eval）
    const std::string contextJson = context.dump();
    JSValue contextVal = JS_ParseJSON(ctx, contextJson.data(), contextJson.size(), "<context>");
    if (JS_IsException(contextVal)) {
        bool timeout = false;
        return fail("context serialize failed: " + describeException(ctx, &timeout));
    }
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "context", contextVal);

    // 2. 求值 prelude + 用户脚本（GLOBAL 类型，非模块，无 import/require）
    const std::string full = std::string(kPrelude) + "\n" + jsCode;
    JSValue scriptVal = JS_Eval(ctx, full.data(), full.size(), "<plan>",
                                JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(scriptVal)) {
        bool timeout = false;
        std::string desc = describeException(ctx, &timeout);
        result.timeout = timeout;
        result.sandboxAbort = timeout;
        JS_FreeValue(ctx, global);  // 否则 global 及其 context 属性残留，FreeRuntime 断言
        return fail(std::string(timeout ? "script timeout after " +
                                         std::to_string(timeoutMs) + "ms: "
                                       : "script syntax/runtime error: ") + desc);
    }
    JS_FreeValue(ctx, scriptVal);

    // 3. 取回 __registry.plan，必须是函数
    JSValue registry = JS_GetPropertyStr(ctx, global, "__registry");
    JSValue planFn = JS_IsObject(registry)
                         ? JS_GetPropertyStr(ctx, registry, "plan")
                         : JS_UNDEFINED;
    JS_FreeValue(ctx, registry);
    if (!JS_IsFunction(ctx, planFn)) {
        JS_FreeValue(ctx, planFn);
        JS_FreeValue(ctx, global);
        return fail("script must register a plan function: "
                    "define(\"plan\", function(context){ ... return "
                    "{\"nodes\":[...], \"links\":[...]}; });");
    }

    // 4. 调用 plan(context)
    JSValue callArg = JS_GetPropertyStr(ctx, global, "context");
    JSValue ret = JS_Call(ctx, planFn, JS_UNDEFINED, 1, &callArg);
    JS_FreeValue(ctx, callArg);
    JS_FreeValue(ctx, planFn);
    JS_FreeValue(ctx, global);
    if (JS_IsException(ret)) {
        bool timeout = false;
        std::string desc = describeException(ctx, &timeout);
        result.timeout = timeout;
        result.sandboxAbort = timeout;
        return fail(std::string(timeout ? "script timeout after " +
                                         std::to_string(timeoutMs) + "ms during plan(): "
                                       : "plan() threw: ") + desc);
    }

    // 5. 结果 JSON 序列化 → Plan IR
    JSValue serialized = JS_JSONStringify(ctx, ret, JS_UNDEFINED, JS_UNDEFINED);
    JS_FreeValue(ctx, ret);
    if (JS_IsException(serialized)) {
        bool timeout = false;
        return fail("plan() result is not JSON-serializable: " +
                    describeException(ctx, &timeout));
    }
    std::string planText;
    {
        size_t len = 0;
        const char* s = JS_ToCStringLen2(ctx, &len, serialized, 0);
        if (s) {
            planText.assign(s, len);
            JS_FreeCString(ctx, s);
        }
    }
    JS_FreeValue(ctx, serialized);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    if (planText.empty()) {
        result.error = "plan() returned nothing (expected an object)";
        return result;
    }
    json parsed = json::parse(planText, nullptr, false);
    if (!parsed.is_object()) {
        result.error = "plan() must return an object with nodes/links arrays, got: " +
                       planText.substr(0, 200);
        return result;
    }
    result.ok = true;
    result.planIr = std::move(parsed);
    return result;
}

} // namespace deepagent
