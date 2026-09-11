"""Open WebUI Tools 共享实现：素材定位、任务单创建、执行、进度推送、结果预览。

每个 Tool 文件(assistant/tools/*.py)是对本模块的薄封装；
本模块在 Open WebUI 服务进程内通过 sys.path 注入被导入。
"""
from __future__ import annotations

import asyncio
import base64
import json
import sys
from pathlib import Path
from typing import Any, Optional

_ASSISTANT_DIR = Path(__file__).resolve().parents[1]
if str(_ASSISTANT_DIR) not in sys.path:
    sys.path.insert(0, str(_ASSISTANT_DIR))

from core import config, sdk_runner, task_manager  # noqa: E402
from core import media as media_lib  # noqa: E402

_MAX_INLINE_PREVIEW_BYTES = 3 * 1024 * 1024  # base64 内联预览上限


# ---------------------------------------------------------------------------
# Open WebUI 消息附件定位
# ---------------------------------------------------------------------------
def resolve_message_files(files: Any) -> list[dict]:
    """把 __files__(消息附件元数据) 解析为本地路径。

    Open WebUI 0.6.x 上传文件平铺存储为 <DATA_DIR>/uploads/<file_id>_<filename>；
    旧版本为 <DATA_DIR>/uploads/<file_id>/<filename>。附件元数据结构随版本
    差异较大(顶层 id/url/name 或嵌套 file 对象)，这里做多级回退解析。
    """
    uploads = config.WEBUI_UPLOADS_DIR
    resolved: list[dict] = []
    for f in files or []:
        if not isinstance(f, dict):
            continue
        inner = f.get("file") if isinstance(f.get("file"), dict) else {}
        fid = str(f.get("id") or f.get("file_id") or inner.get("id") or "")
        name = str(f.get("name") or f.get("filename") or inner.get("filename")
                   or inner.get("meta", {}).get("name") or "")
        url = str(f.get("url") or "")
        local: Optional[Path] = None

        # 0) 内联 data URL(前端可能把图片直接内联进请求): 解码落盘后使用
        if url.startswith("data:") and ";base64," in url:
            local = _materialize_data_url(url, name, uploads)
            fid = fid or local.stem

        # 1) 附件元数据直接给出可用的本地路径
        if local is None:
            for cand in (f.get("path"), inner.get("path"), url):
                if not cand:
                    continue
                p = Path(str(cand))
                if p.is_file():
                    local = p
                    break
                # "/api/v1/files/<id>/content" 形式 → 提取 id
                if "/api/v1/files/" in str(cand):
                    fid = fid or str(cand).split("/api/v1/files/")[1].split("/")[0]

        # 2) 平铺存储: uploads/<file_id>_<filename>
        if local is None and fid and uploads.is_dir():
            for entry in uploads.iterdir():
                if entry.is_file() and entry.name.startswith(fid):
                    local = entry
                    break

        # 3) 目录存储(旧版本): uploads/<file_id>/<filename>
        if local is None and fid:
            d = uploads / fid
            if d.is_dir():
                exact = d / name if name else None
                if exact and exact.is_file():
                    local = exact
                else:
                    inner_files = sorted(x for x in d.iterdir() if x.is_file())
                    if inner_files:
                        local = inner_files[0]

        if local is None:
            # 诊断: 记录未解析附件的原始结构(便于排查前端载荷差异)
            _debug_dump_unresolved(f)
            continue
        resolved.append({"id": fid or local.stem, "name": name or local.name,
                         "path": str(local)})
    return resolved


def _extract_from_message_content(messages: Optional[list]) -> list[dict]:
    """最后一级回退：从用户消息的 content 内容部件(image_url)中提取图片。

    Open WebUI 发给模型前可能把附件转换为内容部件并剥离 files 字段。
    支持 data: 内联与 /api/v1/files/<id>/content 两种 URL。
    """
    resolved: list[dict] = []
    for msg in reversed(messages or []):
        if not isinstance(msg, dict) or msg.get("role") != "user":
            continue
        content = msg.get("content")
        if not isinstance(content, list):
            continue
        for part in content:
            if not isinstance(part, dict) or part.get("type") != "image_url":
                continue
            url = (part.get("image_url") or {}).get("url", "")
            if not url:
                continue
            resolved.extend(resolve_message_files([{"url": url}]))
        if resolved:
            break
    return resolved


