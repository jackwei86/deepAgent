# 更新日志 (Changelog)

本文件记录 DeepAgent 项目各版本的功能变化。格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)。

---

## [未发布]

### 新增

- **任务链与版本树（一期）**：以历史任务产物为素材时自动登记派生关系（任务单新增 `parent_task_id`/`pipeline_id`，schema 1.1）；素材库"任务产物"页显示链徽标、**处理链**视图（第 N 步/状态/产物路径）与**导入并继续处理**；新增 `GET /media/api/chain/{task_id}` 接口
- 托盘/任务执行子进程统一 `CREATE_NO_WINDOW`，彻底消除命令行黑框；托盘服务改用 `pythonw`
- 素材库页面添加图标（安装为应用时显示）

### 文档

- 新增 `docs/任务链与版本树-一期实施.md`（带序号修改清单 + 逐项验证）
- 新增 `docs/任务链与版本树-二期LangGraph方案.md`（StateGraph + SqliteSaver 设计，暂不实施）
- `docs/任务信息JSON格式.md` 升级 schema 1.1；§10.4/10.5 记录前端补丁

---

## [v1.1.0] - 2026-09-10

> 基线 v1.0.0 之上的增强版本：多用户素材库、同端口集成界面、工具参数化、桌面化体验。

### 新增

- **多用户素材库**：`media/<用户名>/image|video` 目录结构，按登录用户名隔离；扩展名自动分类（jpg/png/bmp/webp/gif → image；mp4/mov/avi/mkv/webm/flv/ts/wmv/m4v → video）
- **素材库网页管理**（同 8080 端口，`/media-ui/`）：
  - 左侧树状目录浏览；图片点击放大、视频在线播放
  - 拖拽/多选上传，按扩展名自动分类入库
  - 任意文件下载导出、删除管理
  - **任务产物一键导入**素材库（列出最近任务的结果文件）
  - **工具说明 Tab**：全部已注册工具及参数表，与注册表实时同步
- **对话关键词识别**："素材库 / 收藏夹 / 当前目录"即指当前用户素材目录；各工具新增 `file_name` 参数（合成工具为 `file_name`+`file_name2`），可直接处理素材库文件，无需上传附件
- **工具参数全面暴露**：美颜类新增 `denoise` 降噪开关；合成新增 `center` 自动居中开关；全部参数说明在"工具说明"页实时展示
- **登录体验**：登录表单标签改为"用户名"、去除浏览器邮箱格式校验；管理员登录名由 `admin@deepagent.local` 简化为 `admin`
- **防幻觉加固**：`deepseek-v4-pro` 模型档案注入系统提示词——必须真实调用工具、严禁虚构任务结果

### 变更

- 启动方式：`start.bat` 改用集成启动器 `assistant/run_webui.py`（素材库路由与页面挂载进 Open WebUI 同进程同端口；SPA 兜底 Mount 自动重排序保证自定义路由命中）
- 工具版本：4 个 DeepAgent 工具升级至 v1.1.0（新增素材库参数与关键词语义说明）

### 新增组件

- `assistant/core/media.py` 素材库核心（分类/解析/树/安全路径）
- `assistant/media_api.py` 素材库 API（树/上传/下载/删除/任务导入/工具说明，复用 Open WebUI 登录鉴权）
- `assistant/run_webui.py` 集成启动器
- `assistant/media_ui/index.html` 素材库与工具说明单页
- `assistant/tray.py` + `start_tray.bat` / `start_tray.vbs` 系统托盘（vbs 完全无命令行窗口；8080 已有服务时自动复用）
- `desktop/tauri-app` Tauri 桌面壳占位工程与指引（需 Rust 工具链）

### 说明

- C++ SDK（`deepagent-cv`）本版本无算法改动，SDK 版本保持 1.0.0
- 已知限制：登录表单补丁与素材库路由挂载作用于 venv 内/启动器层，`pip` 重装升级 open-webui 后需按文档重打（见 `docs/项目需求与实现步骤.md` §10.4）

---

## [v1.0.0] - 2026-09-10

> 首个可用版本（基线）：AI 图像/视频任务助手全链路打通。

### 核心功能

- **输入**：聊天文字输入 + 麦克风语音输入（本地 faster-whisper，GPU 加速）
- **多 LLM 接入**：OpenAI 兼容端点运行时配置，DeepSeek / 智谱 GLM / MiniMax / Ollama 均可
- **图像/视频任务**：美颜（双边滤波磨皮+美白降噪）、抠图（GrabCut / 绿幕键控）、合成（Alpha 混合）、视频逐帧美颜
- **C++ SDK 调用**：`deepagent-cv.exe`（VS2019 v142 x64 Debug + OpenCV）以命令行方式执行；JSON-lines 进度协议
- **任务单 JSON**：LLM 解析结果固化为标准任务单（源文件描述/任务参数/执行状态），归档 `data/tasks/<task_id>/task.json`，支持 `run --task` 供第三方工具重放
- **进度与预览**：聊天实时进度状态条；结果图片内联预览、视频播放链接
- **CUDA 加速**：torch 2.11.0+cu128、faster-whisper GPU（cuBLAS/cuDNN）

### 基于

- 开源基座 [Open WebUI](https://github.com/open-webui/open-webui) v0.6.43（BSD），通过官方 Tools 扩展机制二次开发
