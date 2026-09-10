"""
title: DeepAgent 视频美颜
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
        timeout_seconds: int = Field(default=3600, description="任务超时(秒)，视频任务较耗时")
        webui_base_url: str = Field(default="http://127.0.0.1:8080",
                                     description="Open WebUI 服务地址(用于上传结果文件)")
        llm_provider: str = Field(default="open-webui", description="记录到任务单的解析模型提供方")
        llm_model: str = Field(default="unknown", description="记录到任务单的解析模型名")

    async def video_beauty(
        self,
        strength: float = 0.7,
        whiten: float = 0.0,
        __event_emitter__=None,
        __files__=None,
        __messages__=None,
        __request__=None,
    ) -> str:
        """对用户上传的视频执行逐帧美颜处理（磨皮/降噪/可选美白），输出美化后的视频并提供播放链接。
        处理耗时与视频长度成正比，过程中会实时汇报进度百分比。
        重要：本工具会自动从用户消息的附件中定位视频文件——即使你无法直接查看视频内容，
        只要用户表达了对视频的美化/美颜意图，就必须立即调用本工具。
        不要以"没有看到视频"为由拒绝或要求用户重新上传；若消息中确实没有附件，工具会返回明确的提示。

        :param strength: 美颜强度 0.0-1.0，默认 0.7。
        :param whiten: 美白程度 0.0-1.0，默认 0 表示关闭。
        """
        v = getattr(self, "valves", None) or self.Valves()
        return await tool_impl.execute_tool_task(
            event_emitter=__event_emitter__, request=__request__, files=__files__,
            messages=__messages__,
            task_type="video_beauty", name="视频美颜",
            description=f"对上传视频逐帧美颜，强度 {strength}，美白 {whiten}",
            sdk_command="video-beauty",
            parameters={"strength": strength, "denoise": True, "whiten": whiten},
            output_format="mp4", input_roles=["source"],
            timeout_s=v.timeout_seconds, base_url=v.webui_base_url,
            llm_label=(v.llm_provider, v.llm_model), progress_label="视频美颜",
        )
