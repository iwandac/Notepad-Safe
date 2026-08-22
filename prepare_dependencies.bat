@echo off
setlocal

REM This script performs no network access. Runtime dependency sources must
REM already exist under third_party\src in a fresh clone. It verifies their
REM real on-disk content, then builds only generated libraries under .deps.
set "SCRIPT_DIR=%~dp0"

echo === Verifying pinned vendored source trees ===
powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%tools\verify_supply_chain.ps1"
if errorlevel 1 exit /b 1

echo === Preparing source-built QScintilla ===
powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%tools\bootstrap_qscintilla.ps1" -SkipSupplyChainVerification
if errorlevel 1 exit /b 1

echo === Preparing vendored PCRE2 ===
powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%tools\bootstrap_pcre2.ps1" -SkipSupplyChainVerification
if errorlevel 1 exit /b 1

if /I "%NOTEPADSAFE_WITH_KDBX%"=="OFF" (
    echo KDBX backend disabled by NOTEPADSAFE_WITH_KDBX=OFF
) else (
    echo === Preparing source-built crypto dependencies ===
    powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%tools\bootstrap_crypto_deps.ps1" -SkipSupplyChainVerification
    if errorlevel 1 exit /b 1
)

echo === LOCKED OFFLINE DEPENDENCIES READY ===
exit /b 0
