"""DeepAgent 托盘程序：系统托盘图标管理集成服务(Open WebUI + 素材库, 8080)。

以 pythonw 运行(无控制台窗口)，菜单：
  打开聊天界面 / 打开素材库 / 启动或重启服务 / 退出
依赖: pip install pystray pillow
"""
import os
import subprocess
import sys
import threading
import webbrowser
from pathlib import Path

import pystray
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
VENV_PYTHONW = ROOT / ".venv" / "Scripts" / "pythonw.exe"
BASE_URL = "http://127.0.0.1:8080"
CREATE_NO_WINDOW = 0x08000000  # 子进程不分配控制台(防黑框)

_proc: subprocess.Popen | None = None


def _child_env() -> dict:
    env = {**os.environ,
           "DEEPAGENT_ROOT": str(ROOT),
           "DATA_DIR": str(ROOT / "data" / "webui"),
           "DEEPAGENT_MEDIA_DIR": str(ROOT / "media"),
           "HF_ENDPOINT": "https://hf-mirror.com",
           "OPENCV_LOG_LEVEL": "ERROR"}
    # 剥离代理环境变量：aiohttp 不支持 socks5, 会导致服务内
    # 模型列表/连接校验报 Connection error；DeepSeek/GLM/MiniMax
    # 均为国内直连，无需代理
    for k in ("HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY",
              "http_proxy", "https_proxy", "all_proxy"):
        env.pop(k, None)
    env["NO_PROXY"] = "*"
    nvidia_bins = ROOT / ".venv" / "Lib" / "site-packages" / "nvidia"
    for sub in ("cublas", "cudnn"):
        p = nvidia_bins / sub / "bin"
        if p.is_dir():
            env["PATH"] = f"{p};{env['PATH']}"
    return env


def _port_in_use(port: int = 8080) -> bool:
    import socket
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.settimeout(0.5)
        return s.connect_ex(("127.0.0.1", port)) == 0


def start_service() -> None:
    global _proc
    if _proc and _proc.poll() is None:
        return
    if _port_in_use():
        # 8080 已有服务(例如手动启动的)在跑：托盘只做管理入口，不重复拉起
        return
    # pythonw(无控制台) + CREATE_NO_WINDOW：双保险防止黑框
    # 服务日志写入文件(排障必需)，托盘退出不杀日志
    log_fh = open(ROOT / "data" / "webui_service.log", "ab")
    _proc = subprocess.Popen([str(VENV_PYTHONW), str(ROOT / "assistant" / "run_webui.py")],
                             cwd=str(ROOT), env=_child_env(),
                             creationflags=CREATE_NO_WINDOW,
                             stdout=log_fh, stderr=log_fh)


def stop_service() -> None:
    global _proc
    if _proc and _proc.poll() is None:
        _proc.terminate()
        _proc = None


def open_url(path: str) -> None:
    webbrowser.open(BASE_URL + path)


def make_icon() -> Image.Image:
    img = Image.new("RGBA", (64, 64), (31, 36, 48, 255))
    d = ImageDraw.Draw(img)
    d.ellipse([8, 8, 56, 56], fill=(59, 130, 246, 255))
    d.text((24, 22), "D", fill="white")
    return img


def on_quit(icon, item) -> None:
    stop_service()
    icon.stop()


def main() -> None:
    start_service()
    menu = pystray.Menu(
        pystray.MenuItem("打开聊天界面", lambda: open_url("/"), default=True),
        pystray.MenuItem("打开素材库", lambda: open_url("/media-ui/?v=20260914")),
        pystray.MenuItem("重启服务", lambda: (stop_service(), start_service())),
        pystray.MenuItem("退出", on_quit),
    )
    icon = pystray.Icon("DeepAgent", make_icon(), "DeepAgent 任务助手", menu)
    icon.run()


if __name__ == "__main__":
    main()
