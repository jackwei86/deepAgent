# DeepAgent — AI 图像/视频任务助手

基于开源项目 **[Open WebUI](https://github.com/open-webui/open-webui)**(BSD) 二次开发的 AI 任务助手：
用户通过**文字或语音**下达指令，由可自由切换的国产/开源 LLM(DeepSeek / 智谱 GLM / MiniMax / Ollama…)
解析出具体任务与参数，固化为**标准任务单 JSON** 后，以**命令行方式调用本地 C++ 图像/视频处理 SDK** 执行，
任务进度实时反馈在聊天 UI，结果图像内联预览、视频提供播放链接。

```
用户(文字/语音) ──► Open WebUI + DeepAgent Tools ──► LLM 意图解析(function calling)
                        │ 创建任务单 task.json (docs/任务信息JSON格式.md)
                        ▼
              sdk_runner 组装命令行 ──► deepagent-cv.exe run --task task.json
                        │ JSON-lines 进度事件流            (C++ SDK, VS2019 x64 Debug)
                        ▼
              聊天 UI 进度状态 ──► 完成后结果预览(图/视频) + 任务单归档
```

## 目录结构

| 目录 | 说明 |
|---|---|
| `cpp-sdk/` | 演示版 C++ 图像/视频 SDK：美颜(双边滤波)、抠图(GrabCut/绿幕)、合成(Alpha 混合)、视频逐帧处理；CLI 封装 + JSON-lines 进度协议 |
| `assistant/` | Open WebUI 二开层：`core/`(任务单管理+SDK 执行器)、`tools/`(4 个 Tool)、`install_tools.py`、`start.bat` |
| `docs/` | 《项目需求与实现步骤.md》《任务信息JSON格式.md》 |
| `data/` | 任务单 `tasks/<task_id>/task.json+result.*`、Open WebUI 数据 `webui/` |
| `samples/` | 冒烟测试素材 |

## 快速开始

```bat
:: 1. 构建 C++ SDK (VS2019 v142 / x64 / Debug；OpenCV 目录可按需替换)
cd cpp-sdk\scripts && build_windows.bat

:: 2. 安装 Python 环境(CUDA 12.8 版 torch + open-webui + faster-whisper)
cd ..\assistant\scripts && setup_env.bat

:: 3. 启动服务
cd .. && start.bat
:: 浏览器打开 http://127.0.0.1:8080 → 创建管理员账号

:: 4. 注册 DeepAgent 工具(管理员 API 密钥在 设置→账号→API密钥)
python install_tools.py --token <你的API密钥>

:: 5. 配置 LLM 与语音(见 docs/项目需求与实现步骤.md §6)
```

## 已验证的执行链路

| 任务 | 素材 | 耗时 | 结果 |
|---|---|---|---|
| 图像美颜 0.8 | 1280×720 jpg | ~0.3s | `data/tasks/*/result.jpg` |
| 抠图 GrabCut | 1280×720 jpg | ~8s | RGBA PNG |
| 抠图 绿幕键控 | 1280×720 jpg | ~0.1s | RGBA PNG |
| 图像合成 0.5x | 底图+前景 PNG | ~0.1s | jpg |
| 视频美颜 0.6 | 1280×720@25fps 6s | ~30s(150帧) | mp4(24 次进度回调) |

## 关键设计

- **任务单 JSON**(`docs/任务信息JSON格式.md`)：LLM 解析结果 → 标准任务单落盘 → 可直接发给任何第三方工具执行(`deepagent-cv.exe run --task task.json` 即参考实现)。
- **纯 Debug 链路**：本机 OpenCV 4.6 Debug DLL 与 VS2019 Debug CRT 的 `std::string` ABI 一致，杜绝跨模块字符串错位。
- **CUDA 加速**：Python 侧(torch/faster-whisper 语音识别)安装 cu128 版本；C++ 侧算法为 CPU 实现，如需 GPU 可换 CUDA 版 OpenCV 重建。

详细的需求分析、架构说明、配置指南见 **`docs/项目需求与实现步骤.md`**。
