# 更新日志 / CHANGELOG

本项目遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

## V1.2.0 (2026-09-20)

### 新增
- **会话归档**：主列表会话项一键归档（软状态，数据完整保留）；侧栏底部「已归档」可折叠视图（箭头指示折叠状态）；删除按钮仅存在于归档视图（带不可恢复确认）；归档会话继续对话自动取消归档
- **「📂 打开所在目录」**：udrt / HTML 资产卡片一键打开资源管理器并选中文件，窗口置顶（TOPMOST）
- **HTML 资产生成**：字幕对话在生成 udrt 的同时由 LLM 直接生成 HTML 资产（`{guid}.html`，与 udrt 同目录关联），回复中展示资产卡片（🌐 浏览器查看 / 📂 打开所在目录 / ⬇ 下载）
- **相对路径体系**：udrt / 协议中路径以公共部署目录（U-DeepRT.exe 与 DeepAgentBackend.exe 同目录）为基准相对传递，UI 显示绝对路径；输出目录默认改为 `<exe_dir>\udrt`
- **Node 消费模式**：字幕节点（U-DeepRT）运行时按 GUID 定位并消费对话生成的资产，不再运行时触发 LLM 生成
- Node 网关与资产 API：`POST /api/open_folder`、`GET /api/asset/content`（含路径越权/穿越 403 防御）、`GET /api/contexts`
- Node 上下文：`GET /api/contexts` 状态列表、`retention_days` 可配置（默认 15 天）

### 修复
- SSE 分块响应缺少终止符（客户端 curl exit 18）
- `chat_id: null` 类型不匹配异常导致连接静默断开（前端表现为"无回复"）
- 静态文件浏览器缓存导致前端改动不生效（挂载点统一 `Cache-Control: no-cache`）
- WinHTTP 请求体误发 UTF-16 导致 DeepSeek 400
- 非流式 LLM 响应误走 SSE 解析导致意图/Plan 阶段静默回退

### 变更
- 静态资源引用加版本参数，前端改动刷新即生效
- udrt 中节点 GUID 双写（`internal-data.guid` + `props.strGuid`），与字幕节点 DyProps 对齐
- CAIConversation（AIServices）升级为对象级线程安全

## V1.1.0 (2026-09-18)

### 新增
- 任务栏托盘常驻（GUI 子系统，无控制台）：左键/双击打开对话、右键菜单（打开对话/打开 udrt 输出目录/退出）
- 单实例互斥：重复启动自动唤起已有服务的对话页面
- Node 上下文 JSON 落盘（原子替换写 + 损坏备份 `.bad`）
- 会话删除 API（`DELETE /api/chats/{id}`）
- 运行日志落盘（`output/deepagent.log`）

### 修复
- `http_server` 诊断日志输出到控制台的编码问题

## V1.0.0 (2026-09-17)

### 新增
- DeepAgentBackend 初版：DeepSeek LLM 网关（WinHTTP + SSE 流式）、聊天 Agent 编排（意图分析 → 知识库选择 → Plan IR → udrt 确定性编译 → 落盘 → 流式回复）
- 外部知识库（`knowledge_base.json`）与节点目录（`node_catalog.json`）：字幕 HTML 生成（`0x70000002`）、Gaussian Render（`0x70000001`）
- udrt 确定性编译器（GUID 生成、语义端口→数字端口、props 合并、结构校验）
- 聊天会话管理（REST + SSE + 持久化）、Node 网关 REST（按 GUID 的多轮上下文）
- Web 前端（Open WebUI 风格：会话侧栏/流式渲染/udrt 卡片/知识库弹窗/语音输入）
- 托盘常驻、单实例、HTML 资产生成与卡片
