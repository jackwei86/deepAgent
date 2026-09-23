#include "process_launcher.h"

#include <chrono>
#include <filesystem>

#include "log.h"

namespace deepagent {
namespace {

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::wstring toWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
    std::wstring w(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), &w[0], size);
    return w;
}

struct WatchParam {
    ProcessLauncher* self;
    HANDLE handle;
    std::string key;
};

} // namespace

ProcessLauncher& ProcessLauncher::instance() {
    static ProcessLauncher g;
    return g;
}

bool ProcessLauncher::launch(const std::string& kind, const std::string& exePath,
                             const std::string& projectAbs, DWORD* pid, std::string* error) {
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        Logf("[launcher] launch failed (%s): %s", kind.c_str(), msg.c_str());
        return false;
    };
    if (kind.empty() || exePath.empty() || projectAbs.empty())
        return fail("kind/exePath/projectAbs 均必填");

    const std::string key = kind + "|" + projectAbs;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(key);
        if (it != entries_.end() && it->second.info.running) {
            if (pid) *pid = it->second.info.pid;
            if (error) error->clear();
            return true;   // 幂等：已在运行
        }
    }

    std::wstring exeW = toWide(exePath);
    std::wstring projW = toWide(projectAbs);
    std::wstring cmd = L"\"" + exeW + L"\" \"" + projW + L"\"";

    // 工作目录 = exe 所在目录（保证 DLL/资源解析一致）
    std::filesystem::path exeFs(exePath);
    std::wstring workDir = toWide(exeFs.parent_path().string());

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE,
                        0, nullptr, workDir.empty() ? nullptr : workDir.c_str(), &si, &pi)) {
        return fail("CreateProcess failed: " + std::to_string(GetLastError()) +
                    " (exe: " + exePath + ")");
    }

    Entry e;
    e.process = pi.hProcess;
    e.info.kind = kind;
    e.info.project = projectAbs;
    e.info.exe = exePath;
    e.info.pid = pi.dwProcessId;
    e.info.running = true;
    e.info.startedAtMs = nowMs();
    CloseHandle(pi.hThread);   // 主线程句柄不需要

    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_[key] = std::move(e);
    }

    // 退出监视线程：WaitForSingleObject → 更新状态、关句柄
    auto* wp = new WatchParam{this, pi.hProcess, key};
    HANDLE watcher = CreateThread(nullptr, 0, exitWatchStub, wp, 0, nullptr);
    if (watcher) CloseHandle(watcher);

    Logf("[launcher] launched %s pid=%lu exe=%s project=%s",
         kind.c_str(), (unsigned long)pi.dwProcessId, exePath.c_str(), projectAbs.c_str());
    if (pid) *pid = pi.dwProcessId;
    if (error) error->clear();
    return true;
}

DWORD WINAPI ProcessLauncher::exitWatchStub(LPVOID param) {
    auto* wp = static_cast<WatchParam*>(param);
    wp->self->exitWatch(wp->handle, wp->key);
    delete wp;
    return 0;
}

void ProcessLauncher::exitWatch(HANDLE handle, const std::string& key) {
    WaitForSingleObject(handle, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(handle, &code);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(key);
        if (it != entries_.end()) {
            it->second.info.running = false;
            it->second.info.exitCode = code;
        }
    }
    Logf("[launcher] process exited key=%s exitCode=%lu", key.c_str(), (unsigned long)code);
    CloseHandle(handle);
}

std::vector<ProcessLauncher::Info> ProcessLauncher::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Info> out;
    out.reserve(entries_.size());
    for (const auto& [key, e] : entries_) out.push_back(e.info);
    return out;
}

ProcessLauncher::~ProcessLauncher() {
    // 托管不专制：仅关闭仍持有的句柄，进程继续运行
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [key, e] : entries_) {
        if (e.process) CloseHandle(e.process);
        e.process = nullptr;
    }
}

} // namespace deepagent
