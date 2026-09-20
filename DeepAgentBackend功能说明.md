# DeepAgentBackend.exe 功能说明

> 版本：2.0
>
> 更新日期：2026-09-18
>
> 定位：DeepAgent（Graph_Saturn AI 助手）的 C++ 后端程序——托盘常驻服务，承载 Web 对话界面、AI Agent 执行流、.udrt 图工程文件生成，以及 **Graph 节点的 LLM 网关与按 GUID 的上下文管理**。
>
> 相关文档：需求与设计见 [../deepAgent需要与实现.md](../deepAgent需要与实现.md)；AI 服务层见 [../AIServices/AIServices功能说明.md](../AIServices/AIServices功能说明.md)

---

## 1. 程序概览

```
┌────────────── 浏览器（frontend/） ──────────────┐
│ Open WebUI 风格聊天页：会话侧栏 / 流式回复 /      │
│ 状态条 / udrt 结果卡片 / 知识库弹窗 / 语音输入    │
└──────────────┬──────────────────────────────────┘
               │ HTTP 127.0.0.1:8090
┌──────────────▼──────────────────────────────────────────────────────┐
│ DeepAgentBackend.exe（GUI 子系统 · 托盘常驻）= LLM 统一网关           │
│  cpp-httplib HTTP 服务（静态页 + REST + SSE）                        │
│  Agent 节点流（聊天编排）：意图分析 → 选择 → Plan IR → udrt 编译       │
│  Node 网关：/api/nodes/{guid}/generate（按 GUID 的多轮上下文管理）    │
│  上下文存储（node_contexts.json 落盘 + discard/过期清理）             │
│  LLM（DeepSeek, WinHTTP）                                           │
└──────┬──────────────────────────────────────────────▲───────────────┘
       │ 生成 .udrt                                    │ REST（网关协议）
       ▼                                              │
  output/udrt/*.udrt ──U-DeepRT 加载── NodePxy ────────┘
  （props.strGuid 标识节点）      （CNodeGateway，AIServices）
```

- **核心架构约定**：Graph 节点（NodePxy）**不直接调用 LLM 厂商 API**；所有 LLM 请求统一经 DeepAgentBackend 网关代理，节点与 Backend 之间以 **GUID** 双向识别。
- **技术栈**：C++17 / VS2019 v142 / x64 Debug；cpp-httplib + nlohmann/json（header-only）；WinHTTP（LLM）+ cpprest（AIServices 节点侧网关客户端）。
- **产物**：`E:\totem_AI\MetaSDK\x64\Debug\DeepAgentBackend.exe`（仅 Debug 编译，遵循工作区 AGENTS.md 约定）。

## 2. 启动与托盘

| 功能 | 说明 |
|---|---|
| 托盘常驻 | GUI 子系统启动，无控制台窗口；任务栏通知区显示自绘应用图标 |
| 打开对话 | 左键单击 / 双击托盘图标，或右键菜单「打开对话」→ 默认浏览器打开对话页 |
| 右键菜单 | 「打开对话」（默认加粗项）/「打开 udrt 输出目录」/「退出」 |
| 单实例互斥 | 命名互斥量 `Local\DeepAgentBackend_SingleInstance`；重复启动时自动唤起已有服务的对话页面后退出 |
| 优雅停机 | 菜单「退出」→ 停止 HTTP 服务、join 服务器/清理线程、释放互斥量 |
| 启动失败提示 | 端口被占用等错误经托盘气泡通知（无法绑定时）或系统弹窗（配置文件缺失时） |
| 日志 | `output/deepagent.log`（UTF-8，追加）+ OutputDebugString |

## 3. 核心功能 A：AI 对话与 udrt 生成

### 3.1 Agent 执行流（每轮对话）

