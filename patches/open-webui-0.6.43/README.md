# open-webui 0.6.43 前端环境级补丁

本目录保存 DeepAgent 对 open-webui 前端的全部环境级补丁（venv 内文件不入 git，
此处为入库副本）。`pip` 重装/升级 open-webui 后，运行应用脚本即可重打。

## 补丁内容

| 文件 | 补丁 |
|---|---|
| `files/frontend/index.html` | 侧边栏底部注入两个入口："🖼️ 素材库"（/media-ui/）与"🛠️ 工具说明"（/media-ui/?docs=1）；位置在"工作空间"项之后，带分割线；URL 参数驱动视图切换 |
| `files/frontend/_app/immutable/nodes/43.D1AE-7j5.js` | 登录表单：`type="email"` → `type="text"`（去掉浏览器邮箱格式校验）；标签"电子邮箱" → "用户名" |

## 使用

```bat
:: 应用补丁(pip 安装/重装/升级 open-webui 后运行)
.venv\Scripts\python.exe patches\open-webui-0.6.43\apply_patches.py apply

:: 还原为原始文件(排查问题时用)
.venv\Scripts\python.exe patches\open-webui-0.6.43\apply_patches.py restore
```

应用后浏览器 **Ctrl+F5** 强刷一次加载新资源。

## 目录结构

- `files/` — **补丁后**的文件（按 venv 内相对路径存放）
- `originals/` — 对应的**原始文件**（首次应用前自动备份的副本也留在 venv 内 `*.bak`）
- `apply_patches.py` — 一键应用/还原脚本

## 注意事项

1. 补丁基于 **open-webui 0.6.43** 的编译产物；若升级到其他版本，minified 文件名与内容会变，
   需人工对照 `files/` 与新版文件重新制作补丁（修改点说明见
   `docs/项目需求与实现步骤.md` §10.4 登录表单、§10.5 侧边栏注入）；
2. apply 脚本首次应用时自动把 venv 里的原文件备份为 `*.bak`，重复运行安全；
3. 应用后需刷新浏览器；若页面异常，先运行 `restore` 排查是否补丁与新版本不兼容。
