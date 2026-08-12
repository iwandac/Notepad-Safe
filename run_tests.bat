@echo off
setlocal enabledelayedexpansion

REM ============================================================================
REM  Notepad Safe - run all tests (direct exe + CTest double check)
REM  Paths are relative to the script dir (%~dp0)
REM ============================================================================

set "SCRIPT_DIR=%~dp0"
set "BUILD_DIR=%SCRIPT_DIR%build"

if not defined NOTEPADSAFE_VS_ROOT (
    set "NOTEPADSAFE_VS_ROOT=C:\Program Files\Microsoft Visual Studio\18\Insiders"
)
set "VS_CTEST=%NOTEPADSAFE_VS_ROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"

if not exist "%BUILD_DIR%\test_textencoding.exe" (
    echo ERROR: Tests not built. Run build.bat first.
    exit /b 1
)

set PASS=0
set FAIL=0

echo.
echo ============ Running Exe Tests Directly ============
echo.

echo --- TextEncoding Tests ---
"%BUILD_DIR%\test_textencoding.exe"
if !errorlevel! == 0 (
    set /a PASS+=1
    echo [PASS] test_textencoding
) else (
    set /a FAIL+=1
    echo [FAIL] test_textencoding
)
echo.

echo --- Search Logic Tests ---
"%BUILD_DIR%\test_search.exe"
if !errorlevel! == 0 (
    set /a PASS+=1
    echo [PASS] test_search
) else (
    set /a FAIL+=1
    echo [FAIL] test_search
)
echo.

echo --- SessionManager Tests ---
"%BUILD_DIR%\test_session.exe"
if !errorlevel! == 0 (
    set /a PASS+=1
    echo [PASS] test_session
) else (
    set /a FAIL+=1
    echo [FAIL] test_session
)
echo.

echo --- Settings / UI Sync Tests ---
"%BUILD_DIR%\test_settings_ui.exe"
if !errorlevel! == 0 (
    set /a PASS+=1
    echo [PASS] test_settings_ui
) else (
    set /a FAIL+=1
    echo [FAIL] test_settings_ui
)
echo.

echo --- LargeFile Tests ---
"%BUILD_DIR%\test_largefile.exe"
if !errorlevel! == 0 (
    set /a PASS+=1
    echo [PASS] test_largefile
) else (
    set /a FAIL+=1
    echo [FAIL] test_largefile
)
echo.

echo ============ Running CTest ============
pushd "%BUILD_DIR%"
if exist "%VS_CTEST%" (
    "%VS_CTEST%" --output-on-failure
    set CTEST_RC=!errorlevel!
) else (
    ctest --output-on-failure
    set CTEST_RC=!errorlevel!
)
popd

echo.
echo ============ Summary ============
echo   Direct exe : %PASS% passed, %FAIL% failed
echo   CTest      : rc=%CTEST_RC%
echo.

if %FAIL% gtr 0 exit /b 1
if not "%CTEST_RC%"=="0" exit /b %CTEST_RC%
exit /b 0