| 步骤 | 行为 | 失败策略 |
|------|------|----------|
| 1 意图分析 | 系统提示 + 知识库条目 → LLM（temperature 0.2）→ 严格 JSON 输出：`is_tool_task` / `entry_id` / `subtitle_text`（字幕对白原文）/ `style_prompt`（风格字体要求提炼）/ `reason` | LLM 不可用 → 知识库关键词匹配回退 |
| 2 选择校验 | 选中条目必须存在于知识库（确定性） | 无效则忽略工具调用转普通对话 |
| 3 Plan 生成 | LLM 生成 Graph Plan IR（仅引用目录 catalog key 与语义端口） | 编译预校验失败 → 回退知识库内置 `udrt_template` |
| 4 属性覆盖 | 按目录声明合并属性覆盖：`strContent`←字幕文本、`strPrompt`←风格指令、`strApiKey`/`strModel`←Backend 自身 LLM 配置 | — |
| 5 编译 + 校验 | 实例 ID 分配、**GUID 生成**、语义端口→数字端口索引、props 默认值合并、position 布局；随后结构校验 | 校验失败 → SSE `error` |
| 6 落盘 | 原子写 `output/udrt/<前缀>_<时间戳>.udrt` | — |
| 7 回复 | LLM 流式生成自然语言汇报，token 增量推送 | 空回复 → SSE `error` |

**核心原则：LLM 不直接编写 udrt**——LLM 只做意图分析与语义层 Plan，udrt 由节点目录 + 确定性编译器生成。

未命中工具节点时进入**普通对话**：携带会话历史上下文流式回复，可介绍自身能力与知识库中已登记的工具 Node。

### 3.2 GUID 与节点网关（Node ⇄ Backend）

DeepAgentBackend 生成 udrt 时**为每个节点生成 GUID**（`CoCreateGuid`，32 位大写十六进制）：

- 写入节点 `props.strGuid`（目录声明了该属性的节点，如字幕节点——宿主加载 udrt 后 NodePxy 经 DyProps 读到）；
- 同时写入 `internal-data.guid`（所有节点均有记录字段）。

**节点与 Backend 的双向交互全部以 GUID 识别**。Graph 节点不再直接调用 LLM 厂商 API——所有 LLM 请求经 Backend 的节点网关代理（协议见第 5 节），多轮对话上下文由 Backend 按 GUID 持久管理。

以字幕节点（Graph_Saturn `CSubtitleGenNodePxy`）为例的运行时行为（**消费模式**，已实现）：

```
对话中（DeepAgent）：udrt 落盘 + LLM 直接生成 HTML 资产（{guid}.html，与 udrt 同目录）
U-DeepRT 节点 Execute（帧内）：
  ├─ EnsureGuid：读 DyProps.strGuid（空则本地生成兜底）
  ├─ 定位资产：<exe_dir>\udrt\{guid}.html（每帧检查，出现即消费）
  └─ 不触发任何 LLM 生成（运行后不再生成资产）
```

### 3.3 字幕工具 Node 的属性自动填充

生成的 udrt 中，字幕节点 props 与 `Graph_Saturn/include/Inc_NodeProxies_Saturn/ISubtitleGenNodePxy.h` 的 `subtitle_gen::CDyProps` 完全对齐（模块 ID 对应 `llm_subtitle_source = 0x70000002`）：

| 属性 | 填充来源 |
|------|----------|
| `strGuid` | 编译器生成的节点 GUID |
| `strContent` | 用户输入的字幕对白文本（意图分析提取） |
| `strPrompt` | prompt 中的风格/字体要求，由 LLM 浓缩为节点修改指令，无则空 |
| `strApiKey` / `strModel` | DeepAgent 自身 LLM 配置自动填入 |
| `strOutputDir` / `nActiveSegment` | 保持目录默认 |

### 3.4 知识库与节点目录（可扩展）

- `knowledge/knowledge_base.json`：工具 Node 语义条目（id、名称、模块 ID、能力描述、关键词、输入输出、`udrt_template`）。当前两条：`saturn_subtitle_html`（SubTitle Html Gen，`0x70000002`）与 `saturn_gaus_render`（Gaussian Render，`0x70000001`）。
- `knowledge/node_catalog.json`：确定性编译依据——catalog key → `model_name`、语义端口→数字端口索引、props/props_st 默认值（字幕节点含 `strGuid` 占位）、内置宿主节点 Titan Sink/Source、AV Monitor。
- **扩展新工具 Node**：在两个 JSON 中新增条目与目录项即可，无需改代码。

