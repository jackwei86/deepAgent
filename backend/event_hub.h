#pragma once

#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "third_party/json.hpp"

namespace deepagent {

// 进程内事件广播 hub（V1.5.0 R3）：exe SSE 订阅者每人一个独立队列，
// publish 按项目过滤投递。消息低频，队列容量截断最旧（防慢消费者积压）。
// 线程安全。
class EventHub {
public:
    enum class PopResult { Got, Timeout, Closed };

    class Subscription {
    public:
        explicit Subscription(std::string filterProject)
            : filterProject_(std::move(filterProject)) {}

        // filterProject 为空表示接收全部项目；否则只收 project 匹配或未带项目的事件
        const std::string& filter() const { return filterProject_; }

        void push(const std::string& event, const std::string& data) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (closed_) return;
                if (queue_.size() >= kMaxQueue) queue_.pop_front();
                queue_.emplace_back(event, data);
            }
            cv_.notify_one();
        }

        PopResult waitPop(std::string* event, std::string* data, int timeoutMs) {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
                    return closed_ || !queue_.empty();
                })) {
                return PopResult::Timeout;
            }
            if (closed_ && queue_.empty()) return PopResult::Closed;
            *event = queue_.front().first;
            *data = queue_.front().second;
            queue_.pop_front();
            return PopResult::Got;
        }

        void close() {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                closed_ = true;
            }
            cv_.notify_all();
        }

    private:
        static constexpr size_t kMaxQueue = 256;
        std::string filterProject_;
        std::mutex mutex_;
        std::condition_variable cv_;
        std::deque<std::pair<std::string, std::string>> queue_;  // (event, data-json)
        bool closed_ = false;
    };

    using SubPtr = std::shared_ptr<Subscription>;

    static EventHub& instance();

    // filterProject 为空 = 全部；否则只收匹配项目（或未带项目字段的事件）
    SubPtr subscribe(const std::string& filterProject = {});

    // project 为空 = 广播事件（所有订阅者都收）
    void publish(const std::string& event, const nlohmann::json& data,
                 const std::string& project = {});

    // 关闭全部订阅（进程退出时）
    void shutdown();

private:
    EventHub() = default;

    std::mutex mutex_;
    std::map<size_t, SubPtr> subs_;
    size_t nextId_ = 1;
};

} // namespace deepagent
