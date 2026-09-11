"""
title: DeepAgent 图像美颜
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

    async def image_beauty(
        self,
        strength: float = 0.7,
        whiten: float = 0.0,
        denoise: bool = True,
        file_name: str = "",
        __event_emitter__=None,
        __files__=None,
        __messages__=None,
        __user__=None,
        __request__=None,
    ) -> str:
        """对图片执行美颜处理（双边滤波磨皮 + 可选美白降噪），返回处理后的结果图片。
        素材来源（二选一）：① 用户消息中附带的图片；② 素材库文件。
        重要："素材库"、"收藏夹"、"当前目录"指的是当前用户的素材库目录
        （image 子目录存放图片、video 子目录存放视频）——当用户提到这些词并给出
        文件名时，必须把文件名传入 file_name 参数直接调用本工具，
        不要以"没有看到图片"为由要求用户重新上传；若素材库和附件中都没有该文件，
        工具会返回明确的提示。
        处理链：若处理的素材本身是此前任务的结果，系统会自动登记派生关系（parent/链ID），在素材库"任务产物"页可查看完整处理链版本树，并支持从任意历史结果继续处理。
        示例："美颜一下这张图"、"处理素材库里的 photo.jpg，磨皮强一点(强度0.9)并美白0.3"。

        :param strength: 美颜/磨皮强度，0.0-1.0，默认 0.7；用户说"强一点"用 0.9，"轻微"用 0.4。
        :param whiten: 美白程度 0.0-1.0，默认 0 关闭；用户说"美白/提亮"时给 0.2-0.5。
        :param denoise: 是否附加降噪，默认 true；用户要求"保留皮肤纹理/不要过度处理"时设为 false。
        :param file_name: 素材库文件名（如 photo.jpg 或 image/photo.jpg）。消息已附带图片时留空。
        """
        v = getattr(self, "valves", None) or self.Valves()
        return await tool_impl.execute_tool_task(
            event_emitter=__event_emitter__, request=__request__, files=__files__,
            messages=__messages__, user=__user__,
            media_files=[file_name] if file_name else None,
            task_type="image_beauty", name="图像美颜",
            description=f"对图片执行美颜处理，磨皮强度 {strength}，美白 {whiten}，降噪 {denoise}",
            sdk_command="beauty",
            parameters={"strength": strength, "denoise": denoise, "whiten": whiten},
            output_format="jpg", input_roles=["source"],
            timeout_s=v.timeout_seconds, base_url=v.webui_base_url,
            llm_label=(v.llm_provider, v.llm_model), progress_label="图像美颜",
        )
