"""SDK 执行器：以命令行子进程方式执行任务单，流式解析 JSON-lines 进度事件。

执行方式(需求5"以命令行的形式去执行最终要执行的任务")：
    deepagent-cv.exe run --task <task.json>
stderr 与 stdout 合并读取避免管道阻塞；非 JSON 行按原始文本记入日志。
"""
from __future__ import annotations

import json
import subprocess
import threading
import time
from pathlib import Path
from typing import Callable, Optional

from . import config
from .task_manager import TaskOrder

# 运行中的进程注册表(供取消)
_PROCS: dict[str, subprocess.Popen] = {}
_LOCK = threading.Lock()

ProgressCb = Callable[[int, str], None]
LogCb = Callable[[str], None]


class RunError(RuntimeError):
    pass


def _normalize_ascii_paths(order: TaskOrder) -> None:
    """OpenCV 在 Windows 上无法读取非 ASCII 路径(imread/fopen ANSI 限制)。

    执行前把含非 ASCII 字符的输入复制为 ASCII 临时文件并改写任务单；
    输出路径含非 ASCII 时先输出到 ASCII 临时名，成功后回改名。
    """
    import hashlib
    import shutil

    def _is_ascii(p: Path) -> bool:
        try:
            str(p).encode("ascii")
            return True
        except UnicodeEncodeError:
            return False

    changed = False
    for inp in order.data.get("inputs", []):
        p = Path(inp.get("path", ""))
        if not p.is_file() or _is_ascii(p):
            continue
        digest = hashlib.md5(str(p).encode("utf-8")).hexdigest()[:10]
        tmp = config.OUTPUTS_DIR / f"ascii_{digest}{p.suffix.lower()}"
        if not tmp.exists() or tmp.stat().st_size != p.stat().st_size:
            shutil.copy2(p, tmp)
        inp["path"] = str(tmp)
        order.append_log(f"输入含非 ASCII 路径，已复制为临时文件: {tmp.name}")
        changed = True

    out = Path(order.output_path)
    if not _is_ascii(out):
        tmp_out = out.parent / f"result_ascii{out.suffix.lower()}"
        order.data["output"]["path"] = str(tmp_out)
        order.execution["_final_output_path"] = str(out)
        order.append_log(f"输出含非 ASCII 路径，将先写出到 {tmp_out.name}")
        changed = True

    if changed:
        order.save()


def cancel(task_id: str) -> bool:
    """终止指定任务的执行进程。"""
    with _LOCK:
        proc = _PROCS.get(task_id)
    if proc is None:
        return False
    try:
        proc.terminate()
        time.sleep(2)
        if proc.poll() is None:
            proc.kill()
    except OSError:
        pass
    return True


def run_task(order: TaskOrder, *, on_progress: Optional[ProgressCb] = None,
             on_log: Optional[LogCb] = None, timeout_s: float = 1800.0) -> dict:
    """执行任务单并回写执行结果。

    返回 order.execution 的终态字典。抛出 RunError 表示启动失败。
    """
    if not config.CV_EXE.exists():
        raise RunError(f"C++ SDK 可执行文件不存在: {config.CV_EXE}")

    task_file = Path(order.execution["task_file"])
    if not task_file.exists():
        raise RunError(f"任务单文件不存在: {task_file}")

    command = [str(config.CV_EXE), "run", "--task", str(task_file)]
    _normalize_ascii_paths(order)
    order.mark_running(command)
    if on_log:
        on_log(f"命令行: {' '.join(command)}")

    started = time.time()
    proc = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        cwd=str(config.PROJECT_ROOT),
        env=config.CHILD_ENV_BASE,
        creationflags=config.CREATE_NO_WINDOW,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    with _LOCK:
        _PROCS[order.task_id] = proc

    # 看门狗：无论进程是否持续输出，超时即终止(防止首行输出前挂起导致永久阻塞)
    killed_reason: Optional[str] = None

    def _wd() -> None:
        nonlocal killed_reason
        if proc.poll() is None:
            killed_reason = f"执行超时(>{timeout_s}s)，已终止"
            try:
                proc.kill()
            except OSError:
                pass

    wd = threading.Timer(timeout_s, _wd)
    wd.daemon = True
    wd.start()

    done_event: dict = {}
    error_event: dict = {}
    try:
        assert proc.stdout is not None
        for line in proc.stdout:
            line = line.strip()
            if not line:
                continue
            try:
                ev = json.loads(line)
            except json.JSONDecodeError:
                # OpenCV 或系统的非 JSON 输出，按日志保留
                order.append_log(line[:300])
                if on_log:
                    on_log(line[:200])
                continue

            etype = ev.get("event")
            if etype == "progress":
                order.set_progress(ev.get("percent", 0), ev.get("stage", ""))
                if on_progress:
                    on_progress(ev.get("percent", 0), ev.get("stage", ""))
            elif etype == "log":
                msg = ev.get("message", "")
                order.append_log(msg)
                if on_log:
                    on_log(msg)
            elif etype == "done":
                done_event = ev
            elif etype == "error":
                error_event = ev

        proc.wait(timeout=30)
    finally:
        wd.cancel()
        with _LOCK:
            _PROCS.pop(order.task_id, None)

    exit_code = proc.returncode
    out_path = order.output_path

    if killed_reason:
        order.mark_finished("failed", exit_code=exit_code, error=killed_reason)
    elif exit_code == 0 and done_event:
        # 非 ASCII 输出路径: 产物先落 ASCII 临时名, 这里回改名为最终路径
        final_path = order.execution.pop("_final_output_path", None)
        if final_path:
            final = Path(final_path)
            final.parent.mkdir(parents=True, exist_ok=True)
            Path(done_event.get("output", out_path)).replace(final)
            done_event["output"] = str(final)
        result = {
            "output_path": done_event.get("output", out_path),
            "elapsed_ms": done_event.get("elapsed_ms"),
        }
        p = Path(result["output_path"])
        if p.exists():
            result["size_bytes"] = p.stat().st_size
            meta = _quick_probe(p)
            result.update(meta)
        order.mark_finished("success", exit_code=0, result=result)
    else:
        msg = error_event.get("message") or f"执行器退出码 {exit_code}"
        order.mark_finished("failed", exit_code=exit_code, error=msg)

    return order.execution


def _quick_probe(p: Path) -> dict:
    """对产物做轻量探测(宽高)，失败不影响主流程。"""
    try:
        from .task_manager import probe_metadata
        meta = probe_metadata(p)
        return {k: v for k, v in meta.items() if k in ("width", "height")}
    except Exception:  # noqa: BLE001
        return {}