## 4. 核心功能 B：Node 上下文管理（按 GUID）

`backend/node_context.{h,cpp}`——`NodeContextStore`：

| 能力 | 说明 |
|---|---|
| 按 GUID 存多轮会话 | messages（system 常驻首位 + user/assistant 交替）、version（每次生成 +1）、state（idle/requesting/done/failed） |
| content / prompt 轮次 | content=新建/重建会话（新字幕文本）；prompt=向既有上下文追加修改指令 |
| 持久化 | `output/node_contexts.json`：启动加载、变更即写、临时文件原子替换、损坏备份 `.bad` |
| discard | `POST /api/context/discard` 仅标记；**撤销删除**场景：同 guid 再次 generate 自动恢复 active，历史继续有效 |
| 过期清理 | 后台线程启动时 + 每 30 分钟扫描；discarded 超过 `DEEPAGENT_CTX_RETENTION_DAYS`（默认 15 天）物理删除 |
| 并发保护 | 同 guid 轮次进行中（requesting）的新 generate 返回 409 busy |

## 5. REST API

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/` | 对话前端（静态托管 frontend/） |
| POST | `/api/chat/stream` | 聊天（SSE 响应），body: `{chat_id?, message}` |
| GET | `/api/chats` | 会话列表（按创建时间升序） |
| GET | `/api/chats/{id}` | 单个会话完整历史 |
| DELETE | `/api/chats/{id}` | 删除会话（不存在返回 404） |
| GET | `/api/knowledge` | 知识库条目 |
| GET | `/api/health` | 健康检查（LLM 配置状态、条目数、输出目录） |
| **POST** | **`/api/nodes/{guid}/generate`** | **Node 网关生成**：body `{system?, kind:"content"\|"prompt", text}`；响应即 LLM 结果 `{state:"done", html, version, guid}`；轮次进行中返回 409 busy；LLM 失败返回 502 `{state:"failed", error}` |
| **POST** | **`/api/context/discard`** | **上下文标记 discard**：body `{"guid": "..."}`；响应 `{discarded:true}` |
| **GET** | **`/api/context/{guid}`** | **查看该 guid 完整上下文**（调试） |
| **GET** | **`/api/contexts`** | **全部上下文状态列表**（含 retention_days，调试） |
| POST | `/api/open_folder` | 打开生成资产所在目录：body `{"path": "文件或目录"}`；文件 → 资源管理器打开所在目录并选中（`explorer /select`），目录 → 直接打开；本机服务有权限，网页沙箱无法直接打开本地目录故经由此 API |
| GET | `/api/asset/content?path=...` | 返回生成的 HTML 资产内容（text/html，供浏览器查看/下载）；仅允许 output/udrt 目录内文件，越权/穿越返回 403 |

SSE 事件流（聊天）：`status`（步骤进度）/ `token`（增量）/ `udrt`（udrt 工程卡片）/ `asset`（**HTML 资产卡片**：{type,file,guid,version}）/ `done` / `error`。

## 6. 会话存储（聊天）

- 持久化 `output/chats.json`：启动加载、变更即写、原子替换、损坏备份 `.bad`、自动标题、按时间升序。
- 前端侧边栏 hover 显示 🗑 删除按钮（带确认框）；删除当前会话回到欢迎页。

## 7. LLM 配置

加载顺序：`config/.env` → 同名进程环境变量覆盖 → 代码默认值。

| 配置项 | 默认值 | 说明 |
|---|---|---|
| `DEEPSEEK_API_KEY` | （无） | 必配；未配置时服务可启动但 LLM 调用失败并在 health 中标注 |
| `DEEPSEEK_MODEL` | `deepseek-chat` | 模型名；同时用于 udrt 的 `strModel` 填充 |
| `DEEPSEEK_BASE_URL` | `https://api.deepseek.com` | OpenAI 兼容服务地址 |
| `DEEPSEEK_PATH` | `/v1/chat/completions` | 请求路径 |
| `DEEPAGENT_HOST` / `DEEPAGENT_PORT` | `127.0.0.1` / `8090` | HTTP 服务监听 |
| `DEEPAGENT_FRONTEND_DIR` / `DEEPAGENT_KNOWLEDGE_DIR` / `DEEPAGENT_UDRT_DIR` / `DEEPAGENT_CHATS_FILE` | `<root>` 下对应目录 | 路径覆盖 |
| `DEEPAGENT_CTX_RETENTION_DAYS` | `15` | Node 上下文：discarded 后保留天数（超期物理删除，0 = 立即清理） |

