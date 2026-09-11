"""全局路径与运行配置。

所有路径可通过环境变量覆盖，便于部署到其他机器：
  DEEPAGENT_CV_EXE     C++ SDK 可执行文件路径
  DEEPAGENT_DATA_DIR   项目数据根目录(任务单/上传/产物)
  DATA_DIR             Open WebUI 数据目录(start.bat 中与本项目一致)
"""
import os
from pathlib import Path

# E:\DeepAgent
PROJECT_ROOT = Path(__file__).resolve().parents[2]

CV_EXE = Path(os.environ.get("DEEPAGENT_CV_EXE", PROJECT_ROOT / "cpp-sdk" / "build" / "Debug" / "deepagent-cv.exe"))

DATA_DIR = Path(os.environ.get("DEEPAGENT_DATA_DIR", PROJECT_ROOT / "data"))
TASKS_DIR = DATA_DIR / "tasks"          # 每个任务一个子目录: <task_id>/task.json + result.*
OUTPUTS_DIR = DATA_DIR / "outputs"      # 独立产物目录(临时/手工执行)

# 素材库: media/<用户名>/image|video (对话中"素材库/收藏夹/当前目录"即指此目录)
MEDIA_DIR = Path(os.environ.get("DEEPAGENT_MEDIA_DIR", PROJECT_ROOT / "media"))

# Open WebUI 数据目录(其上传文件保存在 <DATA_DIR>/uploads/<file_id>/<name>)
WEBUI_DATA_DIR = Path(os.environ.get("DATA_DIR", str(DATA_DIR / "webui")))
WEBUI_UPLOADS_DIR = WEBUI_DATA_DIR / "uploads"

# SDK 引擎标识(写入任务单 execution.engine)
ENGINE_NAME = "cpp-sdk"
ENGINE_VERSION = "1.0.0"

# Windows 子进程不分配控制台窗口(托盘/无窗口宿主下防止黑框弹出)
CREATE_NO_WINDOW = 0x08000000

# 子进程环境: 抑制 OpenCV 的 INFO/WARN 日志噪音, 保证 stdout 只有 JSON-lines 协议
CHILD_ENV_BASE = {**os.environ, "OPENCV_LOG_LEVEL": "ERROR"}


def ensure_dirs() -> None:
    for d in (TASKS_DIR, OUTPUTS_DIR, MEDIA_DIR):
        d.mkdir(parents=True, exist_ok=True)
