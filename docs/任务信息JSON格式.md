# DeepAgent 任务信息 JSON 格式规范

> 版本：`schema_version 1.1`　　更新：2026-09-11（1.1 新增 `parent_task_id`/`pipeline_id` 任务链字段）
>
> 用途：AI 任务助手将用户语音/文字经 LLM 解析后得到的"具体要执行的任务"固化为一份**任务单（Task Order）JSON**，
> 落盘保存并可直接发送给第三方工具执行。第三方工具只需按本规范读取 `inputs` + `task`，
> 执行处理后回写/输出 `execution` 结果即可接入本系统（参考实现：`deepagent-cv.exe run --task <task.json>`）。

## 1. 设计原则

1. **自包含**：一份 JSON 包含任务的完整上下文——用户原话、来源（语音/文字）、源文件描述、任务类型与参数、输出要求、执行状态。第三方工具无需再查询其他信息。
2. **生命周期可追溯**：任务单由助手创建（`pending`），执行器边执行边更新（`running`/进度/日志），终态回写（`success`/`failed`/`cancelled`）。同一文件即完整执行档案。
3. **命令行友好**：`execution.command` 给出可直接复制执行的完整命令行（argv 数组形式），与需求"以命令行形式执行最终任务"对应。
4. **可扩展**：未知 `task.type` 允许取值 `custom`，`task.parameters` 为开放对象，第三方可自行约定子参数。

## 2. 顶层结构

```text
TaskOrder
├── schema_version   string  固定 "1.1"
├── task_id          string  全局唯一，格式 YYYYMMDD-HHMMSS-XXXX（时间 + 4位随机）
├── parent_task_id   string  任务链：直接上游任务的 task_id（素材取自其产物时自动登记；1.1 新增，可为 null）
├── pipeline_id      string  任务链：链 ID = 首个任务的 task_id，其后任务继承（1.1 新增，可为 null）
├── created_at       string  ISO8601 带时区，任务单创建时间
├── updated_at       string  ISO8601 带时区，最后更新时间
├── origin           object  任务来源（用户输入与 LLM 解析信息）        【必填】
├── task             object  LLM 解析出的任务定义                      【必填】
├── inputs           array   源文件信息列表（≥1 项）                   【必填】
├── output           object  输出文件要求                              【必填】
└── execution        object  执行信息（助手/执行器维护）                【必填】
```

## 3. 字段详细定义

### 3.1 `origin` — 任务来源

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `input_type` | string | ✅ | `"text"`（文字输入）或 `"voice"`（语音输入，经 STT 转写） |
| `user_text` | string | ✅ | 用户原始话语原文；语音输入时为转写文本 |
| `language` | string | ❌ | 语言代码，如 `"zh"`、`"en"`，默认 `"zh"` |
| `stt_engine` | string | ❌ | 语音转写引擎标识（如 `faster-whisper:large-v3`），文字输入时省略 |
| `llm.provider` | string | ✅ | 解析本任务的 LLM 服务商标识（如 `deepseek`、`zhipu`、`minimax`、`openai`） |
| `llm.model` | string | ✅ | 具体模型名（如 `deepseek-chat`、`glm-4-plus`） |
| `session_id` | string | ❌ | 会话 ID（Open WebUI chat id），便于回溯 |
| `user_id` | string | ❌ | 用户 ID |

### 3.2 `task` — 任务定义（LLM 解析结果）

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `type` | string | ✅ | 任务类型枚举，见 3.2.1 |
| `name` | string | ✅ | 人类可读任务名（如 `"图像美颜"`） |
| `description` | string | ✅ | 任务自然语言描述：LLM 对"要做什么"的理解，供人核对与第三方工具理解意图 |
| `sdk_command` | string | ✅ | 对应 C++ SDK 子命令：`beauty` / `matting` / `composite` / `video-beauty` / `custom` |
| `parameters` | object | ✅ | 任务专属参数，见 3.2.2；未知任务类型为自由对象 |

#### 3.2.1 任务类型枚举（`task.type`）

| 取值 | 含义 | 对应 sdk_command |
|---|---|---|
| `image_beauty` | 图像美颜（磨皮/降噪/增强） | `beauty` |
| `image_matting` | 图像抠图（前景提取） | `matting` |
| `image_composite` | 图像合成（前景贴到底图） | `composite` |
| `video_beauty` | 视频美颜（逐帧处理） | `video-beauty` |
| `restore` | 撤销/还原：不做处理，结果内容=指定历史任务的产物（1.1 新增，由助手"撤销还原"工具生成） | `restore`(Python 内置拷贝，不经 C++) |
| `custom` | 第三方自定义任务 | `custom` |

#### 3.2.2 各任务 `parameters` 定义

**`image_beauty` / `video_beauty`**

| 字段 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `strength` | number | `0.7` | 美颜强度 0.0–1.0（映射双边滤波直径/迭代次数） |
| `denoise` | bool | `true` | 是否附加轻度降噪 |
| `whiten` | number | `0.0` | 美白程度 0.0–1.0，0 为关闭 |

