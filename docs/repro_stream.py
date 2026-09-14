# -*- coding: utf-8 -*-
"""复现用户场景：流式聊天(带附件+工具)，并落盘服务日志供分析"""
import sys, json, urllib.request
sys.path.insert(0, r"E:\DeepAgent")

pw = open(r"E:\DeepAgent\data\webui-admin-credentials.txt", encoding="utf-8").read().split("密码: ")[1].splitlines()[0]
req = urllib.request.Request("http://127.0.0.1:8080/api/v1/auths/signin",
    data=json.dumps({"email": "admin", "password": pw}).encode(),
    headers={"Content-Type": "application/json"}, method="POST")
tok = json.loads(urllib.request.urlopen(req, timeout=30).read())["token"]

body = {
    "model": "deepseek-v4-pro",
    "messages": [{"role": "user", "content": "对这张图美颜"}],
    "tool_ids": ["deepagent_image_beauty", "deepagent_image_matting",
                 "deepagent_image_composite", "deepagent_video_beauty", "deepagent_restore"],
    "files": [{"type": "file", "id": "4b09d9d7-1935-43e4-bce2-44d4f7a41cf8",
               "url": "4b09d9d7-1935-43e4-bce2-44d4f7a41cf8", "name": "Active.png"}],
    "stream": True,
}
req = urllib.request.Request("http://127.0.0.1:8080/api/chat/completions",
    data=json.dumps(body).encode(), method="POST")
req.add_header("Authorization", "Bearer " + tok)
req.add_header("Content-Type", "application/json")
resp = urllib.request.urlopen(req, timeout=600)
chunks = []
for raw in resp:
    line = raw.strip()
    if line.startswith(b"data:"):
        chunks.append(line[5:].strip().decode("utf-8", "replace"))
print("SSE 块数:", len(chunks))
data = "\n\n".join(c for c in chunks if c and c != "[DONE]")
open(r"E:\DeepAgent\data\debug_stream.txt", "w", encoding="utf-8").write(data)
print("含 tool_calls:", '"tool_calls"' in data)
print("含 美颜:", "美颜" in data)
print("含 素材/file_name:", "file_name" in data)
