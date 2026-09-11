# -*- coding: utf-8 -*-
"""一期修改逐项验证脚本（任务链 + 版本树）"""
import sys, json, asyncio, re, shutil
sys.path.insert(0, r"E:\DeepAgent")
from pathlib import Path

from assistant.core import media, config, task_manager, tool_impl

results = []
def check(item, cond, detail=""):
    results.append((item, bool(cond), detail))
    print(("✅" if cond else "❌"), item, detail)

# ---------- 第1项: task_manager 链字段 + get_chain ----------
entry = task_manager.make_input_entry(r"E:\DeepAgent\samples\test_image.jpg", role="source")
root = task_manager.create_task(
    origin={"user_text": "A"}, task_type="image_beauty", name="A-美颜",
    description="链头", sdk_command="beauty", parameters={}, inputs=[entry],
    output_format="jpg")
child = task_manager.create_task(
    origin={"user_text": "B"}, task_type="image_matting", name="B-抠图",
    description="链子", sdk_command="matting", parameters={}, inputs=[entry],
    output_format="png", parent_task_id=root.task_id, pipeline_id=root.task_id)
check("1a 任务单含 parent/pipeline 且 schema 1.1",
      child.data["parent_task_id"] == root.task_id
      and child.data["pipeline_id"] == root.task_id
      and child.data["schema_version"] == "1.1")
chain = task_manager.get_chain(child.task_id)
check("1b get_chain 链序正确",
      chain and [t["task_id"] for t in chain["tasks"]] == [root.task_id, child.task_id]
      and [t["step"] for t in chain["tasks"]] == [1, 2])
check("1c get_chain 未知任务返回 None", task_manager.get_chain("不存在") is None)

# ---------- 第2项: tool_impl 自动推断 parent/pipeline ----------
async def call_tool(task_type, name, sdk, params, media_path, out_fmt):
    return await tool_impl.execute_tool_task(
        event_emitter=None, request=None, files=None, messages=None,
        user={"name": "admin"}, media_files=[str(media_path)],
        task_type=task_type, name=name, description=f"链验证-{name}",
        sdk_command=sdk, parameters=params, output_format=out_fmt,
        input_roles=["source"], progress_label=name)

async def e2e():
    src = media.resolve_in_media("admin", "Active.png")
    md_a = await call_tool("image_beauty", "图像美颜", "beauty",
                           {"strength": 0.7, "denoise": True, "whiten": 0.0}, src, "jpg")
    Path(r"E:\DeepAgent\data\debug_md.txt").write_text(md_a, encoding="utf-8", newline="")
    m = re.search(r"任务单：`([^`]+)`", md_a)
    check("2a A 任务执行且 md 可提取 task_id", bool(m), repr(md_a[:80]))
    if not m:
        return
    a_tid = m.group(1)
    a_result = Path(task_manager.load_task(a_tid).execution["result"]["output_path"])
    check("2b A 产物文件存在", a_result.is_file())

    md_b = await call_tool("image_matting", "图像抠图", "matting",
                           {"mode": "grabcut", "background": "transparent"}, a_result, "png")
    m2 = re.search(r"任务单：`([^`]+)`", md_b)
    check("2c B 任务(以A产物为素材)执行成功", bool(m2), repr(md_b[:80]))
    if not m2:
        return
    b_tid = m2.group(1)
    b = task_manager.load_task(b_tid)
    check("2d B 自动登记 parent=A", b.data["parent_task_id"] == a_tid,
          f"parent={b.data['parent_task_id']}, A={a_tid}")
    check("2e B 继承 pipeline=A", b.data["pipeline_id"] == a_tid)
    chain = task_manager.get_chain(b_tid)
    check("2f 链序 [A,B] 步号 [1,2]",
          [t["task_id"] for t in chain["tasks"]] == [a_tid, b_tid]
          and [t["step"] for t in chain["tasks"]] == [1, 2])

asyncio.run(e2e())

# ---------- 回归: 现有素材库流程不受影响 ----------
async def regression():
    src = media.resolve_in_media("admin", "Active.png")
    md = await call_tool("image_beauty", "图像美颜", "beauty",
                         {"strength": 0.7, "denoise": True, "whiten": 0.0}, src, "jpg")
    check("回归 素材库直引流程正常", "✅" in md)
asyncio.run(regression())

passed = sum(1 for _, ok, _ in results if ok)
print("-" * 60)
print(f"一期验证: 通过 {passed}/{len(results)}")
sys.exit(0 if passed == len(results) else 1)