**`image_matting`**

| 字段 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `mode` | string | `"grabcut"` | 抠图算法：`grabcut`（通用交互式分割，取画面中央主体）/ `chroma`（绿幕键控） |
| `rect` | object | `null` | grabcut 初始矩形 `{x,y,width,height}`（像素），缺省取画面中央 60% 区域 |
| `background` | string | `"transparent"` | 抠出前景的背景：`transparent`（RGBA PNG）/ `white` / `green` |

**`image_composite`**

| 字段 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `x`, `y` | int | `0` | 前景贴到底图的左上角坐标 |
| `scale` | number | `1.0` | 前景缩放比例 |
| `center` | bool | `true` | 缺省 `x/y` 时是否自动居中 |

### 3.3 `inputs[]` — 源文件信息（描述"素材是什么"）

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `file_id` | string | ✅ | 文件在系统内的唯一 ID（Open WebUI 文件 id 或生成的 uuid） |
| `role` | string | ✅ | 素材角色：`source`（主输入）/ `foreground`（合成前景）/ `background`（合成底图）/ `mask`（蒙版） |
| `path` | string | ✅ | 绝对路径（执行器以此为准） |
| `filename` | string | ✅ | 原始文件名（保留用户命名） |
| `mime_type` | string | ✅ | 如 `image/jpeg`、`image/png`、`video/mp4` |
| `size_bytes` | int | ❌ | 文件大小 |
| `width` / `height` | int | ❌ | 图像/视频帧尺寸（探测后填充） |
| `duration_ms` | int | ❌ | 视频时长（毫秒） |
| `fps` | number | ❌ | 视频帧率 |
| `frame_count` | int | ❌ | 视频总帧数 |
| `description` | string | ❌ | LLM 对该素材的自然语言描述（如"用户上传的人像照片"） |

### 3.4 `output` — 输出要求

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `path` | string | ✅ | 输出绝对路径（任务目录内） |
| `format` | string | ✅ | `jpg` / `png` / `mp4` |
| `mime_type` | string | ❌ | 输出 MIME 类型 |
| `overwrite` | bool | ❌ | 默认 `true` |

### 3.5 `execution` — 执行信息（由执行器维护，第三方工具对接时重点回写）

| 字段 | 类型 | 说明 |
|---|---|---|
| `engine` | string | 执行引擎：`cpp-sdk`（演示 C++ SDK）或第三方工具标识 |
| `engine_version` | string | 引擎版本（如 `1.0.0`） |
| `command` | array\<string\ | 完整命令行（argv 形式），可复制直接执行 |
| `task_file` | string | 本任务单 JSON 自身路径（自引用） |
| `status` | string | `pending` → `running` → `success` / `failed` / `cancelled` |
| `progress_percent` | number | 0–100，整数 |
| `stage` | string | 当前阶段描述（如 `"处理帧 350/1000"`） |
| `started_at` / `finished_at` | string | ISO8601；未开始为 `null` |
| `elapsed_ms` | int | 总耗时 |
| `exit_code` | int | 进程退出码；未运行为 `null` |
| `error` | string | 失败原因；成功为 `null` |
| `result.output_path` | string | 实际产物路径 |
| `result.output_url` | string | UI 预览/下载 URL（Open WebUI `/api/v1/files/...`） |
| `result.width/height/size_bytes` | int | 产物属性 |
| `result.thumbnail_path` | string | 缩略图路径（视频任务生成） |
| `logs` | array\<string\ | 执行日志尾部（最多保留 50 条） |

## 4. 完整示例

### 4.1 语音输入 → 图像美颜（终态 success）

