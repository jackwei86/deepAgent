#include "node_catalog.h"

#include <algorithm>
#include <fstream>

using json = nlohmann::json;

namespace deepagent {

bool NodeCatalog::load(const std::string& path, std::string* error) {
    std::ifstream in(path);
    if (!in.is_open()) {
        if (error) *error = "cannot open node catalog: " + path;
        return false;
    }
    try {
        json j = json::parse(in);
        nodes_ = j.value("nodes", json::object());
    } catch (const std::exception& e) {
        if (error) *error = std::string("node catalog parse error: ") + e.what();
        return false;
    }
    return true;
}

const json* NodeCatalog::node(const std::string& catalogKey) const {
    auto it = nodes_.find(catalogKey);
    if (it == nodes_.end()) return nullptr;
    return &it.value();
}

std::vector<std::string> NodeCatalog::catalogKeys() const {
    std::vector<std::string> keys;
    for (auto it = nodes_.begin(); it != nodes_.end(); ++it) keys.push_back(it.key());
    std::sort(keys.begin(), keys.end());
    return keys;
}

bool NodeCatalog::resolvePort(const json& planNodes, const std::string& nodeKey,
                              const std::string& semanticPort, int* portIndex,
                              const json** catalogNode) const {
    for (const auto& pn : planNodes) {
        if (pn.value("key", "") != nodeKey) continue;
        const json* cat = node(pn.value("catalog", ""));
        if (!cat) return false;
        // try output ports then input ports
        for (const char* field : {"output_ports", "input_ports"}) {
            auto ports = cat->find(field);
            if (ports == cat->end() || !ports->is_object()) continue;
            auto p = ports->find(semanticPort);
            if (p != ports->end() && p->is_number_integer()) {
                if (portIndex) *portIndex = p->get<int>();
                if (catalogNode) *catalogNode = cat;
                return true;
            }
        }
        return false;
    }
    return false;
}

} // namespace deepagent
