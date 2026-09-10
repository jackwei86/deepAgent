@echo off
rem ============================================================================
rem DeepAgent AI Task Assistant - launcher (ASCII only to avoid codepage issues)
rem Requires: E:\DeepAgent\.venv created by setup_env.bat (or manually)
rem Session-only env vars (no permanent system changes, per AGENTS.md)
rem ============================================================================
setlocal enabledelayedexpansion

set "ROOT=%~dp0.."
set "VENV=%ROOT%\.venv"
if not exist "!VENV!\Scripts\python.exe" (
    echo [ERROR] venv not found: !VENV!
    echo          Run scripts\setup_env.bat first.
    exit /b 1
)

call "!VENV!\Scripts\activate.bat"

rem ---- session-only environment ----
rem Project root for DeepAgent tools (robust import path detection)
set "DEEPAGENT_ROOT=!ROOT!"
rem Open WebUI data dir keeps uploads/files together with project data
set "DATA_DIR=!ROOT!\data\webui"
rem HuggingFace mirror for model downloads in China
set "HF_ENDPOINT=https://hf-mirror.com"
rem faster-whisper GPU needs cuBLAS/cuDNN DLLs from pip packages on PATH
if exist "!VENV!\Lib\site-packages\nvidia\cublas\bin" set "PATH=!VENV!\Lib\site-packages\nvidia\cublas\bin;!PATH!"
if exist "!VENV!\Lib\site-packages\nvidia\cudnn\bin" set "PATH=!VENV!\Lib\site-packages\nvidia\cudnn\bin;!PATH!"
rem silence OpenCV INFO/WARN noise in child processes
set "OPENCV_LOG_LEVEL=ERROR"

echo Starting DeepAgent (Open WebUI + media library): http://127.0.0.1:8080
echo   media library UI: http://127.0.0.1:8080/media-ui/
echo (First run: open the page and create the admin account, then see README for LLM/STT setup)
python "!ROOT!\assistant\run_webui.py"
endlocal