def _log_payload(files: Any, messages: Optional[list], resolved: list) -> None:
    """记录工具实际收到的载荷摘要(诊断用)，追加到 data/debug_tool_payload.log。"""
    try:
        import json as _json
        import time
        msgs = messages or []
        last_user = next((m for m in reversed(msgs)
                          if isinstance(m, dict) and m.get("role") == "user"), {}) or {}
        summary = {
            "time": time.strftime("%Y-%m-%d %H:%M:%S"),
            "top_files": len(files) if isinstance(files, list) else str(type(files)),
            "top_files_sample": (files[:1] if isinstance(files, list) and files else None),
            "messages_count": len(msgs),
            "last_user_keys": sorted(last_user.keys()) if last_user else [],
            "last_user_files": len(last_user.get("files") or []),
            "last_user_content_type": type(last_user.get("content")).__name__,
            "resolved": len(resolved),
        }
        dbg = config.DATA_DIR / "debug_tool_payload.log"
        with dbg.open("a", encoding="utf-8") as fh:
            fh.write(_json.dumps(summary, ensure_ascii=False, default=str)[:1500] + "\n")
    except Exception:  # noqa: BLE001
        pass


def _materialize_data_url(url: str, name: str, uploads: Path) -> Path:
    """把 data:<mime>;base64,<...> 解码写入 uploads 目录并返回路径。"""
    import base64
    import hashlib
    head, b64 = url.split(";base64,", 1)
    ext = {"image/jpeg": ".jpg", "image/png": ".png", "image/bmp": ".bmp",
           "image/webp": ".webp", "video/mp4": ".mp4"}.get(head[5:], ".bin")
    stem = Path(name).stem if name else "inline"
    safe_stem = "".join(c for c in stem if c.isalnum() or c in "-_")[:40] or "inline"
    digest = hashlib.md5(b64.encode("ascii")).hexdigest()[:8]
    out = uploads / f"inline_{safe_stem}_{digest}{ext}"
    if not out.exists():
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(base64.b64decode(b64))
    return out


def _debug_dump_unresolved(entry: Any) -> None:
    try:
        import json as _json
        import time
        dbg = config.DATA_DIR / "debug_unresolved_files.log"
        with dbg.open("a", encoding="utf-8") as fh:
            fh.write(time.strftime("[%Y-%m-%d %H:%M:%S] ") +
                     _json.dumps(entry, ensure_ascii=False, default=str)[:2000] + "\n")
    except Exception:  # noqa: BLE001 - 诊断日志失败不影响主流程
        pass


# ---------------------------------------------------------------------------
# 进度/通知
# ---------------------------------------------------------------------------
async def emit_status(event_emitter, description: str, *, done: bool = False,
                      error: bool = False) -> None:
    if not event_emitter:
        return
    payload = {"type": "status", "data": {"description": description, "done": done,
                                          "error": error}}
    try:
        result = event_emitter(payload)
        if asyncio.iscoroutine(result):
            await result
    except Exception:  # noqa: BLE001 - 事件失败不影响任务执行
        pass


def _thread_progress_emitter(event_emitter, loop, label: str):
    """工作线程内调用的同步进度回调 → 线程安全地转为 async 事件。"""
    def cb(percent: int, stage: str) -> None:
        if not event_emitter or not loop:
            return
        data = {"type": "status", "data": {
            "description": f"{label} {percent}%｜{stage}", "done": False}}
        try:
            asyncio.run_coroutine_threadsafe(_emit(event_emitter, data), loop)
        except RuntimeError:
            pass
    return cb


async def _emit(event_emitter, data: dict) -> None:
    result = event_emitter(data)
    if asyncio.iscoroutine(result):
        await result


