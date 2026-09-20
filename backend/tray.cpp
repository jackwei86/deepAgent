#include "tray.h"

#include <windows.h>
#include <shellapi.h>

#include "resource.h"

namespace deepagent {
namespace tray {
namespace {

constexpr UINT WM_APP_TRAY = WM_APP + 1;   // 托盘回调消息
constexpr UINT IDM_OPEN_CHAT = 2001;       // 打开对话（默认项）
constexpr UINT IDM_OPEN_UDRT = 2002;       // 打开输出目录
constexpr UINT IDM_EXIT = 2003;            // 退出

constexpr wchar_t kWndClass[] = L"DeepAgentTrayWnd";

NOTIFYICONDATAW g_nid = {};
TrayCallbacks g_cb;

void OpenChat()   { if (g_cb.onOpenChat) g_cb.onOpenChat(); }
void OpenUdrtDir(){ if (g_cb.onOpenUdrtDir) g_cb.onOpenUdrtDir(); }
void ExitApp()    { if (g_cb.onExit) g_cb.onExit(); }

// 右键弹出菜单；TrackPopupMenu 需要前台窗口否则点外部不消失
void ShowContextMenu(HWND hwnd) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, IDM_OPEN_CHAT, L"打开对话");
    AppendMenuW(menu, MF_STRING, IDM_OPEN_UDRT, L"打开 udrt 输出目录");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"退出");
    SetMenuDefaultItem(menu, IDM_OPEN_CHAT, FALSE);   // 加粗默认项，双击同义

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    int cmd = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RETURNCMD,
                             pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);   // 消息泵切换，保证菜单失去焦点即关闭
    DestroyMenu(menu);

    switch (cmd) {
    case IDM_OPEN_CHAT: OpenChat(); break;
    case IDM_OPEN_UDRT: OpenUdrtDir(); break;
    case IDM_EXIT:      ExitApp(); break;
    default: break;
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_APP_TRAY: {
        // lParam 低字 = 鼠标事件（对 NOTIFYICON_VERSION_4 为 XWORD）；此处沿用版本 0 语义
        switch (LOWORD(lParam)) {
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
            OpenChat();
            break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            ShowContextMenu(hwnd);
            break;
        default:
            break;
        }
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_OPEN_CHAT: OpenChat(); break;
        case IDM_OPEN_UDRT: OpenUdrtDir(); break;
        case IDM_EXIT:      ExitApp(); break;
        default: break;
        }
        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

} // namespace

bool Create(HINSTANCE hInstance, const std::wstring& tip, const TrayCallbacks& callbacks) {
    g_cb = callbacks;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kWndClass;
    if (!RegisterClassExW(&wc)) return false;

    HWND hwnd = CreateWindowExW(0, kWndClass, L"DeepAgent", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 0, 0,
                                nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) return false;
    ShowWindow(hwnd, SW_HIDE);   // 纯托盘应用：主窗口不可见

    HICON icon = static_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APPICON),
                                               IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                               GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    if (!icon) icon = LoadIconW(nullptr, IDI_APPLICATION);

    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = icon;
    wcsncpy_s(g_nid.szTip, tip.c_str(), _TRUNCATE);
    if (!Shell_NotifyIconW(NIM_ADD, &g_nid)) return false;
    return true;
}

void RunMessageLoop() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void Destroy() {
    if (g_nid.hWnd) {
        DestroyWindow(g_nid.hWnd);   // WM_DESTROY 内完成 NIM_DELETE
        g_nid = {};
    }
}

void ShowErrorBalloon(const std::wstring& title, const std::wstring& text) {
    if (!g_nid.hWnd) return;
    NOTIFYICONDATAW nid = g_nid;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_ERROR;
    wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

} // namespace tray
} // namespace deepagent
