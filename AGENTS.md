# DeepAgent Agent Instructions

## Required Build Environment

- Operating system: Windows.
- IDE/toolchain: Visual Studio 2019.
- Platform: `x64`.
- MSVC toolset: `v142`.
- CUDA 12.1 12.3 12.8 Toolkit already installed on the system.
- Recommended use CUDA version: `12.8`.
- If not necessary, do not install, upgrade, downgrade, or replace Visual Studio, MSVC, CUDA
- Do not modify system-wide environment variables permanently.
- Do not use Visual Studio 2022 unless explicitly requested.
- Do not compile the project using release mode.Only compile it in debug mode.

## Toolchain Initialization

Run builds from a Visual Studio 2019 x64 Developer PowerShell or Developer Command Prompt.

Before building, verify the environment:

```powershell
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"

if (Test-Path $vswhere) {
    & $vswhere `
        -version "[16.0,17.0)" `
        -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -latest `
        -property installationPath
}

where.exe cl
where.exe msbuild
where.exe nvcc
nvcc --version
```

## 版本日志更新说明

- 从基线版本v1.0往后，每次修改代码后，都在更新日志.md中写入对应的版本和修改的内容
