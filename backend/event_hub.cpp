#include "event_hub.h"

namespace deepagent {

EventHub& EventHub::instance() {
    static EventHub g;
    return g;
}

EventHub::SubPtr EventHub::subscribe(const std::string& filterProject) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto sub = std::make_shared<Subscription>(filterProject);
    subs_[nextId_++] = sub;
    return sub;
}

void EventHub::publish(const std::string& event, const nlohmann::json& data,
                       const std::string& project) {
    const std::string dataStr = data.dump();
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, sub] : subs_) {
        const std::string& f = sub->filter();
        if (f.empty() || project.empty() || f == project)
            sub->push(event, dataStr);
    }
}

void EventHub::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, sub] : subs_) sub->close();
    subs_.clear();
}

} // namespace deepagent
