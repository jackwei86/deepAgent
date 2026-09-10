"""
title: DeepAgent 图像合成
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
        timeout_seconds: int = Field(default=1800, description="任务超时(秒)")
        webui_base_url: str = Field(default="http://127.0.0.1:8080",
                                     description="Open WebUI 服务地址(用于上传结果文件)")
        llm_provider: str = Field(default="open-webui", description="记录到任务单的解析模型提供方")
        llm_model: str = Field(default="unknown", description="记录到任务单的解析模型名")

    async def image_composite(
        self,
        x: int = -1,
        y: int = -1,
        scale: float = 1.0,
        __event_emitter__=None,
        __files__=None,
        __messages__=None,
        __request__=None,
    ) -> str:
        """把第二张图片（前景，可为抠图产物等带透明的PNG）合成到第一张图片（底图）上。
        重要：本工具会自动从用户消息的附件中按顺序取两张图片（第1张=底图，第2张=前景）——
        即使你无法直接查看图片内容，只要用户表达了把一张图贴/合成到另一张图的意图，
        就必须立即调用本工具。不要以"没有看到图片"为由拒绝；若附件不足，工具会返回明确的提示。

        :param x: 前景贴到底图上的左上角X坐标(像素)。传 -1 表示自动居中（默认）。
        :param y: 前景左上角Y坐标(像素)。传 -1 表示自动居中（默认）。
        :param scale: 前景缩放倍数，默认 1.0；大于1放大，小于1缩小。
        """
        v = getattr(self, "valves", None) or self.Valves()
        return await tool_impl.execute_tool_task(
            event_emitter=__event_emitter__, request=__request__, files=__files__,
            messages=__messages__,
            task_type="image_composite", name="图像合成",
            description=(f"将前景合成到底图，位置({x},{y}) 缩放{scale}，"
                         + ("自动居中" if x < 0 or y < 0 else "指定坐标")),
            sdk_command="composite",
            parameters={"x": x, "y": y, "scale": scale, "center": x < 0 or y < 0},
            output_format="jpg", input_roles=["source", "foreground"],
            timeout_s=v.timeout_seconds, base_url=v.webui_base_url,
            llm_label=(v.llm_provider, v.llm_model), progress_label="图像合成",
        )
