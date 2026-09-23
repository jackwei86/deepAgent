@echo off
rem Asset pipeline feature test (F1-F12 + ZCode P0-P4 G1-G12) standalone build (VS2019 v142 / x64 / Debug)
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" >nul

set NG=E:\totem_AI\DeepAgent\third_party\NeoGraph
set QJS=%NG%\build\generated\quickjs-msvc
set INC=/I "%~dp0." /I "%~dp0third_party" /I "%NG%\include" /I "%NG%\deps\asio\include" /I "%NG%\deps\yyjson" /I "%NG%\deps" /I "%QJS%"
set DEF=/D NEOGRAPH_STATIC_BUILD /D ASIO_STANDALONE /D ASIO_NO_DEPRECATED /D _WIN32_WINNT=0x0A00 /D WIN32_LEAN_AND_MEAN /D NOMINMAX /D _CRT_SECURE_NO_WARNINGS
set CXXFLAGS=/nologo /Zi /std:c++20 /utf-8 /MDd /EHsc /GR /permissive- /W3 /Od

cd /d %~dp0

rem ---- QuickJS (P1 sandbox): compile as C11 without symbol prefix ----
rem ---- plan_js empty prefix shim shadows deps/quickjs/neograph prefix header ----
for %%f in (quickjs cutils dtoa libregexp libunicode) do (
  if not exist %%f.obj cl /nologo /TC /std:c11 /utf-8 /W0 /Od /MDd ^
    /D _CRT_SECURE_NO_WARNINGS /D CONFIG_VERSION=\"2026-06-04\" ^
    /I "%~dp0plan_js" /I "%QJS%" /I "%NG%\deps\quickjs" ^
    /c "%QJS%\%%f.c" /Fo"%~dp0%%f.obj"
  if errorlevel 1 exit /b 1
)

cl %CXXFLAGS% %INC% %DEF% ^
  asset_feature_test.cpp asset_pipeline.cpp asset_adapters.cpp asset_runner.cpp ^
  plan_script.cpp udrt_compiler.cpp node_catalog.cpp avatar_parser.cpp event_hub.cpp asset_context.cpp ^
  quickjs.obj cutils.obj dtoa.obj libregexp.obj libunicode.obj ^
  /Fe:asset_feature_test.exe ^
  /link /LIBPATH:"%NG%\build\Debug" /LIBPATH:"D:\ProgramData\Anaconda3\Library\lib" ^
  neograph_core.lib neograph_llm.lib neograph_async.lib yyjson.lib libcrypto.lib libssl.lib ole32.lib
if errorlevel 1 exit /b 1
echo === run F1-F12 + G1-G12 ===
asset_feature_test.exe
exit /b %ERRORLEVEL%
