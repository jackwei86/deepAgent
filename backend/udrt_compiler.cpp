#include "udrt_compiler.h"

#include <chrono>
#include <cctype>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

#include <windows.h>
#include <combaseapi.h>

using json = nlohmann::json;

namespace deepagent {
namespace {

const char* kUDeepRtVersion = "0x20260818";  // matches udrt/gaus_render.udrt

// 生成 GUID（大写无连字符，如 D4A1B2C3D5E6478F9A0B1C2D3E4F5A6B），
// DeepAgentBackend 与 Node 之间以此标识会话上下文
std::string newGuid() {
    GUID g{};
    if (FAILED(CoCreateGuid(&g))) return {};
    char buf[64] = {};
    snprintf(buf, sizeof(buf), "%08X%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X",
             (unsigned)g.Data1, (unsigned)g.Data2, (unsigned)g.Data3,
             g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
             g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return buf;
}

bool isValidHexModuleId(const std::string& s) {
    if (s.size() != 10) return false;
    if (s[0] != '0' || (s[1] != 'x' && s[1] != 'X')) return false;
    for (size_t i = 2; i < s.size(); ++i) {
        if (!std::isxdigit((unsigned char)s[i])) return false;
    }
    return true;
}

// parse "nodeKey:semanticPort"
bool splitPortRef(const std::string& ref, std::string* nodeKey, std::string* port) {
    auto pos = ref.find(':');
    if (pos == std::string::npos || pos == 0 || pos + 1 >= ref.size()) return false;
    *nodeKey = ref.substr(0, pos);
    *port = ref.substr(pos + 1);
    return true;
}

// P2: 结构化诊断（path/expected/actual/hint）——Agent Loop 将其回填给 LLM 自修复
void emitError(std::string* error, json* detailed,
               const std::string& msg, const std::string& path,
               const std::string& expected, const std::string& actual,
               const std::string& hint = {},
               json knownCatalogs = {}) {
    if (error) *error = msg;
    if (detailed) {
        json d{{"error", msg}, {"path", path}, {"expected", expected}, {"actual", actual}};
        if (!hint.empty()) d["hint"] = hint;
        if (knownCatalogs.is_array()) d["known_catalogs"] = knownCatalogs;
        *detailed = std::move(d);
    }
}

} // namespace

UdrtCompiler::UdrtCompiler(const NodeCatalog& catalog) : catalog_(catalog) {}

// 已知 catalog key 清单（P2: unknown catalog 时回填给 LLM 的合法值域）
static json knownCatalogKeys(const NodeCatalog& catalog) {
    json keys = json::array();
    for (const auto& k : catalog.catalogKeys()) keys.push_back(k);
    return keys;
}

bool UdrtCompiler::compile(const json& planIr, json* udrt, std::string* error,
                           json* detailedError) const {
    if (!planIr.is_object()) {
        emitError(error, detailedError, "plan IR is not an object", "/",
                  "JSON object", planIr.type_name(),
                  "Plan IR 必须是 {\"nodes\": [...], \"links\": [...]} 形式的对象");
        return false;
    }
    const auto& planNodes = planIr.value("nodes", json::array());
    const auto& links = planIr.value("links", json::array());
    if (!planNodes.is_array() || planNodes.empty()) {
        emitError(error, detailedError, "plan IR has no nodes", "/nodes",
                  "non-empty array of {key, catalog}",
                  planNodes.is_array() ? "empty array" : planNodes.type_name(),
                  "nodes 为空：请至少放置一个节点（可参考 template 的 nodes）");
        return false;
    }

    // 1. instantiate nodes (instance ids 0..n, deterministic layout)
    json nodes = json::array();
    std::map<std::string, int> keyToId;
    int id = 0;
    for (const auto& pn : planNodes) {
        std::string key = pn.value("key", "");
        std::string catKey = pn.value("catalog", "");
        const json* cat = catalog_.node(catKey);
        if (!cat) {
            emitError(error, detailedError,
                      "unknown catalog key in plan: " + catKey,
                      "/nodes[" + std::to_string(id) + "]/catalog",
                      "one of known_catalogs", catKey,
                      "catalog key 必须取自 known_catalogs；"
                      "先调 list_knowledge_nodes 获取合法清单",
                      knownCatalogKeys(catalog_));
            return false;
        }
        if (keyToId.count(key)) {
            emitError(error, detailedError,
                      "duplicate plan node key: " + key,
                      "/nodes[" + std::to_string(id) + "]/key",
                      "unique instance key", key,
                      "key 是实例名，同一 Plan 内不得重复；请重命名其中一个");
            return false;
        }
        keyToId[key] = id;

        json internal;
        internal["model_comment"] = cat->value("model_comment", catKey);
        internal["model_name"] = cat->value("model_name", "");
        // merge catalog default props then plan overrides
        json props = cat->value("props", json::object());
        if (pn.contains("props") && pn["props"].is_object()) {
            for (auto it = pn["props"].begin(); it != pn["props"].end(); ++it) {
                props[it.key()] = it.value();
            }
        }
        // 为每个节点指定 GUID：目录声明 strGuid 属性的写入 props（Node 运行期
        // 经 DyProps 读取，用于与 DeepAgentBackend 的会话关联）；所有节点均在
        // internal-data.guid 记录（供上层按 GUID 双向交互）。
        const std::string guid = newGuid();
        if (props.is_object() && props.contains("strGuid"))
            props["strGuid"] = guid;
        internal["guid"] = guid;
        internal["props"] = props;
        internal["props_st"] = cat->value("props_st", json::object());
        if (cat->contains("source_index")) internal["source_index"] = (*cat)["source_index"];
        if (cat->contains("sinker_index")) internal["sinker_index"] = (*cat)["sinker_index"];

        json node;
        node["id"] = id;
        node["internal-data"] = internal;
        // simple layered layout: keep plan order, spread horizontally
        node["position"] = {{"x", id * 320}, {"y", 120 + (id % 2) * 220}};
        nodes.push_back(node);
        ++id;
    }

    // 2. resolve semantic links to numeric port indexes
    json connections = json::array();
    int linkIdx = 0;
    for (const auto& link : links) {
        const std::string idx = std::to_string(linkIdx);
        std::string from = link.value("from", "");
        std::string to = link.value("to", "");
        std::string fromNode, fromPort, toNode, toPort;
        if (!splitPortRef(from, &fromNode, &fromPort) ||
            !splitPortRef(to, &toNode, &toPort)) {
            emitError(error, detailedError,
                      "bad port reference in link: " + from + " -> " + to,
                      "/links[" + idx + "]",
                      "{\"from\": \"nodeKey:outPort\", \"to\": \"nodeKey:inPort\"}",
                      from + " -> " + to,
                      "端口引用必须是 \"实例名:端口名\" 两段式，冒号分隔");
            return false;
        }
        if (!keyToId.count(fromNode) || !keyToId.count(toNode)) {
            std::string missing = keyToId.count(fromNode) ? toNode : fromNode;
            json knownKeys = json::array();
            for (const auto& [k, v] : keyToId) knownKeys.push_back(k);
            emitError(error, detailedError,
                      "link references unknown node key: " + from + " -> " + to,
                      "/links[" + idx + "]",
                      "declared node key (one of /nodes[].key)", missing,
                      "link 只能引用 nodes 数组中已声明的实例 key",
                      std::move(knownKeys));
            return false;
        }
        int outIdx = -1, inIdx = -1;
        const json* outCat = nullptr;
        const json* inCat = nullptr;
        if (!catalog_.resolvePort(planNodes, fromNode, fromPort, &outIdx, &outCat)) {
            emitError(error, detailedError,
                      "cannot resolve output port: " + from,
                      "/links[" + idx + "]/from",
                      "output port of " + fromNode, fromPort,
                      "该节点的输出端口名不合法；可用的语义端口见其 catalog 的 "
                      "output_ports（context.catalogs 已列出）");
            return false;
        }
        if (!catalog_.resolvePort(planNodes, toNode, toPort, &inIdx, &inCat)) {
            emitError(error, detailedError,
                      "cannot resolve input port: " + to,
                      "/links[" + idx + "]/to",
                      "input port of " + toNode, toPort,
                      "该节点的输入端口名不合法；可用的语义端口见其 catalog 的 "
                      "input_ports（context.catalogs 已列出）");
            return false;
        }
        connections.push_back({{"inNodeId", keyToId[toNode]},
                               {"inPortIndex", inIdx},
                               {"outNodeId", keyToId[fromNode]},
                               {"outPortIndex", outIdx}});
        ++linkIdx;
    }

    json doc;
    doc["UDeepRtVersion"] = kUDeepRtVersion;
    doc["connections"] = connections;
    doc["nodes"] = nodes;
    doc["view_offset_x"] = 0;
    doc["view_offset_y"] = 0;
    doc["view_scale"] = 1;
    *udrt = doc;
    return true;
}

bool UdrtCompiler::validate(const json& udrt, std::string* error,
                            json* detailedError) const {
    if (!udrt.is_object()) {
        emitError(error, detailedError, "udrt is not an object", "/",
                  "JSON object", udrt.type_name());
        return false;
    }
    auto nodesIt = udrt.find("nodes");
    auto connIt = udrt.find("connections");
    if (nodesIt == udrt.end() || !nodesIt->is_array() || nodesIt->empty()) {
        emitError(error, detailedError, "udrt has no nodes", "/nodes",
                  "non-empty array",
                  nodesIt == udrt.end() ? "missing" : std::string(nodesIt->type_name()));
        return false;
    }
    if (connIt == udrt.end() || !connIt->is_array()) {
        emitError(error, detailedError, "udrt has no connections array", "/connections",
                  "array",
                  connIt == udrt.end() ? "missing" : std::string(connIt->type_name()));
        return false;
    }
    std::set<int> ids;
    size_t ni = 0;
    for (const auto& n : *nodesIt) {
        const std::string at = std::to_string(ni);
        if (!n.contains("id") || !n["id"].is_number_integer()) {
            emitError(error, detailedError, "node without integer id",
                      "/nodes[" + at + "]/id", "integer",
                      n.contains("id") ? std::string(n["id"].type_name()) : "missing");
            return false;
        }
        int nid = n["id"].get<int>();
        if (!ids.insert(nid).second) {
            emitError(error, detailedError, "duplicate node id: " + std::to_string(nid),
                      "/nodes[" + at + "]/id", "unique integer id",
                      std::to_string(nid));
            return false;
        }
        const json& internal = n.value("internal-data", json::object());
        std::string modelName = internal.value("model_name", "");
        if (!isValidHexModuleId(modelName)) {
            emitError(error, detailedError,
                      "node " + std::to_string(nid) + " has invalid hex model_name: " + modelName,
                      "/nodes[" + at + "]/internal-data/model_name",
                      "\"0x\" + 8 hex digits (10 chars total)", modelName,
                      "model_name 是引擎模块注册的十六进制 ID，取自目录默认值，"
                      "Plan IR 不应覆盖它");
            return false;
        }
        if (!n.contains("position") || !n["position"].is_object()) {
            emitError(error, detailedError,
                      "node " + std::to_string(nid) + " missing position",
                      "/nodes[" + at + "]/position", "object {x, y}", "missing");
            return false;
        }
        ++ni;
    }
    size_t ci = 0;
    for (const auto& c : *connIt) {
        int inId = c.value("inNodeId", -1), outId = c.value("outNodeId", -1);
        if (!ids.count(inId) || !ids.count(outId)) {
            emitError(error, detailedError, "connection references unknown node id",
                      "/connections[" + std::to_string(ci) + "]",
                      "existing node ids",
                      "inNodeId=" + std::to_string(inId) +
                          ", outNodeId=" + std::to_string(outId));
            return false;
        }
        if (!c.contains("inPortIndex") || !c.contains("outPortIndex")) {
            emitError(error, detailedError, "connection missing port index",
                      "/connections[" + std::to_string(ci) + "]",
                      "inPortIndex + outPortIndex integers", "missing");
            return false;
        }
        ++ci;
    }
    return true;
}

bool UdrtCompiler::writeFile(const json& udrt, const std::string& path,
                             std::string* error) const {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        if (error) *error = "cannot open file for write: " + path;
        return false;
    }
    out << udrt.dump(4);
    out.close();
    if (!out.good()) {
        if (error) *error = "failed writing file: " + path;
        return false;
    }
    return true;
}

std::string UdrtCompiler::makeFilePath(const std::string& dir, const std::string& prefix) {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    std::ostringstream oss;
    oss << dir << "\\";
    if (!prefix.empty()) oss << prefix << "_";
    oss << std::put_time(&tm, "%Y%m%d_%H%M%S") << ".udrt";
    return oss.str();
}

} // namespace deepagent
