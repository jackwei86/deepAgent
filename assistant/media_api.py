"""素材库 API 路由（由 run_webui.py 挂载到 Open WebUI 应用，前缀 /media/api）。

鉴权复用 Open WebUI 的 get_verified_user；文件操作均限定在
media/<当前用户名>/ 内（防目录穿越）。
"""
from __future__ import annotations

import json
from pathlib import Path

from fastapi import APIRouter, Depends, HTTPException, UploadFile, File
from fastapi.responses import FileResponse

from .core import media, task_manager

router = APIRouter()


def _current_user():
    # 延迟导入: 仅在 run_webui.py 同进程内可用
    from open_webui.utils.auth import get_verified_user
    return get_verified_user


@router.get("/tree")
def tree(user=Depends(_current_user())):
    return media.list_tree(user.name)


@router.post("/upload")
async def upload(files: list[UploadFile] = File(...), user=Depends(_current_user())):
    saved, rejected = [], []
    for f in files:
        kind = media.classify(f.filename or "")
        if kind is None:
            rejected.append(f.filename or "?")
            continue
        dest_dir = media.user_media_root(user.name) / kind
        dest = dest_dir / Path(f.filename).name
        with dest.open("wb") as out:
            while chunk := await f.read(1024 * 1024):
                out.write(chunk)
        saved.append({"name": dest.name, "kind": kind,
                      "rel_path": f"{kind}/{dest.name}", "size": dest.stat().st_size})
    return {"saved": saved, "rejected": rejected}


@router.get("/download")
def download(path: str, user=Depends(_current_user())):
    target = media.safe_user_path(user.name, path)
    if target is None:
        raise HTTPException(404, "文件不存在或路径越界")
    return FileResponse(target, filename=target.name)


@router.delete("/file")
def delete_file(path: str, user=Depends(_current_user())):
    target = media.safe_user_path(user.name, path)
    if target is None:
        raise HTTPException(404, "文件不存在或路径越界")
    target.unlink()
    return {"deleted": target.name}


@router.get("/task-outputs")
def task_outputs(user=Depends(_current_user())):
    """最近的任务产物（供一键导入素材库），附任务链关联字段。"""
    out = []
    for order in task_manager.list_tasks(limit=30):
        full = task_manager.load_task(order["task_id"])
        if not full:
            continue
        result = full.execution.get("result") or {}
        p = Path(result.get("output_path") or "")
        if p.is_file():
            out.append({"task_id": order["task_id"], "type": order["type"],
                        "status": order["status"], "name": p.name,
                        "path": str(p), "size": p.stat().st_size,
                        "parent_task_id": full.data.get("parent_task_id"),
                        "pipeline_id": full.data.get("pipeline_id")})
    return out


@router.get("/chain/{task_id}")
def chain(task_id: str, user=Depends(_current_user())):
    """任务所在处理链（版本树）：按时间正序的链上任务列表。"""
    c = task_manager.get_chain(task_id)
    if c is None:
        raise HTTPException(404, "任务不存在")
    return c


@router.post("/import-task")
def import_task(body: dict, user=Depends(_current_user())):
    src = Path(body.get("path", ""))
    tasks_root = task_manager.config.TASKS_DIR.resolve()
    try:
        src.resolve().relative_to(tasks_root)
    except ValueError:
        raise HTTPException(400, "仅允许导入任务目录内的产物")
    if not src.is_file():
        raise HTTPException(404, "产物文件不存在")
    dest = media.import_task_result(user.name, src)
    if dest is None:
        raise HTTPException(400, "不支持的文件类型")
    return {"imported": dest.name, "rel_path": f"{media.classify(dest.name)}/{dest.name}"}


@router.get("/tool-docs")
def tool_docs(user=Depends(_current_user())):
    """全部已注册工具及其参数（与注册表实时同步）。"""
    from open_webui.models.tools import Tools

    docs = []
    for tool in Tools.get_tools():
        specs = tool.specs
        if isinstance(specs, str):
            try:
                specs = json.loads(specs)
            except json.JSONDecodeError:
                specs = []
        functions = []
        for spec in specs or []:
            params = (spec.get("parameters") or {}).get("properties", {}) or {}
            functions.append({
                "function": spec.get("name", ""),
                "description": spec.get("description", ""),
                "parameters": [
                    {"name": n,
                     "type": p.get("type", "string"),
                     "default": p.get("default"),
                     "description": p.get("description", "")}
                    for n, p in params.items()
                ],
            })
        docs.append({"id": tool.id, "name": tool.name, "functions": functions})
    return docs
