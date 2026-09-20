#pragma once

#include <windows.h>

#include <functional>
#include <string>

namespace deepagent {

// 任务栏托盘图标 + 右键菜单（隐藏消息窗口承载）。
// 左键单击 / 双击 = 打开对话（默认项）；右键 = 弹出菜单。
struct TrayCallbacks {
    std::function<void()> onOpenChat;    // 打开对话页面
    std::function<void()> onOpenUdrtDir; // 打开 udrt 输出目录
    std::function<void()> onExit;        // 退出
};

namespace tray {
bool Create(HINSTANCE hInstance, const std::wstring& tip, const TrayCallbacks& callbacks);
void RunMessageLoop(); // 消息泵，收到 WM_QUIT 后返回
void Destroy();        // 移除托盘图标
void ShowErrorBalloon(const std::wstring& title, const std::wstring& text);
} // namespace tray

} // namespace deepagent
