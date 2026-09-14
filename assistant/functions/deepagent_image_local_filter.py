"""
title: DeepAgent 本地图片过滤
author: DeepAgent
version: 1.1.0
required_open_webui_version: 0.5.0
"""
import base64
import hashlib
import os
import shutil
from pathlib import Path


def _upload_dir() -> Path:
    root = os.environ.get("DEEPAGENT_ROOT") or r"E:\DeepAgent"
    d = Path(root) / "data" / "webui" / "uploads"
    d.mkdir(parents=True, exist_ok=True)
    return d


def _save_data_url(url: str) -> str:
    """把 data:image/...;base64 内联图片解码落盘，返回绝对路径。"""
    head, b64 = url.split(";base64,", 1)
    ext = {"image/jpeg": ".jpg", "image/png": ".png", "image/bmp": ".bmp",
           "image/webp": ".webp", "image/gif": ".gif"}.get(head[5:], ".png")
    digest = hashlib.md5(b64.encode("ascii")).hexdigest()[:8]
    out = _upload_dir() / f"inline_{digest}{ext}"
    if not out.exists():
        out.write_bytes(base64.b64decode(b64))
    return str(out)


def _copy_file_by_id(file_id: str) -> str | None:
    """把 open-webui 文件库中已注册的图片复制到 uploads 平铺目录，返回路径。"""
    try:
        from open_webui.models.files import Files
        from open_webui.storage import Storage
        f = Files.get_file_by_id(file_id)
        if not f:
            return None
        local = Path(Storage.get_file(f.path))
        if not local.is_file():
            return None
        out = _upload_dir() / f"{file_id}_{f.filename}"
        if not out.exists():
            out.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(local, out)
        return str(out)
    except Exception:
        return None


def _find_upload_by_prefix(prefix: str) -> str | None:
    d = _upload_dir()
    if d.is_dir() and prefix:
        for entry in d.iterdir():
            if entry.is_file() and entry.name.startswith(prefix):
                return str(entry)
    return None


IMAGE_EXTS = (".jpg", ".jpeg", ".png", ".bmp", ".webp", ".gif")


def _is_image_entry(entry: dict) -> bool:
    name = str(entry.get("name") or entry.get("filename") or "").lower()
    ctype = str(entry.get("content_type") or (entry.get("meta") or {}).get("content_type") or "")
    if ctype.startswith("image/"):
        return True
    return name.endswith(IMAGE_EXTS)


class Filter:
    def __init__(self):
        pass

    def inlet(self, body: dict, __user__: dict = None) -> dict:
        """图片不出本机：把请求中的图片(消息内容部件/顶层 files 元数据)截留保存到本机
        (供本地 C++ 工具使用)，发给 LLM 的只有文字占位。
        模型名含 vision 时跳过(视觉模型需要真实图片)。"""
        model = (body.get("model") or "")
        if "vision" in model:
            return body

        markers: list[str] = []

        # ---- 1) 顶层 files 元数据中的图片 ----
        files = body.get("files")
        if isinstance(files, list):
            kept = []
            for entry in files:
                if not isinstance(entry, dict):
                    kept.append(entry)
                    continue
                if not _is_image_entry(entry):
                    kept.append(entry)
                    continue
                fid = str(entry.get("id") or entry.get("url") or "")
                name = str(entry.get("name") or "")
                saved = None
                try:
                    url = str(entry.get("url") or "")
                    if url.startswith("data:image") and ";base64," in url:
                        saved = _save_data_url(url)
                    elif fid:
                        saved = _find_upload_by_prefix(fid)
                except Exception:
                    saved = None
                if saved:
                    markers.append(f"[用户上传的图片已保存: {saved}，将由本地工具处理]")
                # 图片条目从 files 中移除(不发给 LLM)
            body["files"] = kept

        # ---- 2) 消息内容部件中的图片(data URL / 文件引用) ----
        for msg in body.get("messages", []):
            if not isinstance(msg, dict) or msg.get("role") != "user":
                continue
            content = msg.get("content")
            if not isinstance(content, list):
                continue
            for i, part in enumerate(content):
                if not isinstance(part, dict) or part.get("type") != "image_url":
                    continue
                url = (part.get("image_url") or {}).get("url", "")
                saved = None
                try:
                    if url.startswith("data:image") and ";base64," in url:
                        saved = _save_data_url(url)
                    elif "/api/v1/files/" in url:
                        fid = url.split("/api/v1/files/")[1].split("/")[0].split("?")[0]
                        saved = _find_upload_by_prefix(fid)
                except Exception:
                    saved = None
                if saved:
                    content[i] = {"type": "text",
                                  "text": f"[用户上传的图片已保存: {saved}，将由本地工具处理]"}
                    markers.append(f"[用户上传的图片已保存: {saved}]")

        # ---- 3) 汇总占位: 在最后一条用户消息中追加文字标记(供模型知悉与工具解析) ----
        if markers:
            for msg in reversed(body.get("messages", [])):
                if isinstance(msg, dict) and msg.get("role") == "user":
                    c = msg.get("content")
                    if isinstance(c, str):
                        msg["content"] = [{"type": "text", "text": c},
                                          {"type": "text", "text": " ".join(markers)}]
                    elif isinstance(c, list):
                        c.append({"type": "text", "text": " ".join(markers)})
                    break

        if markers:
            body["deepagent_images_saved"] = len(markers)
        return body
