#pragma once

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>

namespace deepagent {

// 工程宿主进程启动器（CreateProcessW，V1.5.0 R2/R4a）：
//   "avatar" -> Avatar.exe；"udrt" -> UDeepRT.exe（路径可由调用方覆盖）。
// 跟踪句柄/PID/退出状态；同 kind+project 重复启动幂等（返回现有 pid）。
// 后端退出不强制杀进程：仅关闭句柄，宿主进程继续运行（托管不专制）。
class ProcessLauncher {
public:
    struct Info {
        std::string kind;
        std::string project;     // 工程绝对路径
        std::string exe;         // 实际启动的 exe 路径
        DWORD pid = 0;
        bool running = false;
        DWORD exitCode = 0;
        long long startedAtMs = 0;
        std::string error;       // 启动失败原因（running=false 时可读）
    };

    // 启动宿主进程加载工程；失败返回 false 并填 error。
    // 幂等：同 kind+project 已在运行时直接返回成功（pid 填现有值）。
    bool launch(const std::string& kind, const std::string& exePath,
                const std::string& projectAbs, DWORD* pid, std::string* error);

    // 当前全部启动记录快照（含已退出的）
    std::vector<Info> snapshot() const;

    static ProcessLauncher& instance();

    // 不强制杀进程：仅关闭句柄，宿主进程继续运行
    ~ProcessLauncher();

private:
    struct Entry {
        HANDLE process = nullptr;
        Info info;
    };

    static DWORD WINAPI exitWatchStub(LPVOID param);
    void exitWatch(HANDLE handle, const std::string& key);

    mutable std::mutex mutex_;
    std::map<std::string, Entry> entries_;   // key = kind + "|" + project
};

} // namespace deepagent
