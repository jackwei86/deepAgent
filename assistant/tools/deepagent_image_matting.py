"""
title: DeepAgent 图像抠图
author: DeepAgent
version: 1.1.0
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

    async def image_matting(
        self,
        mode: str = "grabcut",
        background: str = "transparent",
        file_name: str = "",
        __event_emitter__=None,
        __files__=None,
        __messages__=None,
        __user__=None,
        __request__=None,
    ) -> str:
        """对图片执行抠图（前景提取），返回抠图结果。
        素材来源（二选一）：① 用户消息中附带的图片；② 素材库文件。
        重要："素材库"、"收藏夹"、"当前目录"指当前用户的素材库目录——
        当用户提到这些词并给出文件名时，必须把文件名传入 file_name 参数直接调用，
        不要要求用户重新上传；若素材库和附件中都没有该文件，工具会返回明确提示。
        示例："把这张图抠出来"、"抠素材库里的 photo.png，换白底"。

        :param mode: 抠图模式。"grabcut"=通用场景智能分割（默认，取画面中央主体）；
                     "chroma"=绿幕键控（适合纯绿背景素材）。
        :param background: 抠出前景的背景："transparent"=透明PNG（默认）、"white"=白底、"green"=绿底。
        :param file_name: 素材库文件名（如 photo.png 或 image/photo.png）。消息已附带图片时留空。
        """
        v = getattr(self, "valves", None) or self.Valves()
        if mode not in ("grabcut", "chroma"):
            mode = "grabcut"
        if background not in ("transparent", "white", "green"):
            background = "transparent"
        return await tool_impl.execute_tool_task(
            event_emitter=__event_emitter__, request=__request__, files=__files__,
            messages=__messages__, user=__user__,
            media_files=[file_name] if file_name else None,
            task_type="image_matting", name="图像抠图",
            description=f"对图片抠取前景，模式 {mode}，背景 {background}",
            sdk_command="matting",
            parameters={"mode": mode, "background": background},
            output_format="png", input_roles=["source"],
            timeout_s=v.timeout_seconds, base_url=v.webui_base_url,
            llm_label=(v.llm_provider, v.llm_model), progress_label="图像抠图",
        )