# ---------------------------------------------------------------------------
# 结果上传与预览
# ---------------------------------------------------------------------------
def _auth_token(request: Any) -> str:
    """从聊天请求头提取 JWT(用于带 token 的文件直链)。"""
    try:
        auth = request.headers.get("authorization") or ""
        return auth[7:] if auth.lower().startswith("bearer ") else ""
    except Exception:  # noqa: BLE001
        return ""


async def upload_to_webui(request: Any, base_url: str, result_path: Path,
                          filename: str) -> Optional[dict]:
    """通过 Open WebUI 文件 API 上传产物，返回 {id,...}；失败返回 None。"""
    try:
        import httpx
        headers = {}
        token = _auth_token(request)
        if token:
            headers["Authorization"] = f"Bearer {token}"
        data = {"profile": json.dumps({"name": filename})}
        async with httpx.AsyncClient(timeout=120) as client:
            with result_path.open("rb") as fh:
                r = await client.post(
                    f"{base_url.rstrip('/')}/api/v1/files/",
                    headers=headers, data=data,
                    files={"file": (filename, fh, _guess_mime(filename))},
                )
            r.raise_for_status()
            return r.json()
    except Exception:  # noqa: BLE001
        return None


def _guess_mime(name: str) -> str:
    ext = Path(name).suffix.lstrip(".").lower()
    return {"jpg": "image/jpeg", "jpeg": "image/jpeg", "png": "image/png",
            "mp4": "video/mp4"}.get(ext, "application/octet-stream")


