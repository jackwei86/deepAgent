"""素材库核心：多用户目录、扩展名分类、文件解析、树状列表。

目录结构：MEDIA_DIR/<用户名>/image|video/<文件>
用户名经净化后作为一级目录；解析/列出均限定在该用户目录内。
对话中"素材库/收藏夹/当前目录"即指当前用户的 MEDIA_DIR/<用户名>/ 目录。
"""
from __future__ import annotations

import re
from pathlib import Path
from typing import Optional

from . import config

IMAGE_EXTS = {".jpg", ".jpeg", ".png", ".bmp", ".webp", ".gif", ".tif", ".tiff"}
VIDEO_EXTS = {".mp4", ".mov", ".avi", ".mkv", ".webm", ".flv", ".ts", ".wmv", ".m4v"}

_SANITIZE_RE = re.compile(r"[^\w\u4e00-\u9fa5-]+")


def sanitize_username(user_name: str) -> str:
    """净化用户名为安全的目录名（保留中文/字母/数字/-_）。"""
    cleaned = _SANITIZE_RE.sub("_", (user_name or "default").strip())
    return cleaned[:64] or "default"


def user_media_root(user_name: str, *, create: bool = True) -> Path:
    """某用户的素材库根目录 media/<净化用户名>/。"""
    root = config.MEDIA_DIR / sanitize_username(user_name)
    if create:
        (root / "image").mkdir(parents=True, exist_ok=True)
        (root / "video").mkdir(parents=True, exist_ok=True)
    return root


def classify(filename_or_ext: str) -> Optional[str]:
    """按扩展名分类 → "image" / "video" / None(不支持的类型)。"""
    suffix = Path(filename_or_ext).suffix.lower()
    if suffix in IMAGE_EXTS:
        return "image"
    if suffix in VIDEO_EXTS:
        return "video"
    return None


def resolve_in_media(user_name: str, file_name: str) -> Optional[Path]:
    """在用户素材库中解析文件名（大小写不敏感，支持 image/xxx.png 子路径）。

    命中返回绝对路径；未命中返回 None。
    """
    if not file_name:
        return None
    root = user_media_root(user_name, create=False)
    if not root.is_dir():
        return None

    rel = Path(file_name.strip().strip("/\\"))
    if not rel.parts or any(part in ("..", ".") for part in rel.parts):
        return None

    # 精确路径
    exact = root / rel
    if exact.is_file():
        return exact

    # 大小写不敏感：先定位子目录，再匹配文件名
    subdirs = [root] + [d for d in (root / "image", root / "video") if d.is_dir()]
    if len(rel.parts) > 1:
        wanted_dir, wanted_name = rel.parts[0], rel.parts[-1]
        subdirs += [d for d in subdirs[1:] if d.name.lower() == wanted_dir.lower()]
        candidates = [wanted_name]
    else:
        candidates = [rel.name]

    lowered = {c.lower() for c in candidates}
    for d in subdirs:
        if not d.is_dir():
            continue
        for entry in d.iterdir():
            if entry.is_file() and entry.name.lower() in lowered:
                return entry
    return None


def list_tree(user_name: str) -> dict:
    """返回用户素材库树状结构：{"name": 用户名, "children": [image, video], ...}"""
    root = user_media_root(user_name, create=True)

    def dir_node(d: Path) -> dict:
        node = {"name": d.name, "type": "dir", "children": []}
        for entry in sorted(d.iterdir(), key=lambda p: (p.is_file(), p.name.lower())):
            if entry.is_dir():
                node["children"].append(dir_node(entry))
            else:
                kind = classify(entry.name)
                if kind is None:
                    continue
                node["children"].append({
                    "name": entry.name, "type": "file", "kind": kind,
                    "size": entry.stat().st_size,
                    "rel_path": f"{d.name}/{entry.name}" if d != root else entry.name,
                })
        return node

    tree = dir_node(root)
    tree["name"] = sanitize_username(user_name)
    return tree


def safe_user_path(user_name: str, rel_path: str) -> Optional[Path]:
    """把相对路径安全解析到用户素材库内（防目录穿越），不存在返回 None。"""
    root = user_media_root(user_name, create=False).resolve()
    target = (root / rel_path).resolve()
    if root not in target.parents and target != root:
        return None
    return target if target.is_file() else None


def import_task_result(user_name: str, result_path: Path) -> Optional[Path]:
    """把任务产物复制进用户素材库（按扩展名分类），返回目标路径。"""
    kind = classify(result_path.name)
    if kind is None or not result_path.is_file():
        return None
    dest_dir = user_media_root(user_name) / kind
    dest = dest_dir / result_path.name
    stem, suffix, n = result_path.stem, result_path.suffix, 1
    while dest.exists():
        dest = dest_dir / f"{stem}_{n}{suffix}"
        n += 1
    import shutil
    shutil.copy2(result_path, dest)
    return dest
