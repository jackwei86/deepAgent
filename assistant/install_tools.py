"""向 Open WebUI 批量注册/更新 DeepAgent 工具(assistant/tools/*.py)。

用法(在 venv 中)：
    python install_tools.py --url http://127.0.0.1:8080 --token <管理员API密钥>

token 获取：Open WebUI → 设置 → 账号 → API 密钥；
或设置环境变量 DEEPAGENT_WEBUI_TOKEN。
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
import urllib.error
import urllib.request
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent / "tools"


def parse_frontmatter(content: str) -> dict:
    """提取工具文件的 docstring frontmatter(title/version 等)。"""
    meta: dict = {}
    m = re.match(r'^\s*"""(.*?)"""', content, re.S)
    if m:
        for line in m.group(1).strip().splitlines():
            if ":" in line:
                k, _, v = line.partition(":")
                meta[k.strip()] = v.strip()
    return meta


def api(url: str, token: str, method: str, payload: dict | None = None) -> tuple[int, dict]:
    data = json.dumps(payload).encode() if payload is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("Authorization", f"Bearer {token}")
    req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            return resp.status, json.loads(resp.read().decode() or "{}")
    except urllib.error.HTTPError as e:
        body = e.read().decode(errors="replace")
        try:
            return e.code, json.loads(body)
        except json.JSONDecodeError:
            return e.code, {"detail": body[:300]}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default=os.environ.get("DEEPAGENT_WEBUI_URL", "http://127.0.0.1:8080"))
    ap.add_argument("--token", default=os.environ.get("DEEPAGENT_WEBUI_TOKEN", ""))
    ap.add_argument("--tools-dir", default=str(TOOLS_DIR))
    args = ap.parse_args()
    if not args.token:
        print("缺少 --token(或环境变量 DEEPAGENT_WEBUI_TOKEN)", file=sys.stderr)
        return 2

    files = sorted(Path(args.tools_dir).glob("deepagent_*.py"))
    if not files:
        print(f"未找到工具文件: {args.tools_dir}", file=sys.stderr)
        return 2

    # 已有工具列表(用于判断 create 还是 update)
    status, existing = api(f"{args.url.rstrip('/')}/api/v1/tools/", args.token, "GET")
    if status != 200:
        print(f"获取工具列表失败 HTTP {status}: {existing}", file=sys.stderr)
        return 1
    existing_ids = {t.get("id") for t in existing}

    ok = 0
    for f in files:
        content = f.read_text(encoding="utf-8")
        meta = parse_frontmatter(content)
        tool_id = f.stem
        payload = {
            "id": tool_id,
            "name": meta.get("title", tool_id),
            "content": content,
            "meta": {k: v for k, v in meta.items()},
            "access_control": None,
        }
        if tool_id in existing_ids:
            # 已存在: 走 POST /api/v1/tools/id/<id>/update 更新
            code, resp = api(f"{args.url.rstrip('/')}/api/v1/tools/id/{tool_id}/update",
                             args.token, "POST", payload)
        else:
            code, resp = api(f"{args.url.rstrip('/')}/api/v1/tools/create",
                             args.token, "POST", payload)
        mark = "OK " if code in (200, 201) else "ERR"
        print(f"[{mark}] {tool_id} HTTP {code} {'' if code in (200, 201) else resp}")
        ok += 1 if code in (200, 201) else 0
    print(f"完成: {ok}/{len(files)} 个工具注册成功")
    return 0 if ok == len(files) else 1


if __name__ == "__main__":
    sys.exit(main())
