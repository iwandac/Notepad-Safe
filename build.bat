@echo off
setlocal enabledelayedexpansion

:: ============================================================================
::  Notepad Safe — 一键构建脚本
::  特性：
::   · SOURCE_DIR / BUILD_DIR 相对于脚本自身所在目录（%~dp0）
::     → 无论 clone 到什么路径，本脚本都能直接构建
::   · Qt 6 路径优先级：
::       1) 环境变量 NOTEPADSAFE_QT6_DIR
::       2) 已定义的 NOTEPADSAFE_VS_ROOT
::       3) 回退默认 "E:\programs\Qt_6.11.1-static-Release_msvc2026_64\msvc2026_64"
::   · VS 路径可通过环境变量 NOTEPADSAFE_VS_ROOT 覆盖（例如 VS 2022 = 17）
:: ============================================================================

:: ---------- 路径相对化：基于 %~dp0（脚本自身所在目录） -------------------
set "SCRIPT_DIR=%~dp0"
set "SOURCE_DIR=%SCRIPT_DIR%."
set "BUILD_DIR=%SCRIPT_DIR%build"

:: ---------- VS 安装根（可通过环境变量 NOTEPADSAFE_VS_ROOT 覆盖）----------
if not defined NOTEPADSAFE_VS_ROOT (
    set "NOTEPADSAFE_VS_ROOT=C:\Program Files\Microsoft Visual Studio\18\Insiders"
)
set "VCVARS=%NOTEPADSAFE_VS_ROOT%\VC\Auxiliary\Build\vcvars64.bat"
set "VS_CMAKE=%NOTEPADSAFE_VS_ROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "VS_NINJA=%NOTEPADSAFE_VS_ROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

if not exist "%VCVARS%" (
    echo ERROR: vcvars64.bat not found: %VCVARS%
    echo   Hint: set NOTEPADSAFE_VS_ROOT=^<你的 VS 安装目录^>
    exit /b 1
)

echo === [1/3] Activating VS environment ===
call "%VCVARS%" >nul 2>&1
if errorlevel 1 (
    echo ERROR: vcvars64.bat failed
    exit /b 1
)

:: ---------- Qt 路径（NOTEPADSAFE_QT6_DIR 作为 cache override） ----------------
if defined NOTEPADSAFE_QT6_DIR (
    echo Qt path from env NOTEPADSAFE_QT6_DIR = %NOTEPADSAFE_QT6_DIR%
)

:: ---------- 准备 build 目录 --------------------------------------------------
if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
mkdir "%BUILD_DIR%"

echo === [2/3] CMake Configure ===
pushd "%BUILD_DIR%"

:: 先传 NOTEPADSAFE_QT6_DIR 作为 cache（若用户设置了 env）
if defined NOTEPADSAFE_QT6_DIR (
    "%VS_CMAKE%" -G Ninja ^
        -DCMAKE_BUILD_TYPE=Release ^
        -DCMAKE_MAKE_PROGRAM="%VS_NINJA%" ^
        -DNOTEPADSAFE_QT6_DIR="%NOTEPADSAFE_QT6_DIR%" ^
        "%SOURCE_DIR%"
) else (
    "%VS_CMAKE%" -G Ninja ^
        -DCMAKE_BUILD_TYPE=Release ^
        -DCMAKE_MAKE_PROGRAM="%VS_NINJA%" ^
        "%SOURCE_DIR%"
)

if errorlevel 1 (
    echo ERROR: CMake configure failed
    popd
    exit /b 1
)

echo === [3/3] Building ===
"%VS_CMAKE%" --build . --config Release -j
if errorlevel 1 (
    echo ERROR: Build failed
    popd
    exit /b 1
)

echo.
echo === BUILD SUCCESS ===
if exist notepad-safe.exe (
    dir notepad-safe.exe | findstr "notepad-safe"
    echo Output: %BUILD_DIR%\notepad-safe.exe
) else (
    echo WARNING: notepad-safe.exe not found
    dir *.exe 2>nul
)

popd
endlocal
exit /b 0
