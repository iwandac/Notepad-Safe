@echo off
setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
set "SOURCE_DIR=%SCRIPT_DIR%."
if defined NOTEPADSAFE_BUILD_DIR (
    set "BUILD_DIR=%NOTEPADSAFE_BUILD_DIR%"
) else (
    set "BUILD_DIR=%SCRIPT_DIR%build"
)
if defined NOTEPADSAFE_BUILD_JOBS (
    set "BUILD_JOBS=%NOTEPADSAFE_BUILD_JOBS%"
) else (
    REM Keep incremental builds responsive on developer machines. Set the
    REM environment variable to 1 when diagnosing an individual command.
    set "BUILD_JOBS=4"
)
if not defined NOTEPADSAFE_BUILD_TESTS (
    set "NOTEPADSAFE_BUILD_TESTS=OFF"
)
if not defined NOTEPADSAFE_BUILD_DIAGNOSTICS (
    set "NOTEPADSAFE_BUILD_DIAGNOSTICS=OFF"
)
if not defined NOTEPADSAFE_WITH_KDBX (
    set "NOTEPADSAFE_WITH_KDBX=ON"
)

REM Match build.bat: stable English /showIncludes output and UTF-8 diagnostics.
set "VSLANG=1033"
chcp 65001 >nul

if not defined NOTEPADSAFE_VS_ROOT (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if exist "!VSWHERE!" (
        for /f "usebackq tokens=*" %%I in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "NOTEPADSAFE_VS_ROOT=%%I"
    )
)
if not defined NOTEPADSAFE_VS_ROOT set "NOTEPADSAFE_VS_ROOT=C:\Program Files\Microsoft Visual Studio\18\Insiders"
set "VCVARS=%NOTEPADSAFE_VS_ROOT%\VC\Auxiliary\Build\vcvars64.bat"
set "VS_CMAKE=%NOTEPADSAFE_VS_ROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "VS_NINJA=%NOTEPADSAFE_VS_ROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

echo === [1/4] Activating VS environment ===
if not exist "%VCVARS%" (
    echo ERROR: vcvars64.bat not found: %VCVARS%
    echo   Hint: set NOTEPADSAFE_VS_ROOT=^<your VS install root^>
    exit /b 1
)
call "%VCVARS%" >nul 2>&1
if errorlevel 1 (
    echo ERROR: vcvars64.bat failed
    exit /b 1
)

echo === [2/4] Preparing locked offline dependencies ===
call "%SCRIPT_DIR%prepare_dependencies.bat"
if errorlevel 1 (
    echo ERROR: dependency preparation failed
    exit /b 1
)

echo === [3/4] CMake reconfigure (incremental) ===
echo Build directory: %BUILD_DIR%
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
pushd "%BUILD_DIR%"

"%VS_CMAKE%" -G Ninja ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_MAKE_PROGRAM="%VS_NINJA%" ^
    -DNOTEPADSAFE_BUILD_TESTS="%NOTEPADSAFE_BUILD_TESTS%" ^
    -DNOTEPADSAFE_BUILD_DIAGNOSTICS="%NOTEPADSAFE_BUILD_DIAGNOSTICS%" ^
    -DNOTEPADSAFE_ENABLE_KDBX_BACKEND="%NOTEPADSAFE_WITH_KDBX%" ^
    "%SOURCE_DIR%"
set "CONFIGURE_RC=!errorlevel!"
if not "!CONFIGURE_RC!"=="0" (
    popd
    exit /b !CONFIGURE_RC!
)

echo === [4/4] Building target(s): %* ===
if "%~1"=="" (
    "%VS_CMAKE%" --build . --config Release -j %BUILD_JOBS%
    set "BUILD_RC=!errorlevel!"
) else (
    "%VS_CMAKE%" --build . --config Release -j %BUILD_JOBS% --target %*
    set "BUILD_RC=!errorlevel!"
)
if not "!BUILD_RC!"=="0" (
    popd
    exit /b !BUILD_RC!
)

echo.
echo === INCREMENTAL BUILD SUCCESS ===
popd
endlocal
exit /b 0
