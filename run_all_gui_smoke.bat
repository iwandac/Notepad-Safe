@echo off
REM ============================================================================
REM  notepad-safe GUI smoke test runner (Windows / PowerShell).
REM  Governed by QUALITY_ASSURANCE_AND_GUI_TESTING.md section 2.
REM
REM  Usage:
REM    cd /d ^<repository-root^>
REM    .\run_all_gui_smoke.bat          Build and run every GUI smoke test.
REM    .\run_all_gui_smoke.bat build    Build only.
REM    .\run_all_gui_smoke.bat coord    Run gui_smoke_coord only.
REM    .\run_all_gui_smoke.bat fr       Run gui_smoke_findreplace only.
REM ============================================================================
setlocal enabledelayedexpansion

set ROOT=%~dp0
if defined NOTEPADSAFE_BUILD_DIR (
    set "BUILD=%NOTEPADSAFE_BUILD_DIR%"
) else (
    set "BUILD=%ROOT%build"
)

set "NOTEPADSAFE_BUILD_TESTS=ON"
set "NOTEPADSAFE_BUILD_DIAGNOSTICS=OFF"
set "NOTEPADSAFE_WITH_KDBX=ON"
if not defined NOTEPADSAFE_VS_ROOT (
    set "NOTEPADSAFE_VS_ROOT=C:\Program Files\Microsoft Visual Studio\18\Insiders"
)
set "VS_CTEST=%NOTEPADSAFE_VS_ROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"

echo.
echo === notepad-safe GUI Smoke Suite ===
echo.

REM -------- Step 1: build --------
pushd "%ROOT%"
if /I "%1"=="build" (
    call "%ROOT%build_incr.bat" notepadsafe_gui_smoke_tests 2>&1
    set "BUILD_RC=!errorlevel!"
    if not "!BUILD_RC!"=="0" (
        echo. & echo [FATAL] Build failed. & exit /b 1
    )
    echo. & echo [OK] Build-only done. & popd & exit /b 0
)
if /I "%1"=="coord" (
    call "%ROOT%build_incr.bat" gui_smoke_coord 2>&1
    set "BUILD_RC=!errorlevel!"
    if not "!BUILD_RC!"=="0" ( echo [FATAL] build gui_smoke_coord failed & exit /b 1 )
    goto RUN_ONE_COORD
)
if /I "%1"=="fr" (
    call "%ROOT%build_incr.bat" gui_smoke_findreplace 2>&1
    set "BUILD_RC=!errorlevel!"
    if not "!BUILD_RC!"=="0" ( echo [FATAL] build gui_smoke_findreplace failed & exit /b 1 )
    goto RUN_ONE_FR
)

REM Default: build and run the complete suite. CMake owns the suite list so a
REM newly registered GUI smoke cannot be silently omitted by this script.
call "%ROOT%build_incr.bat" notepadsafe_gui_smoke_tests 2>&1
set "BUILD_RC=!errorlevel!"
if not "!BUILD_RC!"=="0" (
    echo.
    echo [FATAL] Build failed; see build log above.
    exit /b 1
)

:RUN_ALL
pushd "%BUILD%"
if exist "%VS_CTEST%" (
    "%VS_CTEST%" --no-tests=error --output-on-failure -L "^smoke$"
) else (
    ctest --no-tests=error --output-on-failure -L "^smoke$"
)
set EC=!errorlevel!
popd
if not "!EC!"=="0" exit /b !EC!
echo === GUI SMOKE TESTS PASSED ===
exit /b 0

:RUN_ONE_COORD
pushd "%BUILD%"
echo.
echo === Run ONLY gui_smoke_coord ===
".\gui_smoke_coord.exe"
set EC=%errorlevel%
popd
echo. & echo exit = %EC%
exit /b %EC%

:RUN_ONE_FR
pushd "%BUILD%"
echo.
echo === Run ONLY gui_smoke_findreplace ===
".\gui_smoke_findreplace.exe"
set EC=%errorlevel%
popd
echo. & echo exit = %EC%
exit /b %EC%
