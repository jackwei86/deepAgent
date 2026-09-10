@echo off
rem DeepAgent tray launcher (no console window). ASCII only.
setlocal enabledelayedexpansion
set "ROOT=%~dp0.."
set "DEEPAGENT_ROOT=!ROOT!"
set "DATA_DIR=!ROOT!\data\webui"
set "DEEPAGENT_MEDIA_DIR=!ROOT!\media"
set "HF_ENDPOINT=https://hf-mirror.com"
set "OPENCV_LOG_LEVEL=ERROR"
start "" "!ROOT!\.venv\Scripts\pythonw.exe" "!ROOT!\assistant\tray.py"
echo DeepAgent tray started (check system tray).
endlocal
