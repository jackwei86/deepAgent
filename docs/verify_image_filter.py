# -*- coding: utf-8 -*-
"""本地图片过滤 + 工具标记解析 联合验证"""
import sys, asyncio, json, base64
sys.path.insert(0, r"E:\DeepAgent")
from pathlib import Path
from assistant.core import tool_impl
from assistant.functions.deepagent_image_local_filter import Filter

results = []
def check(item, cond, detail=""):
    results.append((item, bool(cond), detail))
    print(("✅" if cond else "❌"), item, detail)

img_bytes = Path(r"E:\DeepAgent\data\webui\uploads\4b09d9d7-1935-43e4-bce2-44d4f7a41cf8_Active.png").read_bytes()
b64 = base64.b64encode(img_bytes).decode()

# 1) 过滤器: 消息内容部件中的 data URL → 截留落盘 + 替换占位
f = Filter()
body = {"model": "deepseek-v4-pro", "files": [
            {"type": "file", "id": "4b09d9d7-1935-43e4-bce2-44d4f7a41cf8",
             "url": "4b09d9d7-1935-43e4-bce2-44d4f7a41cf8", "name": "Active.png",
             "content_type": "image/png"}],
        "messages": [
            {"role": "user", "content": [
                {"type": "text", "text": "对这张图美颜"},
                {"type": "image_url", "image_url": {"url": "data:image/png;base64," + b64}},
            ]},
            {"role": "assistant", "content": "好的"},
        ]}
out_body = f.inlet(body)
check("过滤器: 顶层图片条目移除", len(out_body.get("files", [])) == 0)
c0 = out_body["messages"][0]["content"]
check("过滤器: 消息中无 image_url 残留", not any(isinstance(p, dict) and p.get("type") == "image_url" for p in c0))
marker_texts = [p.get("text", "") for p in c0 if isinstance(p, dict) and "图片已保存: " in p.get("text", "")]
check("过滤器: 占位标记包含本机路径", any(Path(t.split("图片已保存: ")[1].split("，")[0]).is_file() for t in marker_texts),
      marker_texts[:1])

# 2) 工具解析: 用过滤后的消息走 restore-to-origin 与美颜
async def main():
    md = await tool_impl.execute_tool_task(
        event_emitter=None, request=None, files=None, messages=out_body["messages"],
        user={"name": "admin"},
        restore_task_id="", task_type="restore", name="撤销还原到原图",
        description="还原到最开始", sdk_command="restore", parameters={},
        output_format=None, input_roles=["source"])
    check("restore(留空 task_id) 经消息标记还原原图", "✅" in md, md[:100])

    # 美颜: 模拟经过滤后的消息(占位标记) → 工具解析 → 执行
    md2 = await tool_impl.execute_tool_task(
        event_emitter=None, request=None, files=None,
        messages=out_body["messages"], user={"name": "admin"},
        task_type="image_beauty", name="图像美颜", description="标记解析美颜",
        sdk_command="beauty", parameters={"strength": 0.7}, output_format="jpg",
        input_roles=["source"], progress_label="图像美颜")
    check("美颜经标记解析正常执行", "✅" in md2, md2[:100])

asyncio.run(main())

passed = sum(1 for _, ok, _ in results if ok)
print("-" * 50)
print(f"过滤+标记解析联合验证: 通过 {passed}/{len(results)}")
sys.exit(0 if passed == len(results) else 1)
