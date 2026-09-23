#include "config.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

#include <windows.h>

namespace deepagent {
namespace {

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::map<std::string, std::string> parseEnvFile(const std::string& path) {
    std::map<std::string, std::string> result;
    std::ifstream in(path);
    if (!in.is_open()) return result;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));
        // strip surrounding quotes
        if (val.size() >= 2 && ((val.front() == '"' && val.back() == '"') ||
                                (val.front() == '\'' && val.back() == '\''))) {
            val = val.substr(1, val.size() - 2);
        }
        if (!key.empty()) result[key] = val;
    }
    return result;
}

std::string envOr(const std::map<std::string, std::string>& fileVars,
                  const char* name, const std::string& fallback) {
    // process env wins, then .env, then default
    const char* v = std::getenv(name);
    if (v && *v) return std::string(v);
    auto it = fileVars.find(name);
    if (it != fileVars.end()) return it->second;
    return fallback;
}

std::string narrowPath(const std::wstring& w) {
    if (w.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                   nullptr, 0, nullptr, nullptr);
    std::string s(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], size,
                        nullptr, nullptr);
    return s;
}

} // namespace

Config Config::load(const std::string& root_dir, const std::string& exe_dir) {
    Config cfg;
    auto fileVars = parseEnvFile(root_dir + "\\config\\.env");

    cfg.llm_api_key  = envOr(fileVars, "DEEPSEEK_API_KEY", "");
    cfg.llm_model    = envOr(fileVars, "DEEPSEEK_MODEL", cfg.llm_model);
    cfg.llm_base_url = envOr(fileVars, "DEEPSEEK_BASE_URL", cfg.llm_base_url);
    cfg.llm_path     = envOr(fileVars, "DEEPSEEK_PATH", cfg.llm_path);

    cfg.server_host = envOr(fileVars, "DEEPAGENT_HOST", cfg.server_host);
    cfg.server_port = std::stoi(envOr(fileVars, "DEEPAGENT_PORT", "8090"));
    cfg.ctx_retention_days = std::stoi(envOr(fileVars, "DEEPAGENT_CTX_RETENTION_DAYS", "15"));

    cfg.frontend_dir    = envOr(fileVars, "DEEPAGENT_FRONTEND_DIR", root_dir + "\\frontend");
    cfg.knowledge_dir   = envOr(fileVars, "DEEPAGENT_KNOWLEDGE_DIR", root_dir + "\\knowledge");
    cfg.udrt_output_dir = envOr(fileVars, "DEEPAGENT_UDRT_DIR", "udrt");   // 相对 exe 部署目录
    cfg.chats_file      = envOr(fileVars, "DEEPAGENT_CHATS_FILE", root_dir + "\\output\\chats.json");

    cfg.exe_dir = exe_dir;
    // 相对路径以公共部署目录（exe 目录）为基准解析
    if (!cfg.udrt_output_dir.empty() && cfg.udrt_output_dir[1] != ':') {
        cfg.udrt_output_dir = exe_dir + "\\" + cfg.udrt_output_dir;
    }

    // 工程宿主进程启动（V1.5.0）
    cfg.auto_launch = envOr(fileVars, "DEEPAGENT_AUTO_LAUNCH", "0") == "1";
    cfg.avatar_exe  = envOr(fileVars, "DEEPAGENT_AVATAR_EXE", exe_dir + "\\Avatar.exe");
    cfg.udrt_exe    = envOr(fileVars, "DEEPAGENT_UDRT_EXE", exe_dir + "\\UDeepRT.exe");
    return cfg;
}

} // namespace deepagent
