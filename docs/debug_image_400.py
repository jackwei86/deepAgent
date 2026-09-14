# -*- coding: utf-8 -*-
"""验证 DeepSeek 对不同大小图片 data URL 的响应(400 边界)"""
import json, sqlite3, base64, urllib.request

db = sqlite3.connect(r"E:\DeepAgent\data\webui\webui.db")
key = json.loads(db.execute("SELECT data FROM config WHERE id='1'").fetchone()[0])["openai"]["api_keys"][1]

def probe(label, img_bytes):
    b64 = base64.b64encode(img_bytes).decode()
    body = {"model": "deepseek-v4-pro", "max_tokens": 10, "messages": [{"role": "user", "content": [
        {"type": "text", "text": "美颜"},
        {"type": "image_url", "image_url": {"url": "data:image/png;base64," + b64}},
    ]}]}
    req = urllib.request.Request("https://api.deepseek.com/v1/chat/completions",
        data=json.dumps(body).encode(),
        headers={"Authorization": "Bearer " + key, "Content-Type": "application/json"})
    try:
        resp = urllib.request.urlopen(req, timeout=60)
        d = json.loads(resp.read())
        c = d["choices"][0]["message"].get("content") or ""
        print(f"{label}({len(img_bytes)//1024}KB) -> HTTP 200 | 回复: {c[:60]}")
    except urllib.error.HTTPError as e:
        print(f"{label}({len(img_bytes)//1024}KB) -> HTTP {e.code} | {e.read().decode('utf-8','replace')[:100]}")

import pathlib
big = pathlib.Path(r"E:\DeepAgent\data\webui\uploads\4b09d9d7-1935-43e4-bce2-44d4f7a41cf8_Active.png").read_bytes()
small = pathlib.Path(r"E:\DeepAgent\samples\test_image.jpg").read_bytes()
probe("大图PNG", big)
probe("小图JPG", small)
