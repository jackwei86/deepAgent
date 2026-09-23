# 更新日志 / CHANGELOG

本项目遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

## V1.5.1 (2026-09-22)

计划文档：`E:\totem_AI\DeepAgent_V1.5.1_计划.md`

### 新增
- **工程资产上下文管理**：新增 `asset_context.{h,cpp}`（AssetContext：guid/node_ptr/来源工程副本/entry_id/style_prompt/background/text/html_file/version/chat_id/history）+ 全局索引 `udrt\asset_index.json`（guid O(1) 定位）；`create_avatar_assets`/`create_udrt` 生成时写上下文，`generate_node_asset` 优化回写追加历史
- **按需发送原则**：`.avatar` 工程文件内容不再注入 LLM 上下文（解析在工具内完成，仅路径提示）；`.udrt` 内容注入保留（对分析类回复有影响）；新工具 `get_asset_context(guid)` 供 LLM 按需查询资产生成参数与版本轨迹；`review_asset` 响应附带 `style_prompt` 切片
- **工程副本即唯一工作文件**：聊天路径输入/📎 附件的 `.udrt`/`.avatar` 全量复制到 `projects\{chat_id}\`（服务端强制，不依赖 LLM 行为），源文件与项目解耦，后续引用/修改均针对副本
- **删除联动清理**：`DELETE /api/chats/{id}` 现清理 `projects\{chat_id}` 工程副本 + 该会话生成的全部资产产物 + manifest + 索引条目（只读文件先清属性再删）

### 修复
- **notify 重生成丢失原始样式**：重生成现在从 AssetContext 恢复 `style_prompt`/`background`（此前用裸新文本重跑默认管线，样式全丢）；`/api/exe/notify` 的 changes 支持 `guid` 直查（project 可空）
- **资产生成管线样式缺失（既有缺陷）**：`AssetGraphRunner::buildInput` 从未把 `context.style` 拼入 content 分支的 LLM 消息——style_prompt 只影响过背景板关键词判断。现在样式按 legacy 惯例（`\n\n样式要求：`）拼入用户消息
- 测试事件回调并发 push 共享 vector 的数据竞争（加互斥锁）

### 已知问题
- 测试 exe 偶发段错误（NeoGraph 引擎在多节点批量 + 零延迟 mock 下的内部竞争，cdb 确认堆 UAF 于 graph_executor 协程路径；产品服务不受影响），缓解与详情见 V1.5.1 计划文档 §7

## V1.5.0 (2026-09-22)

计划文档：`E:\totem_AI\DeepAgent_V1.5.0_计划.md`（含启动方式/IPC 方案/合并 vs 分离 exe 的完整对比分析）

### 新增
- **.avatar 工程输入 → 批量字幕资产生成**：新增 `avatar_parser.{h,cpp}`（手写轻量 XML 扫描，零依赖；提取 `nodes/node` 下 `<param key="text_in">` 的 `primary/standard/track` 文本，XML 实体反转义，nodePtr 主键）；Agent Loop 新工具 `create_avatar_assets(path, style_prompt?, background?)`——逐节点批量经 AssetGraphRunner 子步管线生成 HTML（`udrt\{工程名}_{序号}_{guid8}.html`），落盘 manifest `{工程名}.assets.json`（node_ptr↔guid↔file↔version 映射），逐节点发资产卡片；消息路径扫描与 📎 附件白名单支持 `.avatar`（附件自动告知 LLM 保存路径）
- **工程宿主进程启动器**：新增 `process_launcher.{h,cpp}`（CreateProcessW + 句柄/PID 表 + 退出监视线程，同工程幂等）；`.avatar`→Avatar.exe、`.udrt`→UDeepRT.exe（`DEEPAGENT_AVATAR_EXE`/`DEEPAGENT_UDRT_EXE` 可覆盖）；`DEEPAGENT_AUTO_LAUNCH` 开关默认关（关闭时发"可启动"状态提示）；`POST /api/launch {kind, project}` 手动启动、`GET /api/processes` 启动记录快照
- **DeepAgent ⇄ exe 双向 IPC**：新增 `event_hub.{h,cpp}` 进程内事件广播（订阅者独立队列、按项目过滤、慢消费者截断、keepalive 探活）；`GET /api/exe/events?project=` SSE 长连接（事件 `asset_updated {project, guid, file, version, reason}`）；`POST /api/exe/notify` 接收 exe 侧 text_in 变更 → 按 manifest 单节点重生成（回写同一 html、version+1）→ hub 实时推送；create_udrt / create_avatar_assets / 质检优化回写全部接入发布
- **exe 侧接入契约**（计划文档 §4，纯 HTTP 实现无关）：argv 加载约定、SSE 订阅 schema、notify schema、`DEEPAGENT_BACKEND_URL` 端口发现、重载建议（Avatar: openProjectPath；UDeepRT: DrtDocManager::Load 整图）
- 测试 H1-H4（解析保真/实体反转义/损坏容错/FakeProvider 批量生成+manifest/EventHub 过滤与截断），F+G+H 合计 **101 项断言全过**；集成冒烟：.avatar 生成 4 卡片、订阅收 create 事件、notify 重生成 v1→v3 订阅端实时收到、/api/launch 干跑 404、附件流端到端

### 说明
- 两 exe 直接启动目前存在问题（exe 侧 argv 支持等）由用户在 U-MetaApp 后续完善；DeepAgent 侧启动失败不炸、错误可读
- AIServices 相关组件本版本未使用、未修改（IPC 契约实现无关）

## V1.4.0 (2026-09-21)

### 新增
- **P1 — Plan IR 脚本化生成**：旧固定管线的 Plan 生成从"LLM 输出裸 JSON"升级为"LLM 写受限 JS 函数 + QuickJS 沙箱执行"（`define("plan", function(context){...})`），可用 if/for 表达条件逻辑；沙箱限制内存上限（64MB）、栈深（1MB）与执行超时（5s，interrupt 中止），不加载 libc、无文件/网络访问；执行失败自动回填错误给 LLM 一轮自修复，仍失败回退知识库内置模板（`plan_script.h/cpp`；QuickJS 采用 NeoGraph MSVC 适配源码无前缀编译，与 `neograph_*` 库内 prefixed quickjs 符号隔离共存）
- **P2 — 编译诊断结构化**：`UdrtCompiler::compile/validate` 新增结构化诊断出参（`path`/`expected`/`actual`/`hint`/`known_catalogs`），`toolCreateUdrt` 将其回填给 Agent Loop 的 LLM 实现自修复；`NodeCatalog` 新增 `catalogKeys()` 合法值域清单
- **P3 — create_udrt 结果缓存**：按 inputHash（SHA-256(entry_id+subtitle_text+style_prompt+model)）进程内缓存，相同参数直接返回上次结果并带 `cached: true` 标记，不重复调用 LLM
- **P4 — phases 分组预留**：Plan IR 支持可选 `phases` 字段（`[{name, nodes[]}]`），随模板/工具结果/SSE `udrt` 事件透传；编译器不写入 udrt 文件（保持 U-DeepRT 格式兼容），供前端未来分组展示
- **P0 — 工具错误修复线索补全**：`generate_node_asset`（参数缺失/节点 busy/管线失败）与 `get_node_context`（guid 不存在回填 `known_guids` 清单）的错误响应均携带 `repairable`/`hint`/`expected_one_of` 类字段，Agent Loop 的 LLM 可据此下一轮自主纠正
- **资产卡片内嵌预览 + 一键质检优化**：HTML 资产卡片新增内嵌预览（sandbox iframe，默认展示、可收起）与「✨ 优化」按钮；点击后自动发起质检优化流程——Agent 先调新增的 `review_asset` 工具读回资产内容，按四维度（HTML 结构完整性/字幕文本保真/排版样式/用户样式要求符合度）评估，有问题则调 `generate_node_asset(kind=prompt, output_file=原路径)` 覆盖优化并推送 v2 资产卡片，质量良好则直接汇报评估结论。配套修复：资产路径解析基准修正（相对路径按协议以部署目录 exe_dir 为基准，修掉 `udrt\udrt\` 双拼）；未登记 node_context 的资产优化时以"优化指令 + 现有 HTML"自动播种上下文
- ZCode 落地测试 G1-G12：沙箱脚本执行/条件逻辑/语法错误/超时中止/超内存中止/中文数据往返 + 结构化诊断 path/expected/actual + phases 格式兼容（F1-F12 回归全绿，共 73 项）

### 变更
- `DeepAgentBackend.vcxproj` 接入 QuickJS（5 个 C 源文件，逐文件 C11/无警告/空前缀 shim 编译），exe 约 +2.3MB
- 测试构建脚本 `build_asset_test.bat` 扩展（QuickJS C11 目标 + P1/P2 测试源）

### 修复
- **Agent Loop 从未真正生效（V1.3.0 起即静默回退）**：工具检测请求（NeoGraph `OpenAIProvider`/ConnPool）在本服务进程内挂起至 120s 超时后静默回退旧固定管线——表现为"准备中"空窗 20s+、资产卡片 GUID 为空。多轮二分定位：同步 asio/WinHTTP/独立进程探针均正常，排除 DNS/线程上下文/防火墙/进程名拦截，确认为本进程内 ConnPool 异步链路缺陷（留待上游排查）。修复：新增 `WinHttpProvider`（`winhttp_provider.{h,cpp}`）以 WinHTTP 传输实现完整 OpenAI 工具协议（非流式 tool_calls 检测 + **流式 tool_calls 累积**，模型在流式轮直接发起工具调用不再被丢弃）；`AgentGraph::run()` 与 `AssetGraphRunner` 全部切换至该 Provider；`LlmClient` 抽出 `postChatJson`/`postChatStream` 通用请求层
- **Agent Loop 产物卡片缺失**：`create_udrt` 工具执行器现在向前端发送 `udrt`/`asset` SSE 卡片事件（含 GUID/绝对路径/phases），Agent Loop 路径与 legacy 管线的卡片行为一致
- **工具链中途"光说不做"**：编排提示词禁止工具调用过程中输出过渡性文字，要求 `list_knowledge_nodes → create_udrt` 连续完成
- **"准备中"空窗无反馈**：新增工具调用观察点（`Agent::set_tool_gate`）+ 工具执行器状态事件（编译 udrt/生成 HTML 资产等长等待阶段均推 `status` SSE），前端全程可见进度；另加启动期网络自诊断日志（`[netdiag]`）与失败原因落盘（`[agent_loop]`）
- 实测效果：同一条字幕生成请求从 125s+（含 120s 无反馈空窗）降至 **5.3s**，状态事件全程覆盖，udrt/HTML 产物与卡片、流式回复均正常

## V1.3.0 (2026-09-21)

### 新增
- **Agent Loop 聊天编排**：接入 NeoGraph `neograph::llm::Agent`（ReAct 循环），LLM 自主决策调用 4 个工具（`list_knowledge_nodes` / `create_udrt` / `generate_node_asset` / `get_node_context`）的次数与顺序，`max_iterations=8` 止损；工具协议异常时回退旧固定管线（`runLegacyPipeline`）
- **文件输入**：前端 📎 附件（.udrt/.xml）与聊天中路径输入，内容注入 LLM 上下文；上传文件落盘 `output/uploads/{chat_id}/`，删除会话联动清理
- **多路流子步并发**：资产生成管线化（可选背景板分支 ∥ LLM 内容分支 → 合成），`AssetGraphRunner` 并发执行 + 子步状态 SSE 事件
- 会话 JSON 落盘、会话归档、`open_folder` 窗口置顶、HTML 资产卡片打开所在目录

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