LLM 协议：OpenAI 兼容 chat completions（Bearer 认证），支持流式（SSE 增量）与非流式两种模式；结构化阶段（意图/Plan）temperature 0.2，回复阶段使用配置温度（默认 0.7）。任何 OpenAI 兼容服务（GLM、MiniMax、本地 vLLM 等）改 base_url/model/key 即可接入。

## 8. 前端功能（frontend/，纯原生 HTML/CSS/JS，无构建步骤）

- Open WebUI 风格布局：深色会话侧边栏（新建/切换/删除会话、后端健康状态指示）+ 聊天主区；
- SSE 流式渲染：状态条（步骤进度 + 完成态）、Markdown 回复（标题/列表/代码块/表格）、udrt 结果卡片（节点/连接数、文件路径、⬇ 下载、**📂 打开所在目录**、JSON 折叠预览）、HTML 资产卡片（🌐 浏览器查看 / 📂 打开所在目录 / ⬇ 下载）；
- 欢迎页示例卡片（字幕 HTML 生成 / 高斯渲染 / 了解 DeepAgent）一键发送；
- 知识库弹窗：条目名称、模块 ID、预设/真实标签、输入输出说明；
- 语音输入：Web Speech API（`zh-CN`，连续识别 + 中间结果上屏；不支持的环境按钮置灰，识别错误弹出提示）；
- 输入框自适应高度、Enter 发送 / Shift+Enter 换行（兼容中文输入法组合态）；
- 非 2xx 响应显式展示错误信息，不静默失败。

## 9. 目录结构

```
E:\totem_AI\DeepAgent\
├── DeepAgent.sln                     # VS2019 v142, x64, 仅 Debug
├── backend\                          # DeepAgentBackend 源码
│   ├── main.cpp                      # wWinMain：单实例/初始化/托盘/服务器线程
│   ├── config.*                      # .env + 环境变量配置
│   ├── log.*                         # 文件 + OutputDebugString 日志
│   ├── tray.*                        # 托盘图标/右键菜单/消息泵
│   ├── llm_client.*                  # DeepSeek OpenAI 兼容客户端（WinHTTP + SSE）
│   ├── knowledge_base.*              # 知识库加载/关键词评分/LLM 提示文本
│   ├── node_catalog.*                # 节点目录/语义端口解析
│   ├── udrt_compiler.*               # Plan IR → udrt 确定性编译/校验/原子落盘
│   ├── agent_graph.*                 # Agent 节点流执行器
│   ├── chat_store.*                  # 聊天会话存储 + JSON 持久化
│   ├── node_context.*                # Node 按 GUID 的 LLM 上下文管理 + 落盘/过期清理
│   ├── http_server.*                 # REST/SSE/静态托管
│   ├── resource.h / DeepAgentBackend.rc / app.ico
│   └── third_party\{json.hpp, httplib.h}
├── frontend\                         # index.html / app.js / style.css / voice_input.js
├── knowledge\                        # knowledge_base.json / node_catalog.json
├── config\.env                       # LLM 与服务配置
├── output\
│   ├── udrt\*.udrt                   # 生成的图工程文件
│   ├── chats.json(.bad)              # 聊天会话持久化（损坏备份）
│   ├── node_contexts.json(.bad)      # Node 按 GUID 的上下文持久化（损坏备份）
│   └── deepagent.log                 # 运行日志
└── gui-test-screenshots\             # GUI 测试留档
```

