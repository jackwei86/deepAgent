"""
title: DeepAgent 撤销/还原
author: DeepAgent
version: 1.0.0
required_open_webui_version: 0.5.0
"""
import os
import sys
from pathlib import Path


def _deepagent_root() -> str:
    # Open WebUI 会把工具内容写入临时文件再 exec(其 __file__ 指向 Temp 目录)，
    # 因此不能单纯依赖 __file__ 回溯项目根。探测顺序：
    #   1) 环境变量 DEEPAGENT_ROOT(start.bat 中设置)
    #   2) 硬编码项目根(存在性校验通过)
    #   3) __file__ 向上回溯(仅当目录中确实存在 assistant 包时采用)
    env = os.environ.get("DEEPAGENT_ROOT")
    if env and (Path(env) / "assistant" / "core" / "tool_impl.py").exists():
        return env
    hardcoded = Path(r"E:\DeepAgent")
    if (hardcoded / "assistant" / "core" / "tool_impl.py").exists():
        return str(hardcoded)
    try:
        p = Path(__file__).resolve()
        for cand in list(p.parents)[:4]:
            if (cand / "assistant" / "core" / "tool_impl.py").exists():
                return str(cand)
    except NameError:
        pass
    return str(hardcoded)


_ROOT = _deepagent_root()
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from assistant.core import tool_impl  # noqa: E402

from pydantic import BaseModel, Field


class Tools:
    def __init__(self):
        self.citation = False

    class Valves(BaseModel):
        webui_base_url: str = Field(default="http://127.0.0.1:8080",
                                     description="Open WebUI 服务地址(用于上传结果文件)")
        llm_provider: str = Field(default="open-webui", description="记录到任务单的解析模型提供方")
        llm_model: str = Field(default="unknown", description="记录到任务单的解析模型名")

    async def restore_checkpoint(
        self,
        task_id: str = "",
        __event_emitter__=None,
        __files__=None,
        __messages__=None,
        __user__=None,
        __request__=None,
    ) -> str:
        """撤销/还原：把处理状态回退到某个历史步骤的结果（不做任何新的处理，内容完全等于该历史结果）。
        当用户说"撤销上一步"、"撤销抠图/美颜"、"还原到美颜那一步"、"回到之前的样子"时调用本工具。
        task_id 填"要还原到的那一步"的任务单编号——编号出现在此前工具返回的"任务单：`编号`"信息中
        （格式如 20260911-105524-a3d6）。还原采用追加式：历史任务与产物保留不删除，
        还原后会生成一个内容等于该历史结果的新任务，后续处理自动基于它继续。
        示例："撤销抠图，还原到美颜那一步"（task_id=美颜那步返回的任务单编号）。

        :param task_id: 要还原到的历史任务单编号（来自之前工具结果中的"任务单"信息）。
        """
        v = getattr(self, "valves", None) or self.Valves()
        if not task_id:
            return ("⚠️ 缺少要还原到的历史任务编号。请从本轮对话此前的工具结果中找到"
                    "\"任务单：`编号`\"，把编号作为 task_id 传入。")
        return await tool_impl.execute_tool_task(
            event_emitter=__event_emitter__, request=__request__, files=__files__,
            messages=__messages__, user=__user__,
            restore_task_id=task_id.strip(),
            task_type="restore", name="撤销还原",
            description=f"撤销后续处理，还原到历史任务 {task_id} 的结果",
            sdk_command="restore", parameters={}, output_format=None,
            input_roles=["source"], base_url=v.webui_base_url,
            llm_label=(v.llm_provider, v.llm_model), progress_label="撤销还原",
        )
