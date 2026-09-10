"""任务单(TaskOrder)管理：创建、落盘、更新、查询。

任务单 JSON 格式规范见 docs/任务信息JSON格式.md (schema_version 1.0)。
本模块是任务单的唯一写入方(助手侧)；第三方执行器按规范读取/回写。
"""
from __future__ import annotations

import json
import random
import re
import subprocess
import time
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any, Callable, Optional

from . import config

SCHEMA_VERSION = "1.0"

# task.type -> 默认输出扩展名
TASK_OUTPUT_EXT = {
    "image_beauty": "jpg",
    "image_matting": "png",
    "image_composite": "jpg",
    "video_beauty": "mp4",
    "custom": "bin",
}

_MIME_BY_EXT = {
    "jpg": "image/jpeg", "jpeg": "image/jpeg", "png": "image/png",
    "mp4": "video/mp4", "webm": "video/webm", "bin": "application/octet-stream",
}


def _now_iso() -> str:
    return datetime.now(timezone(timedelta(hours=8))).isoformat(timespec="milliseconds")


def new_task_id() -> str:
    return datetime.now().strftime("%Y%m%d-%H%M%S") + "-" + f"{random.randint(0, 0xFFFF):04x}"


class TaskOrder:
    """内存中的任务单；save() 原子写回磁盘。"""

    def __init__(self, data: dict, task_file: Path):
        self.data = data
        self.task_file = task_file

    # ---- 便捷访问 ----
    @property
    def task_id(self) -> str:
        return self.data["task_id"]

    @property
    def execution(self) -> dict:
        return self.data["execution"]

    @property
    def output_path(self) -> str:
        return self.data["output"]["path"]

    # ---- 持久化 ----
    def save(self) -> None:
        self.data["updated_at"] = _now_iso()
        tmp = self.task_file.with_suffix(".json.tmp")
        tmp.write_text(json.dumps(self.data, ensure_ascii=False, indent=2), encoding="utf-8")
        tmp.replace(self.task_file)

    # ---- 执行状态流转 ----
    def mark_running(self, command: list[str]) -> None:
        ex = self.execution
        ex.update({
            "command": command,
            "status": "running",
            "progress_percent": 0,
            "stage": "启动执行器",
            "started_at": _now_iso(),
            "finished_at": None,
            "error": None,
        })
        self.save()

    def set_progress(self, percent: int, stage: str) -> None:
        ex = self.execution
        ex["progress_percent"] = max(0, min(100, int(percent)))
        ex["stage"] = stage
        # 进度高频到达，节流落盘(每 ~2 秒或终态)
        now = time.time()
        if not hasattr(self, "_last_save") or now - self._last_save > 2.0:  # noqa: SLT001
            self._last_save = now
            self.save()

    def append_log(self, message: str, limit: int = 50) -> None:
        logs = self.execution.setdefault("logs", [])
        logs.append(message)
        if len(logs) > limit:
            del logs[0]

    def mark_finished(self, status: str, *, exit_code: Optional[int],
                      error: Optional[str] = None,
                      result: Optional[dict] = None) -> None:
        ex = self.execution
        started = ex.get("started_at")
        elapsed = None
        if started:
            try:
                t0 = datetime.fromisoformat(started)
                elapsed = int((datetime.now(tz=t0.tzinfo) - t0).total_seconds() * 1000)
            except ValueError:
                pass
        ex.update({
            "status": status,
            "progress_percent": 100 if status == "success" else ex.get("progress_percent", 0),
            "stage": "完成" if status == "success" else status,
            "finished_at": _now_iso(),
            "elapsed_ms": elapsed,
            "exit_code": exit_code,
            "error": error,
            "result": result,
        })
        self.save()


def probe_metadata(path: str | Path, timeout_s: float = 15.0) -> dict:
    """调用 C++ SDK 的 info 命令探测素材元数据(宽高/时长/帧率)。"""
    try:
        proc = subprocess.run(
            [str(config.CV_EXE), "info", "--input", str(path)],
            capture_output=True, text=True, encoding="utf-8", errors="replace",
            env=config.CHILD_ENV_BASE, timeout=timeout_s,
        )
        for line in proc.stdout.splitlines():
            line = line.strip()
            if line.startswith("{"):
                try:
                    obj = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if obj.get("event") == "info":
                    return {k: v for k, v in obj.items() if k not in ("event", "path")}
    except (OSError, subprocess.TimeoutExpired):
        pass
    return {}


