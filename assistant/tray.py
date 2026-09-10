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
VENV_PYTHON = ROOT / ".venv" / "Scripts" / "python.exe"
BASE_URL = "http://127.0.0.1:8080"

_proc: subprocess.Popen | None = None


def _child_env() -> dict:
    env = {**os.environ,
           "DEEPAGENT_ROOT": str(ROOT),
           "DATA_DIR": str(ROOT / "data" / "webui"),
           "DEEPAGENT_MEDIA_DIR": str(ROOT / "media"),
           "HF_ENDPOINT": "https://hf-mirror.com",
           "OPENCV_LOG_LEVEL": "ERROR"}
    nvidia_bins = ROOT / ".venv" / "Lib" / "site-packages" / "nvidia"
    for sub in ("cublas", "cudnn"):
        p = nvidia_bins / sub / "bin"
        if p.is_dir():
            env["PATH"] = f"{p};{env['PATH']}"
    return env


def start_service() -> None:
    global _proc
    if _proc and _proc.poll() is None:
        return
    _proc = subprocess.Popen([str(VENV_PYTHON), str(ROOT / "assistant" / "run_webui.py")],
                             cwd=str(ROOT), env=_child_env(),
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


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
        pystray.MenuItem("打开素材库", lambda: open_url("/media-ui/")),
        pystray.MenuItem("重启服务", lambda: (stop_service(), start_service())),
        pystray.MenuItem("退出", on_quit),
    )
    icon = pystray.Icon("DeepAgent", make_icon(), "DeepAgent 任务助手", menu)
    icon.run()


if __name__ == "__main__":
    main()