## 10. 已修复缺陷记录（摘要）

1. WinHTTP 请求体误发 UTF-16 → 改发 UTF-8 字节；
2. 非流式 LLM 响应误走 SSE 解析 → 按 stream 参数分流解析；
3. `chat_id:null` 类型不匹配抛异常致连接静默断开 → 防御性字段读取 + 前端 `res.ok` 检查；
4. SSE 分块缺终止符（curl exit 18）→ `sink.done()` 正确收尾；
5. 会话持久化损坏备份失败（ifstream 句柄未释放即 MoveFileEx）→ 先关闭再移动，失败如实上报。

## 11. 构建方式

```powershell
# VS2019 x64 环境
msbuild E:\totem_AI\DeepAgent\DeepAgent.sln -p:Configuration=Debug -p:Platform=x64
# 产物：E:\totem_AI\MetaSDK\x64\Debug\DeepAgentBackend.exe
```

运行：直接双击或启动 exe（托盘常驻）；首次使用需在 `config\.env` 配置 `DEEPSEEK_API_KEY`。

## 12. 开源 C++ AI Agent 项目对比与推荐

> 评估口径（2026-09 基于 GitHub 实况调研）：**只要求支持 OpenAI 兼容协议（DeepSeek 可直接使用）**，不限制工具链（允许 VS2022/C++20/C++23/CMake/Bazel），不要求异步帧驱动。对比各项目的优点与缺点，评估"在开源项目基础上修改"与"自研"的取舍。

### 12.1 核心事实

- OpenAI 官方与 Anthropic 官方均**没有 C++ SDK**；MCP 官方组织亦无 C++ SDK（仅社区实现）；
- LangChain / LangGraph 没有官方或有影响力的 C++ 版本；
- **工具链实测（2026-09-18，本机）**：VS2019 16.11（MSVC 14.29）`/std:c++20` 对 concepts / coroutines / ranges(views::filter) / jthread / `<format>` 全部编译通过——**C++20 不是 v142 的硬门槛**，两个 C++20 候选（①②）的工具链成本主要剩"CMake 构建引入 + 全量编译验证"，无需升级 VS2022；C++23（⑨ langgraph-cpp）仍不可用。
- 曾是最热门的 C++ OpenAI 客户端 liboai（478★）已于 2025-12 归档。

### 12.2 候选项目优缺点对比

#### ① mhdimo/ai-sdk-cpp —— 功能匹配度最高（45★，MIT，活跃）
- **优点**：OpenAI 兼容 provider **内置 DeepSeek 封装**（另有通用 `openai_compatible.hpp` 支持自定义 base_url，覆盖 vLLM/z.ai 等）；`ToolLoopAgent` 多步工具调用循环；SSE 流式输出（C++20 协程 `AsyncGenerator`）；结构化输出；MCP 工具发现；重试机制——Agent 所需能力清单基本全勾。
- **缺点**：项目自声明 **MSVC 2022+**（实测 v142 的 C++20 特性面足够，剩余风险为全量编译验证与 Boost 1.82 在 v142 的兼容性确认）；依赖链较重：**Boost 1.82+**（含 Boost.JSON）+ Boost.Asio + OpenSSL + CMake 3.20；仅 45★ 的早期项目，API 未冻结，文档少，需做好 fork 自维护准备。

