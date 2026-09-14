# 更新日志 (Changelog)

本文件记录 DeepAgent 项目各版本的功能变化。格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)。

---

## [v1.1.0] - 2026-09-14

> 基于 v1.0.0 基线的增强版本：多用户素材库、同端口集成界面、任务链与版本树、撤销/还原、参数化工具、桌面化体验。

### 新增

- **多用户素材库**：`media/<用户名>/image|video` 目录结构，按登录用户名隔离；扩展名自动分类
- **素材库网页管理**（同 8080 端口，`/media-ui/`）：树状浏览、预览（图片放大/视频播放）、拖拽与按钮上传自动分类、下载导出、删除、任务产物一键导入与"导入并继续处理"、处理链版本树视图
- **对话关键词识别**："素材库 / 收藏夹 / 当前目录"即指当前用户素材目录；各工具新增 `file_name` 参数（合成含 `file_name2`）
- **工具参数全面暴露**：美颜类 `denoise`、合成 `center`；素材库"工具说明"页实时展示全部工具与参数
- **撤销/还原（checkpoint 回退命令）**：新工具"撤销还原"——"撤销抠图，还原到美颜那一步"按任务单编号回退（追加式，历史保留，内容与检查点逐字节一致）；支持"还原到最开始(原图)"（task_id 留空自动取首轮上传图）；验证 9/9
- **任务链与版本树（一期）**：以历史产物为素材自动登记 `parent_task_id`/`pipeline_id`（任务单 schema 1.1）；接口 `GET /media/api/chain/{task_id}`
- **侧边栏集成**：聊天页侧边栏"工作空间"项后注入两个入口（🖼️ 素材库 / 🛠️ 工具说明），带分割线，随主题自适应
- **桌面化**：托盘程序（start_tray.bat；8080 已有服务时自动复用）；PWA"安装为应用"指引；Tauri 占位工程
- **登录体验**：登录名简化为 `admin`；登录表单标签改"用户名"、去除浏览器邮箱格式校验
- **防幻觉加固**：模型档案注入系统提示词——必须真实调用工具、严禁虚构任务结果

### 变更

- 启动方式：`start.bat` 改用集成启动器 `run_webui.py`（素材库 API 与页面挂载进 Open WebUI 同进程；SPA 兜底 Mount 自动重排序），素材库页面强制 `no-store`
- 素材库页面 UI 重构：分类入口移至侧边栏底部、工具说明主区全宽、"← 返回对话"、"＋ 添加素材"与全页拖拽上传；主题自动跟随聊天页深/浅色
- 托盘：服务改用 `pythonw` + 全部子进程 `CREATE_NO_WINDOW`（全程无黑框）；打开页面前自动等待服务就绪；剥离 socks5 代理环境变量（修复模型列表 Connection error 与 LLM 流式挂起）；服务日志落盘 `data/webui_service.log`
- 4 个处理工具升级至 v1.1.0；移除 `start_tray.vbs`，统一 `start_tray.bat`
- C++ SDK 调用方式评估（CLI vs pybind11）：结论与分阶段建议见 `docs/疑问.md` 第 9 节

### 修复

- 附件图片已发往 LLM 但工具找不到素材的问题：三级回退（消息 files → content 图片部件 → data URL 落盘）
- 托盘/子进程弹出命令行黑框
- 双击托盘菜单后"拒绝连接"（打开页面前自动等待服务就绪）

### 文档

- 新增：`任务链与版本树-一期实施.md`（带序号修改清单+逐项验证）、`任务链与版本树-二期LangGraph方案.md`（暂不实施）、`疑问.md`（9 问答）
- 验证脚本：`docs/verify_phase1.py`（一期 10/10）、`docs/verify_checkpoint_restore.py`（检查点还原 9/9）、`docs/repro_stream.py`
- 更新：任务信息JSON格式 schema 1.1、项目需求与实现步骤 §10、README（素材库/桌面化/版本徽章）

### 新增组件

- `assistant/core/media.py`、`assistant/media_api.py`、`assistant/run_webui.py`、`assistant/tray.py`
- `assistant/media_ui/index.html` 素材库与工具说明单页（主题跟随聊天页）
- `assistant/tools/deepagent_restore.py` 撤销还原工具
- `patches/open-webui-0.6.43/` 前端环境级补丁（补丁文件+原始备份+一键 apply/restore）
- `docs/verify_phase1.py`、`docs/verify_checkpoint_restore.py`、`docs/repro_stream.py`

### 说明

- C++ SDK（deepagent-cv）无算法改动，SDK 版本保持 1.0.0
- 已知限制：前端环境级补丁（登录表单、侧边栏入口）在 venv 内，pip 重装/升级后用 `patches/open-webui-0.6.43/apply_patches.py apply` 重打

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
