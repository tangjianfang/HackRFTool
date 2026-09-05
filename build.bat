@echo off
rem ASCII-only on purpose: cmd.exe parses batch files in the ANSI codepage,
rem so UTF-8 Chinese comments/echo text can garble or break parsing.
setlocal

rem Always operate from the repo root (script directory), whatever the caller CWD.
cd /d "%~dp0"

where cmake >nul 2>nul
if errorlevel 1 (
    echo [build.bat] cmake not found in PATH. Install CMake 3.25+ or run from a VS 2022 developer prompt.
    exit /b 1
)

echo [build.bat] Configure preset x64-release ...
cmake --preset x64-release
if errorlevel 1 (
    echo [build.bat] CONFIGURE FAILED.
    exit /b 1
)

echo [build.bat] Build preset x64-release ...
cmake --build --preset x64-release
if errorlevel 1 (
    echo [build.bat] BUILD FAILED - do not run ctest on a stale binary ^(see docs/lessons.md L1^).
    exit /b 1
)

echo [build.bat] OK. Main app: %~dp0out\build\x64-release\Release\HackRFTool.exe
echo [build.bat] Verify with: ctest --preset x64-release
rem When launched by double-click, %cmdcmdline% references this script; pause
rem then so the result stays visible. Terminal invocations skip the pause.
echo %cmdcmdline% | find /i "%~0" >nul && pause
exit /b 0