```json
{
  "schema_version": "1.0",
  "task_id": "20260910-153045-a1b2",
  "created_at": "2026-09-10T15:30:45.120+08:00",
  "updated_at": "2026-09-10T15:30:48.936+08:00",
  "origin": {
    "input_type": "voice",
    "user_text": "帮我把这张照片美颜一下，强度稍微高一点",
    "language": "zh",
    "stt_engine": "faster-whisper:large-v3",
    "llm": { "provider": "deepseek", "model": "deepseek-chat" },
    "session_id": "chat-7f3e2b",
    "user_id": "user-admin"
  },
  "task": {
    "type": "image_beauty",
    "name": "图像美颜",
    "description": "对用户上传的人像照片进行磨皮美颜，根据语音要求采用较高强度（0.8）",
    "sdk_command": "beauty",
    "parameters": { "strength": 0.8, "denoise": true, "whiten": 0.2 }
  },
  "inputs": [
    {
      "file_id": "f0e1d2c3-8b7a-4f6e-9d10-11a12b13c14d",
      "role": "source",
      "path": "E:\\DeepAgent\\data\\uploads\\f0e1d2c3_photo.jpg",
      "filename": "photo.jpg",
      "mime_type": "image/jpeg",
      "size_bytes": 2458621,
      "width": 1920,
      "height": 1080,
      "description": "用户上传的人像照片"
    }
  ],
  "output": {
    "path": "E:\\DeepAgent\\data\\tasks\\20260910-153045-a1b2\\result.jpg",
    "format": "jpg",
    "mime_type": "image/jpeg",
    "overwrite": true
  },
  "execution": {
    "engine": "cpp-sdk",
    "engine_version": "1.0.0",
    "command": [
      "E:\\DeepAgent\\cpp-sdk\\build\\Debug\\deepagent-cv.exe",
      "beauty",
      "--input", "E:\\DeepAgent\\data\\uploads\\f0e1d2c3_photo.jpg",
      "--output", "E:\\DeepAgent\\data\\tasks\\20260910-153045-a1b2\\result.jpg",
      "--strength", "0.8"
    ],
    "task_file": "E:\\DeepAgent\\data\\tasks\\20260910-153045-a1b2\\task.json",
    "status": "success",
    "progress_percent": 100,
    "stage": "完成",
    "started_at": "2026-09-10T15:30:46.001+08:00",
    "finished_at": "2026-09-10T15:30:48.930+08:00",
    "elapsed_ms": 2929,
    "exit_code": 0,
    "error": null,
    "result": {
      "output_path": "E:\\DeepAgent\\data\\tasks\\20260910-153045-a1b2\\result.jpg",
      "output_url": "/api/v1/files/xxxx/result.jpg",
      "width": 1920,
      "height": 1080,
      "size_bytes": 2388412,
      "thumbnail_path": null
    },
    "logs": [
      "[15:30:46] 加载图像 1920x1080",
      "[15:30:47] 双边滤波 strength=0.80",
      "[15:30:48] 写出 result.jpg"
    ]
  }
}
```

### 4.2 文字输入 → 视频美颜（运行中快照）

```json
{
  "schema_version": "1.0",
  "task_id": "20260910-160201-b3c4",
  "created_at": "2026-09-10T16:02:01.000+08:00",
  "updated_at": "2026-09-10T16:02:15.300+08:00",
  "origin": {
    "input_type": "text",
    "user_text": "把这个视频美化一下",
    "language": "zh",
    "llm": { "provider": "zhipu", "model": "glm-4-plus" }
  },
  "task": {
    "type": "video_beauty",
    "name": "视频美颜",
    "description": "对视频逐帧磨皮美化，默认强度 0.7，输出 mp4",
    "sdk_command": "video-beauty",
    "parameters": { "strength": 0.7, "denoise": true, "whiten": 0.0 }
  },
  "inputs": [
    {
      "file_id": "f2233aa2-abcd-4e5f-8080-9999aaaabbbb",
      "role": "source",
      "path": "E:\\DeepAgent\\data\\uploads\\f2233aa2_clip.mp4",
      "filename": "clip.mp4",
      "mime_type": "video/mp4",
      "size_bytes": 52428800,
      "width": 1280,
      "height": 720,
      "duration_ms": 10000,
      "fps": 25.0,
      "frame_count": 250
    }
  ],
  "output": {
    "path": "E:\\DeepAgent\\data\\tasks\\20260910-160201-b3c4\\result.mp4",
    "format": "mp4",
    "mime_type": "video/mp4"
  },
  "execution": {
    "engine": "cpp-sdk",
    "engine_version": "1.0.0",
    "command": ["E:\\DeepAgent\\cpp-sdk\\build\\Debug\\deepagent-cv.exe", "video-beauty",
      "--input", "E:\\DeepAgent\\data\\uploads\\f2233aa2_clip.mp4",
      "--output", "E:\\DeepAgent\\data\\tasks\\20260910-160201-b3c4\\result.mp4",
      "--strength", "0.7"],
    "task_file": "E:\\DeepAgent\\data\\tasks\\20260910-160201-b3c4\\task.json",
    "status": "running",
    "progress_percent": 35,
    "stage": "处理帧 88/250",
    "started_at": "2026-09-10T16:02:01.800+08:00",
    "finished_at": null,
    "exit_code": null,
    "error": null,
    "logs": ["[16:02:01] 打开视频 1280x720@25fps 共250帧"]
  }
}
```

## 5. 第三方工具对接约定

1. **接收**：助手以文件形式交付任务单（`--task <path>`），或经 HTTP POST `task` 字段传递（未来扩展）。
2. **读取**：解析 `inputs[].path`（素材）+ `task.parameters`（参数）+ `output.path`（产物要求）。
3. **执行**：任意语言/引擎实现；若需上报进度，向 stdout 按行输出事件（见《项目需求与实现步骤.md》CLI 协议节）：
   `{"event":"progress","task_id":"...","percent":35,"stage":"..."}`
   `{"event":"done","output":"...","elapsed_ms":1200}`　/　`{"event":"error","message":"..."}`
4. **回写**：结束后更新 `execution`（`status`/`exit_code`/`result`/`logs`）并保持 `schema_version` 不变。
5. **兼容**：未知字段应忽略不报错；新增字段须经版本号升级（`1.1`、`2.0`…）。
