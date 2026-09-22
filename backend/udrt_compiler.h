#pragma once

#include <string>

#include "node_catalog.h"
#include "third_party/json.hpp"

namespace deepagent {

// Deterministic Graph-Plan-IR -> udrt compiler (the LLM never writes udrt).
//
// Plan IR:
//   nodes: [{"key": "tool", "catalog": "saturn_subtitle_html", "props": {...}?}, ...]
//   links: [{"from": "tool:html_file", "to": "sink:in0"}, ...]
//   phases: [{"name": "生成", "nodes": ["tool"]}, ...]?   // P4 预留：仅供 UI 分组展示，
//            编译器不写入 udrt 文件（保持 U-DeepRT 格式兼容），由上层透传给前端
//
// P2: compile()/validate() 的 detailedError 出参返回模型可读的结构化诊断
//   {"error", "path", "expected", "actual", "hint", "known_catalogs"?}，
//   Agent Loop 将其回填给 LLM 实现自修复。
class UdrtCompiler {
public:
    explicit UdrtCompiler(const NodeCatalog& catalog);

    // Compile a Plan IR into a udrt JSON document.
    // On failure fills error (单行摘要) and detailedError (结构化诊断，可空) and returns false.
    bool compile(const nlohmann::json& planIr, nlohmann::json* udrt,
                 std::string* error,
                 nlohmann::json* detailedError = nullptr) const;

    // Structural validation of a compiled udrt (nodes/connections/module ids).
    bool validate(const nlohmann::json& udrt, std::string* error,
                  nlohmann::json* detailedError = nullptr) const;

    // Write udrt JSON to file (4-space indent, UTF-8).
    bool writeFile(const nlohmann::json& udrt, const std::string& path,
                   std::string* error) const;

    // Build a unique output file path for a template prefix, e.g.
    // <dir>/subtitle_html_20260917_142530.udrt
    static std::string makeFilePath(const std::string& dir, const std::string& prefix);

private:
    const NodeCatalog& catalog_;
};

} // namespace deepagent
