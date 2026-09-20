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

} // namespace

UdrtCompiler::UdrtCompiler(const NodeCatalog& catalog) : catalog_(catalog) {}

bool UdrtCompiler::compile(const json& planIr, json* udrt, std::string* error) const {
    if (!planIr.is_object()) {
        if (error) *error = "plan IR is not an object";
        return false;
    }
    const auto& planNodes = planIr.value("nodes", json::array());
    const auto& links = planIr.value("links", json::array());
    if (!planNodes.is_array() || planNodes.empty()) {
        if (error) *error = "plan IR has no nodes";
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
            if (error) *error = "unknown catalog key in plan: " + catKey;
            return false;
        }
        if (keyToId.count(key)) {
            if (error) *error = "duplicate plan node key: " + key;
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
    for (const auto& link : links) {
        std::string from = link.value("from", "");
        std::string to = link.value("to", "");
        std::string fromNode, fromPort, toNode, toPort;
        if (!splitPortRef(from, &fromNode, &fromPort) ||
            !splitPortRef(to, &toNode, &toPort)) {
            if (error) *error = "bad port reference in link: " + from + " -> " + to;
            return false;
        }
        if (!keyToId.count(fromNode) || !keyToId.count(toNode)) {
            if (error) *error = "link references unknown node key: " + from + " -> " + to;
            return false;
        }
        int outIdx = -1, inIdx = -1;
        const json* outCat = nullptr;
        const json* inCat = nullptr;
        if (!catalog_.resolvePort(planNodes, fromNode, fromPort, &outIdx, &outCat)) {
            if (error) *error = "cannot resolve output port: " + from;
            return false;
        }
        if (!catalog_.resolvePort(planNodes, toNode, toPort, &inIdx, &inCat)) {
            if (error) *error = "cannot resolve input port: " + to;
            return false;
        }
        connections.push_back({{"inNodeId", keyToId[toNode]},
                               {"inPortIndex", inIdx},
                               {"outNodeId", keyToId[fromNode]},
                               {"outPortIndex", outIdx}});
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

bool UdrtCompiler::validate(const json& udrt, std::string* error) const {
    if (!udrt.is_object()) {
        if (error) *error = "udrt is not an object";
        return false;
    }
    auto nodesIt = udrt.find("nodes");
    auto connIt = udrt.find("connections");
    if (nodesIt == udrt.end() || !nodesIt->is_array() || nodesIt->empty()) {
        if (error) *error = "udrt has no nodes";
        return false;
    }
    if (connIt == udrt.end() || !connIt->is_array()) {
        if (error) *error = "udrt has no connections array";
        return false;
    }
    std::set<int> ids;
    for (const auto& n : *nodesIt) {
        if (!n.contains("id") || !n["id"].is_number_integer()) {
            if (error) *error = "node without integer id";
            return false;
        }
        int nid = n["id"].get<int>();
        if (!ids.insert(nid).second) {
            if (error) *error = "duplicate node id: " + std::to_string(nid);
            return false;
        }
        const json& internal = n.value("internal-data", json::object());
        std::string modelName = internal.value("model_name", "");
        if (!isValidHexModuleId(modelName)) {
            if (error) *error = "node " + std::to_string(nid) + " has invalid hex model_name: " + modelName;
            return false;
        }
        if (!n.contains("position") || !n["position"].is_object()) {
            if (error) *error = "node " + std::to_string(nid) + " missing position";
            return false;
        }
    }
    for (const auto& c : *connIt) {
        int inId = c.value("inNodeId", -1), outId = c.value("outNodeId", -1);
        if (!ids.count(inId) || !ids.count(outId)) {
            if (error) *error = "connection references unknown node id";
            return false;
        }
        if (!c.contains("inPortIndex") || !c.contains("outPortIndex")) {
            if (error) *error = "connection missing port index";
            return false;
        }
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
