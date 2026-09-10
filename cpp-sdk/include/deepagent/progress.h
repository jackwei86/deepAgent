// 进度与日志回调：SDK 所有长耗时接口均通过此结构上报执行进度，
// 上层(CLI/第三方宿主)据此转发为 JSON-lines 事件或 UI 进度条。
#pragma once

#include <functional>
#include <string>

namespace deepagent {

// percent: 0-100; stage: 当前阶段的人类可读描述(UTF-8)
using ProgressCallback = std::function<void(int percent, const std::string& stage)>;
using LogCallback = std::function<void(const std::string& message)>;

struct ProgressReporter {
    ProgressCallback on_progress; // 可为空
    LogCallback on_log;           // 可为空
};

inline void report_progress(const ProgressReporter& r, int percent, const std::string& stage) {
    if (r.on_progress) r.on_progress(percent, stage);
}

inline void report_log(const ProgressReporter& r, const std::string& message) {
    if (r.on_log) r.on_log(message);
}

} // namespace deepagent
