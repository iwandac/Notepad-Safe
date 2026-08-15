@echo off
setlocal enabledelayedexpansion

REM ============================================================================
REM  Notepad Safe - one-click build script
REM  Features:
REM   - SOURCE_DIR / BUILD_DIR relative to the script dir (%~dp0)
REM       Works regardless of where the product repo is cloned.
REM   - Qt 6 path priority:
REM       1) env var NOTEPADSAFE_QT6_DIR
REM       2) CMake cache NOTEPADSAFE_QT6_DIR
REM       3) default: E:\programs\Qt_6.11.1-static-Release_msvc2026_64\msvc2026_64
REM   - VS install root can be overridden via env NOTEPADSAFE_VS_ROOT (e.g. VS 2022 = 17)
REM   - Production KDBX backend is enabled by default. Set
REM       NOTEPADSAFE_WITH_KDBX=OFF
REM     for a dependency-light editor-only build.
REM ============================================================================

set "SCRIPT_DIR=%~dp0"
set "SOURCE_DIR=%SCRIPT_DIR%."
set "BUILD_DIR=%SCRIPT_DIR%build"

if not defined NOTEPADSAFE_WITH_KDBX (
    set "NOTEPADSAFE_WITH_KDBX=ON"
)

if not defined NOTEPADSAFE_VS_ROOT (
    set "NOTEPADSAFE_VS_ROOT=C:\Program Files\Microsoft Visual Studio\18\Insiders"
)
set "VCVARS=%NOTEPADSAFE_VS_ROOT%\VC\Auxiliary\Build\vcvars64.bat"
set "VS_CMAKE=%NOTEPADSAFE_VS_ROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "VS_NINJA=%NOTEPADSAFE_VS_ROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

if not exist "%VCVARS%" (
    echo ERROR: vcvars64.bat not found: %VCVARS%
    echo   Hint: set NOTEPADSAFE_VS_ROOT=^<your VS install root^>
    exit /b 1
)

echo === [1/4] Activating VS environment ===
call "%VCVARS%" >nul 2>&1
if errorlevel 1 (
    echo ERROR: vcvars64.bat failed
    exit /b 1
)

if defined NOTEPADSAFE_QT6_DIR (
    echo Qt path from env NOTEPADSAFE_QT6_DIR = %NOTEPADSAFE_QT6_DIR%
)

echo === [2/4] Preparing locked dependencies ===
if /I "%NOTEPADSAFE_WITH_KDBX%"=="ON" (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%tools\bootstrap_crypto_deps.ps1"
    if errorlevel 1 (
        echo ERROR: KDBX dependency bootstrap failed
        exit /b 1
    )
) else (
    echo KDBX backend disabled by NOTEPADSAFE_WITH_KDBX=OFF
)

if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
mkdir "%BUILD_DIR%"

echo === [3/4] CMake Configure ===
pushd "%BUILD_DIR%"

if defined NOTEPADSAFE_QT6_DIR (
    "%VS_CMAKE%" -G Ninja ^
        -DCMAKE_BUILD_TYPE=Release ^
        -DCMAKE_MAKE_PROGRAM="%VS_NINJA%" ^
        -DNOTEPADSAFE_ENABLE_KDBX_BACKEND="%NOTEPADSAFE_WITH_KDBX%" ^
        -DNOTEPADSAFE_QT6_DIR="%NOTEPADSAFE_QT6_DIR%" ^
        "%SOURCE_DIR%"
) else (
    "%VS_CMAKE%" -G Ninja ^
        -DCMAKE_BUILD_TYPE=Release ^
        -DCMAKE_MAKE_PROGRAM="%VS_NINJA%" ^
        -DNOTEPADSAFE_ENABLE_KDBX_BACKEND="%NOTEPADSAFE_WITH_KDBX%" ^
        "%SOURCE_DIR%"
)

if errorlevel 1 (
    echo ERROR: CMake configure failed
    popd
    exit /b 1
)

echo === [4/4] Building ===
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
