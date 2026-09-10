# Tauri 桌面壳（需要 Rust 工具链）

本目录是 DeepAgent 桌面壳的 Tauri 工程占位。当前机器未安装 Rust，未执行打包；
安装工具链后按以下步骤完成：

## 环境准备
1. 安装 Rust: `winget install Rustlang.Rustup`（含 cargo）
2. 安装 Tauri CLI: `cargo install tauri-cli --version ^2`（或 npm: `npm i -g @tauri-apps/cli`）

## 工程结构（首次生成）
```
tauri-app/
├── src-tauri/
│   ├── tauri.conf.json   # windows: { url: "http://127.0.0.1:8080" }
│   └── src/main.rs       # 关闭窗口 → 隐藏到托盘；托盘菜单：打开聊天/素材库/退出
└── (UI 由 Open WebUI 提供，壳只是 WebView + 托盘)
```

## 打包
```
cargo tauri build          # 产物: src-tauri/target/release/bundle/msi/*.msi
```
注意: 打包前先确认 `assistant\run_webui.py` 集成服务可正常独立运行，
壳的职责仅是拉起该服务(或要求用户先通过 start.bat/start_tray.bat 启动)并提供桌面窗口。