def build_result_markdown(result_path: Path, task_id: str, file_meta: Optional[dict],
                          base_url: str, token: str) -> str:
    """构造聊天回复的文本摘要。

    图片/视频的预览由 files 事件挂到消息上(见 execute_tool_task)，
    这里只给模型简短的结构化信息，避免 base64 大块内容进入 LLM 上下文。
    """
    size_kb = max(1, result_path.stat().st_size // 1024)
    lines = [f"- 产物文件：`{result_path.name}`（{size_kb} KB）"]
    if file_meta and file_meta.get("id"):
        lines.append(f"- 在线查看/下载：/api/v1/files/{file_meta['id']}/content")
    lines.append(f"- 任务单：`{task_id}`（{config.TASKS_DIR / task_id / 'task.json'}）")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# 主入口：创建任务单 → 命令行执行 → 进度推送 → 结果预览
# ---------------------------------------------------------------------------
async def execute_tool_task(
    *,
    event_emitter,
    request,
    files,
    task_type: str,
    name: str,
    description: str,
    sdk_command: str,
    parameters: dict,
    output_format: Optional[str] = None,
    input_roles: Optional[list[str]] = None,
    timeout_s: int = 1800,
    base_url: str = "http://127.0.0.1:8080",
    llm_label: tuple[str, str] = ("open-webui", "unknown"),
    progress_label: str = "处理中",
    messages: Optional[list] = None,
    media_files: Optional[list[str]] = None,
    user: Optional[dict] = None,
) -> str:
    """Tool 共用执行流程。input_roles 指定各附件的角色(如 ["source","foreground"])。

    素材来源优先级：
      1. 聊天附件(顶层 __files__ → 消息内 files → content 的 image_url 部件)
      2. 素材库 file_name("素材库/收藏夹/当前目录"即 media/<用户名>/ 目录)
    """
    resolved = resolve_message_files(files)
    if not resolved:
        for msg in reversed(messages or []):
            if not isinstance(msg, dict):
                continue
            msg_files = msg.get("files")
            if not msg_files:
                continue
            resolved = resolve_message_files(msg_files)
            if resolved:
                break
    if not resolved:
        resolved = _extract_from_message_content(messages)
    _log_payload(files, messages, resolved)
    roles = input_roles or ["source"] * max(1, len(resolved))

    # 素材库回退: LLM 从"素材库/收藏夹/当前目录"等话语中提取的文件名
    if not resolved and media_files:
        user_name = (user or {}).get("name") or "default"
        for mf in media_files:
            if not mf:
                continue
            p = media_lib.resolve_in_media(user_name, mf)
            if p is None:
                await emit_status(event_emitter, f"素材库中未找到 {mf}", done=True, error=True)
                return (f"⚠️ 素材库(media/{media_lib.sanitize_username(user_name)}/)中"
                        f"未找到「{mf}」。请确认文件名，或先把文件上传/复制到素材库的"
                        f"image(图片)/video(视频)子目录。")
            resolved.append({"id": p.stem, "name": p.name, "path": str(p)})

    if not resolved:
        return ("⚠️ 未在消息中找到素材文件。可以：①直接上传图片/视频后发送；"
                "或 ②把文件放入素材库后说\"处理素材库里的 <文件名>\"。")
    if len(resolved) < len(roles):
        return f"⚠️ 该任务需要 {len(roles)} 个素材文件(角色: {', '.join(roles)})，当前消息只有 {len(resolved)} 个。"

    # 任务链推断: 素材来自某任务产物(data/tasks/<task_id>/)时，登记 parent/pipeline
    parent_task_id: Optional[str] = None
    pipeline_id: Optional[str] = None
    tasks_root = config.TASKS_DIR.resolve()
    for r in resolved:
        try:
            rp = Path(r["path"]).resolve()
        except OSError:
            continue
        if tasks_root == rp or tasks_root not in rp.parents:
            continue
        tid_dir = rp.relative_to(tasks_root).parts[0]
        parent = task_manager.load_task(tid_dir)
        if parent is not None:
            parent_task_id = tid_dir
            pipeline_id = parent.data.get("pipeline_id") or tid_dir
            break

    inputs = [
        task_manager.make_input_entry(r["path"], role=role, file_id=r["id"],
                                      filename=r["name"])
        for r, role in zip(resolved, roles)
    ]

    await emit_status(event_emitter, f"已识别任务「{name}」，正在生成任务单…")

    origin = {
        "input_type": "text",  # Open WebUI 未透传语音/文字标记，默认 text
        "user_text": description,
        "llm": {"provider": llm_label[0], "model": llm_label[1]},
    }
    order = task_manager.create_task(
        origin=origin, task_type=task_type, name=name, description=description,
        sdk_command=sdk_command, parameters=parameters, inputs=inputs,
        output_format=output_format,
        parent_task_id=parent_task_id, pipeline_id=pipeline_id,
    )

    loop = asyncio.get_running_loop()
    on_progress = _thread_progress_emitter(event_emitter, loop, progress_label)

    try:
        execution = await asyncio.to_thread(
            sdk_runner.run_task, order, on_progress=on_progress, timeout_s=timeout_s)
    except sdk_runner.RunError as e:
        await emit_status(event_emitter, f"启动失败：{e}", done=True, error=True)
        return f"❌ 任务启动失败：{e}\n\n任务单已保存：`{order.task_file}`"

    if execution["status"] != "success":
        await emit_status(event_emitter, f"任务失败：{execution.get('error')}",
                          done=True, error=True)
        return (f"❌ 任务执行失败：{execution.get('error') or '未知错误'}\n\n"
                f"任务单：`{order.task_file}`（可查看 execution.logs 排查）")

    result_path = Path(execution["result"]["output_path"])
    elapsed = execution.get("elapsed_ms") or 0
    await emit_status(event_emitter, f"完成，耗时 {elapsed/1000:.1f}s", done=True)

    filename = f"{order.task_id}_{result_path.name}"
    file_meta = await upload_to_webui(request, base_url, result_path, filename)

    # 通过 files 事件把产物挂到助手消息上，UI 原生渲染预览(图片内联/视频播放)
    if file_meta and file_meta.get("id"):
        mime = _guess_mime(result_path.name)
        ftype = "image" if mime.startswith("image/") else "file"
        await _emit(event_emitter, {
            "type": "files",
            "data": {"files": [{
                "type": ftype,
                "id": file_meta["id"],
                "url": f"/api/v1/files/{file_meta['id']}/content",
                "name": filename,
                "mime": mime,
            }]},
        })

    markdown = build_result_markdown(result_path, order.task_id, file_meta,
                                     base_url, _auth_token(request))
    return f"✅ **{name}** 完成（{elapsed/1000:.1f}s），结果文件已附在本条消息中。\n\n{markdown}"
