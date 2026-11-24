@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

echo === 查找 Visual Studio C++ 工具链 ===
set "VSW=C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSW%" (
    echo [错误] 未找到 vswhere.exe，请安装 Visual Studio Build Tools 的 C++ 组件。
    pause & exit /b 1
)

set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"%VSW%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if "%VSPATH%"=="" (
    echo [错误] 未找到含 C++ 工具链的 Visual Studio。
    pause & exit /b 1
)
echo   使用: %VSPATH%

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
    echo [错误] vcvars64.bat 执行失败。
    pause & exit /b 1
)

if not exist build mkdir build

echo === 编译资源 ===
rc /nologo /fo build\app.res res\app.rc
if errorlevel 1 ( echo [错误] 资源编译失败 & pause & exit /b 1 )

echo === 编译程序 ===
cl /nologo /EHsc /std:c++17 /O2 /MT /W3 /utf-8 /DUNICODE /D_UNICODE ^
   /Fo:build\ /Fe:FanCtrl.exe ^
   src\main.cpp src\sensor.cpp src\fan.cpp src\config.cpp src\tray.cpp ^
   build\app.res ^
   /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib ole32.lib oleaut32.lib wbemuuid.lib shell32.lib advapi32.lib
if errorlevel 1 ( echo [错误] 编译失败 & pause & exit /b 1 )

echo.
echo === 构建成功 ===
for %%f in (FanCtrl.exe) do echo   %%~ff  (%%~zf 字节)
echo.
echo 直接运行 FanCtrl.exe 即可（需管理员权限，程序会自行请求）。
pause
