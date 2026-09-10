@echo off
rem ============================================================================
rem DeepAgent CV SDK build script (VS2019 / v142 / x64 / Debug only, per AGENTS.md)
rem Usage: scripts\build_windows.bat [OpenCV_DIR]   (default G:\openv4.11.0._install)
rem NOTE: keep this file ASCII-only to avoid codepage issues.
rem ============================================================================
setlocal enabledelayedexpansion

set "ROOT=%~dp0.."

if "%~1"=="" (
    set "OpenCV_DIR=G:\openv4.11.0._install"
) else (
    set "OpenCV_DIR=%~1"
)
if not exist "!OpenCV_DIR!\OpenCVConfig.cmake" (
    echo [ERROR] OpenCV not found: !OpenCV_DIR!\OpenCVConfig.cmake
    echo         Pass your OpenCV dir as the first argument.
    exit /b 1
)

rem ---- locate VS2019 by probing common edition paths (avoids vswhere quoting pitfalls) ----
set "VS2019DIR="
for %%p in (
    "!ProgramFiles(x86)!\Microsoft Visual Studio\2019\Community"
    "!ProgramFiles(x86)!\Microsoft Visual Studio\2019\Professional"
    "!ProgramFiles(x86)!\Microsoft Visual Studio\2019\Enterprise"
    "!ProgramFiles!\Microsoft Visual Studio\2019\Community"
    "!ProgramFiles!\Microsoft Visual Studio\2019\Professional"
    "!ProgramFiles!\Microsoft Visual Studio\2019\Enterprise"
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community"
) do (
    if not defined VS2019DIR if exist "%%~p\VC\Auxiliary\Build\vcvarsall.bat" set "VS2019DIR=%%~p"
)
if not defined VS2019DIR (
    echo [ERROR] Visual Studio 2019 with C++ v142 toolset not found.
    exit /b 1
)
echo Using VS2019: !VS2019DIR!

call "!VS2019DIR!\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 exit /b 1

rem ---- session-only CUDA 12.8 (this OpenCV is a CUDA 12.8 build; AGENTS.md recommends 12.8) ----
set "CUDA_PATH=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8"
set "CUDACXX=!CUDA_PATH!\bin\nvcc.exe"
set "PATH=!CUDA_PATH!\bin;!PATH!"

rem ---- configure + build (Debug only) ----
cmake -S "!ROOT!" -B "!ROOT!\build" -G "Visual Studio 16 2019" -A x64 -T v142 ^
      -DOpenCV_DIR="!OpenCV_DIR!" ^
      -DCUDA_NVCC_EXECUTABLE="!CUDA_PATH!\bin\nvcc.exe"
if errorlevel 1 (
    echo [ERROR] CMake configure failed.
    exit /b 1
)

cmake --build "!ROOT!\build" --config Debug
if errorlevel 1 (
    echo [ERROR] Build failed.
    exit /b 1
)

echo.
echo [OK] !ROOT!\build\Debug\deepagent-cv.exe
endlocal
