# -*- coding: utf-8 -*-
"""分析 debug_stream.txt(SSE 原始行) 的模型回复"""
import json

lines = open(r"E:\DeepAgent\data\debug_stream.txt", encoding="utf-8").read().splitlines()
reasoning, contents, tool_names = [], [], []
for line in lines:
    if not line.startswith("data:"):
        continue
    payload = line[5:].strip()
    if payload in ("", "[DONE]"):
        continue
    try:
        obj = json.loads(payload)
    except Exception:
        continue
    for ch in obj.get("choices", []):
        d = ch.get("delta") or {}
        if d.get("reasoning_content"):
            reasoning.append(d["reasoning_content"])
        if d.get("content"):
            contents.append(d["content"])
        for tc in d.get("tool_calls") or []:
            fn = tc.get("function", {})
            tool_names.append(fn.get("name", "?"))

print("思考(前120):", "".join(reasoning)[:120])
print("工具名:", tool_names)
print("回复(前300):")
print("".join(contents)[:300])
