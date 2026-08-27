@echo off
setlocal

REM Build and run the non-GUI regression suite. Product build.bat deliberately
REM excludes these targets; this script opts in and builds only this suite.
set "SCRIPT_DIR=%~dp0"
if defined NOTEPADSAFE_BUILD_DIR (
    set "BUILD_DIR=%NOTEPADSAFE_BUILD_DIR%"
) else (
    set "BUILD_DIR=%SCRIPT_DIR%build"
)
set "NOTEPADSAFE_BUILD_TESTS=ON"
set "NOTEPADSAFE_BUILD_DIAGNOSTICS=OFF"

if not defined NOTEPADSAFE_VS_ROOT (
    set "NOTEPADSAFE_VS_ROOT=C:\Program Files\Microsoft Visual Studio\18\Insiders"
)
set "VS_CTEST=%NOTEPADSAFE_VS_ROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"

call "%SCRIPT_DIR%build_incr.bat" notepadsafe_non_gui_tests
if errorlevel 1 (
    echo ERROR: Non-GUI test build failed.
    exit /b 1
)

pushd "%BUILD_DIR%"
if exist "%VS_CTEST%" (
    "%VS_CTEST%" --no-tests=error --output-on-failure -E "^(gui_smoke_|e2e_|quick\.)"
) else (
    ctest --no-tests=error --output-on-failure -E "^(gui_smoke_|e2e_|quick\.)"
)
set "TEST_RC=%errorlevel%"
popd

if not "%TEST_RC%"=="0" exit /b %TEST_RC%
echo === NON-GUI TESTS PASSED ===
exit /b 0
