#pragma once

#include <string>

#include "third_party/json.hpp"

namespace deepagent {

// P1: Plan IR 脚本化生成——LLM 输出受限 JS 函数（而非裸 JSON），
// 在 QuickJS 沙箱中执行后产出 Plan IR，交给确定性编译器。
//
// 脚本约定（对 LLM 暴露的全部宿主能力）：
//   define("plan", function(context) { ... return {nodes:[...], links:[...]}; });
// context 为宿主注入的纯数据对象（用户需求/目录/模板），无文件/网络/进程访问
// （不加载 quickjs-libc），限制内存上限、栈深与执行时长。
struct PlanScriptResult {
    bool ok = false;
    nlohmann::json planIr;   // ok 时有效：脚本返回的对象
    std::string error;       // !ok 时：语法/运行时/超时/超内存/结果形态错误的描述
    bool timeout = false;    // 执行超时（上层可提示重试）
    bool sandboxAbort = false; // 超时或超内存中止
};

// 执行 define("plan", function(context){...}) 形式的脚本并取回 Plan IR。
PlanScriptResult executePlanScript(const std::string& jsCode,
                                   const nlohmann::json& context,
                                   size_t memoryLimitBytes = 64 * 1024 * 1024,
                                   int timeoutMs = 5000);

} // namespace deepagent