def make_input_entry(path: str | Path, *, role: str = "source", file_id: str = "",
                     filename: str = "", description: str = "") -> dict:
    """构造 inputs[] 条目：文件描述 + 探测到的元数据。"""
    p = Path(path)
    entry: dict[str, Any] = {
        "file_id": file_id or p.stem,
        "role": role,
        "path": str(p.resolve()),
        "filename": filename or p.name,
    }
    ext = p.suffix.lstrip(".").lower()
    entry["mime_type"] = _MIME_BY_EXT.get(ext, "application/octet-stream")
    if p.exists():
        entry["size_bytes"] = p.stat().st_size
        meta = probe_metadata(p)
        if meta.get("kind") in ("image", "video"):
            entry["width"] = meta.get("width")
            entry["height"] = meta.get("height")
            if meta.get("kind") == "video":
                entry["duration_ms"] = meta.get("duration_ms")
                entry["fps"] = meta.get("fps")
                entry["frame_count"] = meta.get("frame_count")
    if description:
        entry["description"] = description
    return entry


def create_task(*, origin: dict, task_type: str, name: str, description: str,
                sdk_command: str, parameters: dict, inputs: list[dict],
                output_format: Optional[str] = None) -> TaskOrder:
    """创建任务单并落盘(状态 pending)。inputs 为 make_input_entry 的结果列表。"""
    config.ensure_dirs()
    task_id = new_task_id()
    task_dir = config.TASKS_DIR / task_id
    task_dir.mkdir(parents=True, exist_ok=True)

    fmt = (output_format or TASK_OUTPUT_EXT.get(task_type, "bin")).lower()
    output_path = task_dir / f"result.{fmt}"

    data = {
        "schema_version": SCHEMA_VERSION,
        "task_id": task_id,
        "created_at": _now_iso(),
        "updated_at": _now_iso(),
        "origin": {
            "input_type": "text",
            "user_text": "",
            **origin,
        },
        "task": {
            "type": task_type,
            "name": name,
            "description": description,
            "sdk_command": sdk_command,
            "parameters": parameters,
        },
        "inputs": inputs,
        "output": {
            "path": str(output_path),
            "format": fmt,
            "mime_type": _MIME_BY_EXT.get(fmt, "application/octet-stream"),
            "overwrite": True,
        },
        "execution": {
            "engine": config.ENGINE_NAME,
            "engine_version": config.ENGINE_VERSION,
            "command": [],
            "task_file": str(task_dir / "task.json"),
            "status": "pending",
            "progress_percent": 0,
            "stage": "",
            "started_at": None,
            "finished_at": None,
            "elapsed_ms": None,
            "exit_code": None,
            "error": None,
            "result": None,
            "logs": [],
        },
    }
    order = TaskOrder(data, task_dir / "task.json")
    order.save()
    return order


def load_task(task_id: str) -> Optional[TaskOrder]:
    f = config.TASKS_DIR / task_id / "task.json"
    if not f.exists():
        return None
    return TaskOrder(json.loads(f.read_text(encoding="utf-8")), f)


def list_tasks(limit: int = 50) -> list[dict]:
    """按时间倒序列出任务摘要。"""
    out = []
    if config.TASKS_DIR.exists():
        for d in sorted(config.TASKS_DIR.iterdir(), reverse=True):
            f = d / "task.json"
            if not f.is_file():
                continue
            try:
                obj = json.loads(f.read_text(encoding="utf-8"))
            except (json.JSONDecodeError, OSError):
                continue
            out.append({
                "task_id": obj.get("task_id"),
                "type": obj.get("task", {}).get("type"),
                "status": obj.get("execution", {}).get("status"),
                "progress": obj.get("execution", {}).get("progress_percent"),
                "created_at": obj.get("created_at"),
            })
            if len(out) >= limit:
                break
    return out
