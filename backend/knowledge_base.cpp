#include "knowledge_base.h"

#include <algorithm>
#include <cctype>
#include <fstream>

using json = nlohmann::json;

namespace deepagent {
namespace {

std::string toLower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return out;
}

int countOccurrences(const std::string& text, const std::string& needle) {
    if (needle.empty()) return 0;
    int n = 0;
    for (size_t pos = text.find(needle); pos != std::string::npos;
         pos = text.find(needle, pos + needle.size())) {
        ++n;
    }
    return n;
}

} // namespace

bool KnowledgeBase::load(const std::string& path, std::string* error) {
    std::ifstream in(path);
    if (!in.is_open()) {
        if (error) *error = "cannot open knowledge base: " + path;
        return false;
    }
    try {
        json j = json::parse(in);
        entries_ = j.value("entries", json::array());
    } catch (const std::exception& e) {
        if (error) *error = std::string("knowledge base parse error: ") + e.what();
        return false;
    }
    return true;
}

const json* KnowledgeBase::find(const std::string& id) const {
    for (const auto& e : entries_) {
        if (e.value("id", "") == id) return &e;
    }
    return nullptr;
}

json KnowledgeBase::scoreByKeywords(const std::string& message) const {
    std::string lowerMsg = toLower(message);
    json results = json::array();
    for (const auto& e : entries_) {
        int score = 0;
        for (const auto& kw : e.value("keywords", json::array())) {
            std::string k = toLower(kw.get<std::string>());
            score += countOccurrences(lowerMsg, k);
        }
        if (score > 0) {
            results.push_back({{"id", e.value("id", "")}, {"score", score}});
        }
    }
    std::sort(results.begin(), results.end(), [](const json& a, const json& b) {
        return a["score"].get<int>() > b["score"].get<int>();
    });
    return results;
}

std::string KnowledgeBase::toPromptText() const {
    json simplified = json::array();
    for (const auto& e : entries_) {
        json item;
        item["id"] = e.value("id", "");
        item["name"] = e.value("name", "");
        item["project"] = e.value("project", "");
        item["description"] = e.value("description", "");
        item["input"] = e.value("input", "");
        item["output"] = e.value("output", "");
        simplified.push_back(item);
    }
    return simplified.dump();
}

} // namespace deepagent
