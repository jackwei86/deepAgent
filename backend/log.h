#pragma once

// GUI 子系统下无控制台：日志写入 <root>/output/deepagent.log 并转发 OutputDebugString。
// logFile 为空时仅 OutputDebugString。
namespace deepagent {

void InitLog(const char* logFile);
void Logf(const char* fmt, ...);

} // namespace deepagent
