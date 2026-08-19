@echo off
REM ============================================================================
REM  notepad-safe GUI 冒烟测试一键脚本（Windows / PowerShell 环境）
REM  按 QUALITY_ASSURANCE_AND_GUI_TESTING.md §2 强制管理。
REM
REM  用法：
REM    cd ^<repository^>\notepad-safe
REM    .\run_all_gui_smoke.bat          构建+运行全部 GUI smoke，输出汇总 PASS/FAIL
REM    .\run_all_gui_smoke.bat build    只构建，不运行
REM    .\run_all_gui_smoke.bat coord    只跑 gui_smoke_coord
REM    .\run_all_gui_smoke.bat fr       只跑 gui_smoke_findreplace
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
set TARGETS=gui_smoke_coord gui_smoke_findreplace gui_smoke_large_readonly gui_smoke_smallfixes gui_smoke_vault gui_smoke_secure_documents

echo.
echo === notepad-safe GUI Smoke Suite ===
echo.

REM -------- Step 1. build --------
pushd "%ROOT%"
if /I "%1"=="build" (
    call "%ROOT%build_incr.bat" %TARGETS% 2>&1
    if errorlevel 1 (
        echo. & echo [FATAL] Build failed. & exit /b 1
    )
    echo. & echo [OK] Build-only done. & popd & exit /b 0
)
if /I "%1"=="coord" (
    call "%ROOT%build_incr.bat" gui_smoke_coord 2>&1
    if errorlevel 1 ( echo [FATAL] build gui_smoke_coord failed & exit /b 1 )
    goto RUN_ONE_COORD
)
if /I "%1"=="fr" (
    call "%ROOT%build_incr.bat" gui_smoke_findreplace 2>&1
    if errorlevel 1 ( echo [FATAL] build gui_smoke_findreplace failed & exit /b 1 )
    goto RUN_ONE_FR
)

REM 默认：构建全部 + 运行全部
call "%ROOT%build_incr.bat" %TARGETS% 2>&1
if errorlevel 1 (
    echo. & echo [FATAL] Build failed (see build log above). & exit /b 1
)

:RUN_ALL
set PASS=0
set FAIL=0
for %%T in (%TARGETS%) do (
    echo.
    echo ------------------------------------------------------------
    echo Running %%T ...
    echo ------------------------------------------------------------
    pushd "%BUILD%"
    ".\%%T.exe" -o txt,%TEMP%\%%T.txt -xml,%TEMP%\%%T.junit.xml
    set EC=!errorlevel!
    popd
    if !EC! equ 0 (
        echo [PASS] %%T
        set /a PASS+=1
    ) else (
        echo [FAIL] %%T  (exit=!EC!  log=%TEMP%\%%T.txt)
        echo ---- tail ----
        powershell -NoProfile -Command "Get-Content '%TEMP%\%%T.txt' -ErrorAction SilentlyContinue | Select-Object -Last 80"
        echo --------------
        set /a FAIL+=1
    )
)

:FINISH
echo.
echo ============================================================
echo   GUI SMOKE SUMMARY
echo   PASS = %PASS%    FAIL = %FAIL%
echo ============================================================
if %FAIL% gtr 0 exit /b %FAIL%
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
