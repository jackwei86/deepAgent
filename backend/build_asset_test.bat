@echo off
rem Asset pipeline feature test (F1-F12) standalone build (VS2019 v142 / x64 / Debug)
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" >nul

set NG=E:\totem_AI\DeepAgent\third_party\NeoGraph
set INC=/I "%~dp0." /I "%~dp0third_party" /I "%NG%\include" /I "%NG%\deps\asio\include" /I "%NG%\deps\yyjson" /I "%NG%\deps"
set DEF=/D NEOGRAPH_STATIC_BUILD /D ASIO_STANDALONE /D ASIO_NO_DEPRECATED /D _WIN32_WINNT=0x0A00 /D WIN32_LEAN_AND_MEAN /D NOMINMAX /D _CRT_SECURE_NO_WARNINGS
set CXXFLAGS=/nologo /TP /std:c++20 /utf-8 /MDd /EHsc /GR /permissive- /W3 /Od

cd /d %~dp0
cl %CXXFLAGS% %INC% %DEF% ^
  asset_feature_test.cpp asset_pipeline.cpp asset_adapters.cpp asset_runner.cpp ^
  /Fe:asset_feature_test.exe ^
  /link /LIBPATH:"%NG%\build\Debug" /LIBPATH:"D:\ProgramData\Anaconda3\Library\lib" ^
  neograph_core.lib neograph_llm.lib neograph_async.lib yyjson.lib libcrypto.lib libssl.lib
if errorlevel 1 exit /b 1
echo === run F1-F12 ===
asset_feature_test.exe
exit /b %ERRORLEVEL%
