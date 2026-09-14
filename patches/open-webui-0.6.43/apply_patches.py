# -*- coding: utf-8 -*-
"""open-webui 前端环境级补丁一键应用/还原。

适用版本: open-webui 0.6.43 (pip 重装/升级后重跑本脚本即可重打补丁;
若升级后版本变化, 需先对照 files/ 与新版源文件人工核对再应用)。

用法(在项目根目录):
    .venv\\Scripts\\python.exe patches\\open-webui-0.6.43\\apply_patches.py apply
    .venv\\Scripts\\python.exe patches\\open-webui-0.6.43\\apply_patches.py restore
"""
import shutil
import sys
from pathlib import Path

PATCH_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = PATCH_DIR.parent.parent
FRONTEND_DST = PROJECT_ROOT / ".venv" / "Lib" / "site-packages" / "open_webui" / "frontend"

FILES = [
    "frontend/index.html",
    "frontend/_app/immutable/nodes/43.D1AE-7j5.js",
]


def _dst(rel: str) -> Path:
    return FRONTEND_DST / rel.split("frontend/", 1)[1]


def _orig_src(rel: str) -> Path:
    return PATCH_DIR / "originals" / rel.split("frontend/", 1)[1]


def do_apply() -> None:
    for rel in FILES:
        src = PATCH_DIR / "files" / rel
        dst = _dst(rel)
        bak = dst.with_suffix(dst.suffix + ".bak")
        if not bak.exists() and dst.exists():
            shutil.copy2(dst, bak)  # 首次应用前保留原始文件
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
        print(f"[应用] {rel}")


def do_restore() -> None:
    for rel in FILES:
        dst = _dst(rel)
        bak = dst.with_suffix(dst.suffix + ".bak")
        if bak.exists():
            shutil.copy2(bak, dst)
            print(f"[还原] {rel}")
        else:
            print(f"[跳过] {rel} (无原始备份)")


def main() -> None:
    action = sys.argv[1] if len(sys.argv) > 1 else ""
    if action == "apply":
        do_apply()
        print("\n完成。请刷新浏览器(Ctrl+F5)加载补丁后的前端资源。")
    elif action == "restore":
        do_restore()
        print("\n已还原为 open-webui 原始文件。")
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
