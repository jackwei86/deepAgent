@echo off
rem ============================================================================
rem One-time environment setup for DeepAgent assistant layer (ASCII only)
rem Creates E:\DeepAgent\.venv with Python 3.12 and installs:
rem   1. CUDA 12.8 build of PyTorch (per AGENTS.md recommended CUDA 12.8;
rem      driver 577.03 supports it; falls back to note below if download fails)
rem   2. open-webui (+ its deps; torch already satisfied so pip keeps CUDA build)
rem   3. faster-whisper (local STT) + nvidia cuBLAS/cuDNN wheels for GPU STT
rem   4. httpx (used by tools to upload results)
rem All session-only env; pip cache on E: because C: is nearly full.
rem ============================================================================
setlocal enabledelayedexpansion

set "ROOT=%~dp0..\.."
set "VENV=!ROOT!\.venv"
set "PIP_CACHE_DIR=!ROOT!\.pipcache"

set "PY=C:\Users\17180\AppData\Local\Programs\Python\Python312\python.exe"
if not exist "!PY!" (
    echo [ERROR] Python 3.12 not found: !PY!
    exit /b 1
)

if not exist "!VENV!\Scripts\python.exe" (
    echo Creating venv at !VENV! ...
    "!PY!" -m venv "!VENV!" || exit /b 1
)
call "!VENV!\Scripts\activate.bat"

python -m pip install --upgrade pip || exit /b 1

echo [1/4] Installing PyTorch CUDA 12.8 build ...
rem 锁定 +cu128 本地标签版本: 该标签仅存在于 pytorch 官方索引,
rem 避免本机 pip.ini 的国内镜像(extra-index)提供同名 CPU 轮子被优先选中
python -m pip install "torch==2.11.0+cu128" --index-url https://download.pytorch.org/whl/cu128 --retries 10 --timeout 120 || (
    echo [WARN] CUDA torch install failed, trying default wheel ^(CPU^) ...
    python -m pip install torch || exit /b 1
)

echo [2/4] Installing open-webui ...
python -m pip install "open-webui>=0.6.10,<0.7" || exit /b 1

echo [3/4] Installing faster-whisper + NVIDIA runtime DLLs for GPU STT ...
python -m pip install faster-whisper httpx || (
    echo [WARN] faster-whisper install failed; voice input will fall back to other STT engines
)
python -m pip install nvidia-cublas-cu12 nvidia-cudnn-cu12 || (
    echo [WARN] NVIDIA DLL wheels failed; faster-whisper will run on CPU
)

echo [4/4] Done. Start with assistant\start.bat
endlocal
