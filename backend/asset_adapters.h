#pragma once

// 资产子步适配器：自定义处理过程的宿主（v1 = C++ 函数，接口按 QuickJS 脚本化预留）。
// 设计文档：docs/Node资产生成子步并发设计.md §3。
//
// 节点类型（进程级注册，asset_adapters.cpp 内 ensureAssetNodesRegistered）：
//   da.adapter —— 读 inputs 声明的通道值列表 → AdapterFn → 写 output 通道
//   da.llm     —— 读 prompt_channel（[{role,content}] 数组）→ Provider.complete_async
//                 → StripHtmlFence → 写 output 通道（字符串）

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "third_party/json.hpp"

#include <neograph/neograph.h>

namespace deepagent {

// 适配器函数：inputs 为按 schema 顺序的通道值列表（未写入的通道为 null），
// params 为节点 config 里的扩展参数。返回值写入 output 通道。
using AssetAdapterFn =
    std::function<nlohmann::json(const std::vector<nlohmann::json>& inputs,
                                 const nlohmann::json& params)>;

// 剥掉 LLM 回复外层的 ```html 代码围栏（与旧管线 StripHtmlFence 同语义）。
std::string stripHtmlFence(const std::string& raw);

class AdapterRegistry {
public:
    static AdapterRegistry& instance();

    void registerAdapter(const std::string& name, AssetAdapterFn fn);
    bool has(const std::string& name) const;
    // 未注册名抛 std::runtime_error
    nlohmann::json invoke(const std::string& name, const std::vector<nlohmann::json>& inputs,
                          const nlohmann::json& params) const;

private:
    std::map<std::string, AssetAdapterFn> adapters_;
};

// 注册内置适配器（bg_plate / html_compose）与节点类型（da.adapter / da.llm）。
// 幂等；每次调用 runner 前调一次即可（内部 static guard）。
void ensureAssetNodesRegistered();

}  // namespace deepagent