#### ② ClickHouse/ai-sdk-cpp —— 工程属性最平衡（185★，Apache-2.0，活跃）
- **优点**：OpenAI 兼容 API 只需自定义 base_url（DeepSeek 直接可用）；**工具调用**（多步 tool loop `max_steps`、异步工具并行执行）；流式输出；`ai::Messages` 多轮会话管理；`RetryConfig` 自动重试（408/409/429/5xx）；依赖全部 vendor 进仓库且很轻（cpp-httplib + nlohmann/json + concurrentqueue，无 Boost/Asio）；Apache-2.0 商业友好；ClickHouse 公司背书；CMake 明确含 MSVC 适配（`WIN32_LEAN_AND_MEAN`/`NOMINMAX`/`_WIN32_WINNT`）。
- **缺点**：C++20（**实测 v142 `/std:c++20` 特性面足够**，剩余风险为引入 CMake 构建与全量编译验证）；没有 DeepSeek 专属封装（用 base_url 方式，功能无碍）；无内置高层 Agent 编排抽象（tool loop 有，规划/反思等 agent 模式需自建）；项目年轻（2025-2026），API 可能变动。

#### ③ llama.cpp —— 生态基石但定位不同（128k★，MIT，极活跃）
- **优点**：生态最大、MIT、CUDA 一等公民、自带 OpenAI 兼容 server（`/v1/chat/completions` + 工具调用 `--jinja` + SSE）、Windows/预编译产物完善。
- **缺点**：定位是**本地推理引擎 + server**，不是"调用 DeepSeek 云端 API"的客户端/Agent 框架；作为依赖极其笨重（vendor 整个推理栈）。
- **适用**：需要离线/本地小模型时，它的 server 可直接作为 `DEEPSEEK_BASE_URL` 的本地替换端点（零代码改动）。

#### ④ D7EAD/liboai —— 曾最流行，已归档（478★，MIT）
- **优点**：归档前是 C++ OpenAI 客户端中星数最高、API 面最全的（chat/embeddings/audio/fine-tune/moderation），C++17 + libcurl，MSVC 支持良好。
- **缺点**：**2025-12 已归档**——无安全更新、无 issue 响应，不可作为长期依赖基座；无异步/流式/工具调用之外的高层 Agent 能力。

#### ⑤ olrea/openai-cpp —— 极轻但停更（323★，MIT，停更 2024-04）
- **优点**：C++11 即可用（对老工具链零压力）；header-only（1-2 个头）；自定义 base_url 支持 DeepSeek；chat/embeddings/files 等旧版 API 面。
- **缺点**：**停更 2 年+**；无 function calling/tools、无流式、无异步、无多轮会话管理——只有"瘦客户端"，Agent 能力需全部自建。

#### ⑥ hkr04/cpp-mcp —— MCP 工具协议层（324★，MIT，活跃）
- **优点**：C++17、CMake 3.10+、依赖极轻（cpp-httplib 内含）、MIT、CMake 有明确 MSVC 分支（`/utf-8`/`/bigobj`）；MCP client+server（stdio + HTTP/SSE）。
- **缺点**：定位是 **MCP 工具协议层**，不含 LLM 通信与 Agent 编排（模型调用仍需搭配上面任意客户端）；协议版本偏旧（2025-03-26），未支持 Streamable HTTP 新传输。

#### ⑦ mozilla-ai/agent.cpp —— 本地推理 Agent 编排（207★，MIT，活跃）
- **优点**：C++17、MIT、agent loop 生命周期回调、工具调用 + JSON Schema + GBNF 约束输出、多 agent 示例、CI 含 Windows/MSVC。
- **缺点**：**明确声明不支持调用外部 LLM API**（仅 llama.cpp 本地推理）——DeepSeek 云端场景直接不符。

#### ⑧ RunEdgeAI/agents.cpp —— 不推荐（105★）
- 优点：多 provider、ReAct/Plan-and-Execute/Orchestrator-Workers 工作流模式、工具系统。
- 缺点：**"evaluation License" 非标准开源许可**（商用/修改有法律风险）；Bazel 8.3+ 构建（团队无经验）；MCP 能力在付费 Pro 档。

#### ⑨ ouxianghui/langgraph-cpp —— 过于早期（4★，MIT）
- 优点：LangGraph 语义的 C++ 移植（StateGraph/checkpoint/恢复），与本项目的节点流概念契合；MIT。
- 缺点：硬性 **C++23**（VS2022 支持仍不全）+ CMake 3.24；4★ 个人项目，无生产可用性；E:\langgraph-cpp 可继续作为协议参考。

