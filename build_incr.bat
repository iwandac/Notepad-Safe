@echo off
setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
set "SOURCE_DIR=%SCRIPT_DIR%."
set "BUILD_DIR=%SCRIPT_DIR%build"

if not defined NOTEPADSAFE_VS_ROOT (
    set "NOTEPADSAFE_VS_ROOT=C:\Program Files\Microsoft Visual Studio\18\Insiders"
)
set "VCVARS=%NOTEPADSAFE_VS_ROOT%\VC\Auxiliary\Build\vcvars64.bat"
set "VS_CMAKE=%NOTEPADSAFE_VS_ROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "VS_NINJA=%NOTEPADSAFE_VS_ROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

echo === [1/3] Activating VS environment ===
call "%VCVARS%" >nul 2>&1

echo === [2/3] CMake reconfigure (incremental) ===
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
pushd "%BUILD_DIR%"

"%VS_CMAKE%" -G Ninja ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_MAKE_PROGRAM="%VS_NINJA%" ^
    "%SOURCE_DIR%"
if errorlevel 1 (popd & exit /b 1)

echo === [3/3] Building target(s): %* ===
if "%~1"=="" (
    "%VS_CMAKE%" --build . --config Release -j
) else (
    "%VS_CMAKE%" --build . --config Release -j --target %*
)
if errorlevel 1 (popd & exit /b 1)

echo.
echo === INCREMENTAL BUILD SUCCESS ===
popd
endlocal
exit /b 0
