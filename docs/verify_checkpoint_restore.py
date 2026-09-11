# -*- coding: utf-8 -*-
"""检查点还原验证（用户场景）：
  对话命令1: 对一张图 beauty → 生成图 B
  对话命令2: 对图 B matting → 生成 C
  对话命令3: 撤销 matting，还原到 B
验证: checkpoint(task.json+产物) 完好、还原任务内容==B、链 A→B→C→还原、历史不删除
"""
import sys, asyncio, re, filecmp
sys.path.insert(0, r"E:\DeepAgent")
from pathlib import Path
from assistant.core import media, tool_impl, task_manager

results = []
def check(item, cond, detail=""):
    results.append((item, bool(cond), detail))
    print(("✅" if cond else "❌"), item, detail)

def tid_of(md):
    m = re.search(r"任务单：`([^`]+)`", md)
    assert m, md[:160]
    return m.group(1)

async def call_tool(task_type, name, sdk, params, media_path, out_fmt, restore_task_id=None):
    return await tool_impl.execute_tool_task(
        event_emitter=None, request=None, files=None, messages=None,
        user={"name": "admin"},
        media_files=[str(media_path)] if media_path else None,
        restore_task_id=restore_task_id,
        task_type=task_type, name=name, description=f"检查点还原验证-{name}",
        sdk_command=sdk, parameters=params, output_format=out_fmt,
        input_roles=["source"], progress_label=name)

async def main():
    src = media.resolve_in_media("admin", "Active.png")

    # ---- 命令1: beauty 生成 B ----
    md1 = await call_tool("image_beauty", "美颜生成B", "beauty",
                          {"strength": 0.7, "denoise": True, "whiten": 0.0}, src, "jpg")
    tid_b = tid_of(md1)
    b_task = task_manager.load_task(tid_b)
    img_b = Path(b_task.execution["result"]["output_path"])
    check("命令1 美颜完成，生成图 B(检查点)", img_b.is_file(), f"B={tid_b}")

    # ---- 命令2: 对 B matting 生成 C ----
    md2 = await call_tool("image_matting", "抠图生成C", "matting",
                          {"mode": "grabcut", "background": "transparent"}, img_b, "png")
    tid_c = tid_of(md2)
    c_task = task_manager.load_task(tid_c)
    img_c = Path(c_task.execution["result"]["output_path"])
    check("命令2 抠图完成，生成图 C(检查点)", img_c.is_file(), f"C={tid_c}")
    check("C 的 parent 指向 B(链自动登记)", c_task.data["parent_task_id"] == tid_b)

    # ---- 命令3: 撤销 matting，还原到 B ----
    md3 = await call_tool("restore", "撤销还原", "restore", {}, None, None,
                          restore_task_id=tid_b)
    tid_r = tid_of(md3)
    r_task = task_manager.load_task(tid_r)
    img_r = Path(r_task.execution["result"]["output_path"])
    check("命令3 撤销还原执行成功", r_task.execution["status"] == "success" and img_r.is_file(), f"还原任务={tid_r}")
    check("还原内容 == 图 B(逐字节一致)", filecmp.cmp(img_r, img_b, shallow=False))
    check("还原任务 parent=B(链上登记回退点)", r_task.data["parent_task_id"] == tid_b)

    # ---- checkpoint 完整性 ----
    check("检查点 B 完好且未被覆盖", img_b.is_file() and not img_b.samefile(img_r))
    check("检查点 C 保留(历史不删除)", img_c.is_file() and not img_c.samefile(img_r))

    # ---- 版本树终态 ----
    chain = task_manager.get_chain(tid_r)
    parents = {t["task_id"]: t["parent_task_id"] for t in chain["tasks"]}
    check("版本树 4 节点: 美颜→抠图→还原(还原点=B)",
          len(chain["tasks"]) == 3
          and parents[tid_c] == tid_b and parents[tid_r] == tid_b
          and parents[tid_b] is None)

    print("\n=== 版本树终态 ===")
    for t in chain["tasks"]:
        p = t["parent_task_id"]
        note = "  ◄── 撤销还原(内容=图B)" if t["task_id"] == tid_r else ""
        print(f"  第{t['step']}步 {t['name']} [{t['status']}] {t['task_id']}"
              f" ← {p[-4:] if p else '起点(原图)'}{note}")

asyncio.run(main())
passed = sum(1 for _, ok, _ in results if ok)
print("-" * 50)
print(f"检查点还原验证: 通过 {passed}/{len(results)}")
sys.exit(0 if passed == len(results) else 1)