### 12.3 决策矩阵（只要求 OpenAI 兼容/DeepSeek 口径）

| 维度 | ① mhdimo/ai-sdk-cpp | ② ClickHouse/ai-sdk-cpp | ③ llama.cpp | ④ liboai | ⑤ olrea/openai-cpp | ⑥ cpp-mcp |
|---|---|---|---|---|---|---|
| DeepSeek/OpenAI 兼容 | ✅ 内置 DeepSeek | ✅ 通用 base_url | ✅（server 模式=本地推理） | ✅ | ✅ | ❌（MCP 层） |
| 工具调用 | ✅ ToolLoopAgent | ✅ tool loop | ✅ | ❌ | ❌ | MCP 工具 |
| 流式 | ✅ 协程 SSE | ✅ | ✅ | ❌ | ❌ | — |
| 异步 | ✅ 协程 AsyncGenerator | ✅ std::future | — | ❌ | ❌ | — |
| 多轮会话管理 | ✅ | ✅ ai::Messages | — | 部分 | 部分 | — |
| 许可证 | MIT | Apache-2.0 | MIT | MIT（已归档） | MIT | MIT |
| 活跃度 | 活跃 | 活跃 | 极活跃 | **归档** | **停更** | 活跃 |
| 依赖重量 | 重（Boost 1.82+Asio+OpenSSL） | 轻（vendor cpp-httplib+nlohmann） | 极重（整推理栈） | 中（libcurl） | 中（libcurl） | 轻 |
| 工具链 | MSVC 2022+ / C++20 协程 | C++20 / CMake | CMake（VS2022 推荐） | C++17 | C++11 | C++17 |
| 成熟度风险 | 高（45★ 早期） | 中（185★ 年轻） | 低 | 归档 | 高（停更） | 中 |

### 12.4 推荐结论

放宽工具链约束、只要求 OpenAI 兼容（DeepSeek）后，**"基于开源项目修改" become 可行的优选路线**，推荐分两种策略：

**策略一（推荐）：以 ② ClickHouse/ai-sdk-cpp 为 LLM 通信底座 + 自研薄业务层**
- 优点：DeepSeek 经 base_url 直接可用；工具调用循环/重试/流式/多轮消息管理全部现成；Apache-2.0 商业友好；依赖轻且全 vendor（无 Boost）；活跃且有企业背书；
- 需要自研的部分：Agent 业务编排（知识库/意图分析/udrt 确定性编译/GUID 上下文管理）——这部分是项目特有价值，开源帮不到；
- 需要付出：引入 CMake 构建 + 全量编译验证（v142 `/std:c++20` 实测特性面足够，无需升级 VS2022）。

**策略二：以 ① mhdimo/ai-sdk-cpp 为基座（功能最大化）**
- 优点：DeepSeek 封装 + ToolLoopAgent + 流式 + MCP 全部内置，自研量最小；
- 需要付出：Boost 1.82 依赖链 + C++20 协程复杂度 + 早期项目的修补投入（45★ 项目需做好自行维护 fork 的准备；v142 C++20 特性面实测足够，需确认项目声明的 MSVC 2022+ 门槛是否为硬性）。

**搭配建议**：任选①/②之一作为 LLM 通信底座；需要接入 MCP 工具生态时叠加 **⑥ hkr04/cpp-mcp**；需要本地离线小模型时以 **③ llama.cpp server** 作为 base_url 端点替换（零代码改动）。

**不建议作为基座**：④ liboai（归档）、⑤ olrea/openai-cpp（停更且无工具能力）、⑧ agents.cpp（许可证）、⑨ langgraph-cpp（C++23 + 过早）。

**与现状的衔接**：当前自研框架（LLM 网关 + GUID 上下文 + 确定性 udrt 编译）已交付且经实测；若后续迁移，改造范围仅限 LLM 通信层（`llm_client.*` / `CNodeGateway` 内部实现），Agent 业务层与 REST 协议无需变动。
