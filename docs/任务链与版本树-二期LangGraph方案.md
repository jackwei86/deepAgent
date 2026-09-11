# 二期方案：LangGraph StateGraph + SqliteSaver（流程断点续跑）

> 状态：**仅方案设计，暂不实施**（一期"任务链+版本树"已覆盖产物回退与分支）。
> 触发条件：出现"多步自动工作流（LLM 决策连续处理）/ 长任务断点续跑 / 处理前人工审批"需求时启动本方案。
> 前置评估结论见本文件 §1-§2（优缺点已于 2026-09-10 评审）。

## 1. 背景与已核实事实

| 事实 | 影响 |
|---|---|
| langgraph 1.0.10 + langgraph_checkpoint 4.2.0 **已随 open-webui 装在 venv** | 核心 API 零新增依赖；仅缺 SqliteSaver 实现（需 `pip install langgraph-checkpoint-sqlite`，小包，依赖 aiosqlite） |
| 工具 = 原子 CLI 子进程，状态已持久化在 `task.json`，产物文件不可变不覆盖 | 文件即检查点；图状态只存引用不存产物 blob |
| 对话状态在 `webui.db`、任务状态在 `task.json` | 双真相源——必须用 §4 的关联原则统一 |
| open-webui 钉死 langchain 系版本（自身 RAG 在用） | 升级 langgraph 需整体评估，避免破坏 open-webui |

## 2. 优缺点评审结论（2026-09-10）

**优点**：标准化 checkpoint/时间旅行 API（`get_state_history` + 按 `checkpoint_id` 分叉）；断点续跑（失败从断点重放非整体重跑）；`interrupt` 人工审批；agent 化多步工作流的正确抽象；LangGraph Studio 可视化调试；核心 MIT。

**缺点/风险**：双真相源同步（webui.db ↔ checkpoint 存储，回退不自动联动 UI）；粒度错配（当前任务为秒~分钟级原子操作，checkpoint 价值场景未出现）；langgraph 版本升级可能与 open-webui 钉死版本冲突；LangGraph Server/Studio 在 Windows 原生支持一般；学习调试成本（第四层栈）。

## 3. StateGraph 设计

### 3.1 图结构（工具执行链节点化）

```
                    ┌────────────┐
   pipeline 触发 ──► │ resolve    │ 素材解析(附件/素材库/上游产物)
                    └─────┬──────┘
                          ▼
                    ┌────────────┐
                    │ validate   │ 参数校验/素材可读性检查 ──(失败)──► END(failed)
                    └─────┬──────┘
                          ▼
                    ┌────────────┐   interrupt(长视频/大批量时人工确认)
                    │ execute    │ 子进程 deepagent-cv.exe + 进度事件
                    └─────┬──────┘
                          ▼
                    ┌────────────┐
                    │ register   │ 产物登记/上传/files 事件/任务单回写
                    └────────────┘
```

### 3.2 状态 Schema（只存引用，不存产物 blob）

```python
class PipelineState(TypedDict):
    pipeline_id: str          # 链 ID(=首任务 task_id)，与 thread_id 绑定
    task_ids: list[str]       # 链上全部任务(含历史)——"checkpoint↔任务单↔产物"三跳可达
    current_task_id: str
    material_paths: list[str] # 素材文件路径(引用)
    parameters: dict          # 工具参数
    node_status: dict         # {节点名: ok|failed|skipped}
    error: str | None
    retry_count: int
    result_path: str | None   # 产物路径(引用)
```

### 3.3 Checkpointer

- 实现：`langgraph_checkpoint_sqlite.SqliteSaver`，DB = `data/agent_state.db`
- thread_id = `pipeline_id`；每次节点执行后自动落 checkpoint
- **保留策略**：每条链保留最近 N=10 个 checkpoint + 成功终态；过期清理任务随托盘服务周期执行

## 4. 关联与回退语义（四条设计原则，两期共同遵守）

1. **文件是真相，checkpoint 是加速器**：产物永远以文件为准（不可变不覆盖）；checkpoint 丢失可从任务链完整重建状态机，不允许出现"只在 checkpoint 里"的数据；
2. **双向关联**：graph config 携带 `{"thread_id": pipeline_id, "task_id": ...}`；任务单 JSON 记录 thread 信息——"checkpoint ↔ 任务单 ↔ 产物"三跳可达；
3. **checkpoint 可重建**：序列化格式随 langgraph 升级可能变化，恢复失败时降级为从 task.json 重建（一期链路）；
4. **保留策略**：见 §3.3。

## 5. 回退场景走查（二期上线后）

- **从历史产物分支**：与一期相同（版本树 + 以此继续处理）；
- **断点续跑**：长视频 execute 节点崩溃 → 重新 invoke 同 thread_id → 从最后 checkpoint 继续（execute 节点需支持跳过已完成部分，C++ 侧需提供 `--start-frame` 类参数——**待扩展**）；
- **人工审批**：video-beauty 前置 `interrupt`，管理界面批准后 resume；
- **时间旅行调试**：`get_state_history(thread_id)` 排查任意历史执行状态。

## 6. 工作量与风险

| 项 | 估计 |
|---|---|
| StateGraph + SqliteSaver + 单测 | 2~3 天 |
| execute 节点断点续跑（C++ `--start-frame`） | 1~2 天（需改 SDK） |
| interrupt 审批 UI（复用聊天确认或素材库页） | 1~2 天 |
| 风险 | langgraph 升级与 open-webui 钉死版本冲突；serde 格式变化（§4.3 缓解）；Windows 下 aiosqlite 表现需验证 |
