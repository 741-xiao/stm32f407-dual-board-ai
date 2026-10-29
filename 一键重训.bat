@echo off
setlocal EnableExtensions DisableDelayedExpansion

rem ================================================================
rem  Lab7 one-click retrain  (just double-click)
rem
rem  This file adapts itself to the current machine:
rem    1) finds the project root (the folder containing tools\retrain.py)
rem       -- works even if you put this .bat one level ABOVE Lab7
rem    2) finds a python that can import numpy/tensorflow/onnx
rem       -- managed venv -> other installs -> py -3 -> python on PATH
rem    3) finds Keil UV4
rem       -- %UV4_PATH% -> C:\Keil -> C:\Keil_v5 -> D/E/F:\Keil*
rem    4) runs tools\retrain.py with this project as the working dir
rem
rem  usage:
rem    double-click .............. export -> train -> gen C -> build -> check
rem    this file --check ......... print an environment report only
rem    this file --skip-export ... reuse ai\dataset\lab7.csv (no MySQL)
rem    this file --skip-sep ...... skip the separability gate
rem
rem  NOTE: keep this file pure ASCII + CRLF -- cmd mangles Chinese and
rem        LF-only line endings inside .bat. Chinese messages come from
rem        tools\retrain.py, which switches the console to UTF-8 itself.
rem ================================================================

rem ---------- switch console to UTF-8 BEFORE parsing the rest ----------
rem  (this file is pure ASCII, so byte offsets do not change and cmd
rem   keeps reading the following lines correctly)
chcp 65001 >nul 2>&1

rem ---------- 1) project root ----------
set "ROOT=%~dp0"
if exist "%ROOT%tools\retrain.py" goto root_ok
if exist "%ROOT%Lab7\tools\retrain.py" set "ROOT=%ROOT%Lab7\" & goto root_ok
echo.
echo  [FAIL] tools\retrain.py not found under:
echo         %ROOT%
echo.
echo  Put this .bat in the Lab7 folder (the one with tools\ pc\ firmware\),
echo  or in its parent folder.
echo.
pause
exit /b 2
:root_ok
cd /d "%ROOT%"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

rem ---------- 2) python with the ML packages ----------
set "PY="
set "PY_OK="
rem  the project's managed venv comes FIRST: that is where the ML packages live
call :probe "%USERPROFILE%\.workbuddy\binaries\python\envs\default\Scripts\python.exe"
call :probe "%USERPROFILE%\.workbuddy\binaries\python\versions\3.13.12\python.exe"
for /d %%D in ("%LOCALAPPDATA%\Programs\Python\Python3*") do call :probe "%%~fD\python.exe"
for /d %%D in ("C:\Program Files\Python3*") do call :probe "%%~fD\python.exe"
rem  then fall back to the py launcher / python on PATH
for /f "delims=" %%P in ('py -3 -c "import sys;print(sys.executable)" 2^>nul') do call :probe "%%P"
for /f "delims=" %%P in ('python -c "import sys;print(sys.executable)" 2^>nul') do call :probe "%%P"
if defined PY_OK set "PY=%PY_OK%"

if not defined PY (
    echo.
    echo  [FAIL] no python found.
    echo         install Python 3.11+ and tick "Add python.exe to PATH",
    echo         then run this file again.
    echo.
    pause
    exit /b 3
)

rem ---------- 3) Keil UV4 ----------
set "UV4="
if defined UV4_PATH if exist "%UV4_PATH%" set "UV4=%UV4_PATH%"
for %%D in (C D E F G) do if not defined UV4 if exist "%%D:\Keil\UV4\UV4.exe" set "UV4=%%D:\Keil\UV4\UV4.exe"
for %%D in (C D E F G) do if not defined UV4 if exist "%%D:\Keil_v5\UV4\UV4.exe" set "UV4=%%D:\Keil_v5\UV4\UV4.exe"
for %%D in (C D E F G) do if not defined UV4 if exist "%%D:\MDK\UV4\UV4.exe" set "UV4=%%D:\MDK\UV4\UV4.exe"
if defined UV4 set "UV4_PATH=%UV4%"

rem ---------- 4) banner (paths actually used) ----------
echo.
echo ================================================================
echo   Lab7  one-click retrain
echo   export  -^>  train  -^>  gen C  -^>  build  -^>  check
echo ================================================================
echo   project : %ROOT%
echo   python  : %PY%
if defined PY_OK (echo             deps OK) else (echo             deps MISSING -- run --check)
if defined UV4  (echo   keil    : %UV4%) else (echo   keil    : not found -- build step will be skipped)
echo ================================================================
echo.

rem ---------- 5) hand over ----------
if /i "%~1"=="--check" goto do_check

"%PY%" tools\retrain.py --fast --build --yes %*
set "RC=%ERRORLEVEL%"

echo.
echo ----------------------------------------------------------------
if "%RC%"=="0" (
    echo  [OK] done. flash these two:
    echo       firmware\MDK-ARM\board1\board1.hex
    echo       firmware\MDK-ARM\board2\board2.hex
    echo.
    echo  remember: keep the same board placement when demonstrating.
) else (
    echo  [FAIL] exit code %RC%
    echo  scroll up for the first ERROR line.
    echo.
    echo  common causes:
    echo    - missing python packages -- run:  "%~nx0" --check
    echo    - no data in MySQL -- init_db.py / or use --skip-export
    echo    - Keil UV4 not found -- set env var UV4_PATH
    echo    - stedgeai not found -- install X-CUBE-AI Utilities in CubeMX
)
echo ----------------------------------------------------------------
echo.
pause
exit /b %RC%

rem ================================================================
rem  --check: environment report only (no training, no file changes)
rem ================================================================
:do_check
"%PY%" tools\retrain.py --check
set "RC=%ERRORLEVEL%"
echo.
pause
exit /b %RC%

rem ================================================================
rem  :probe  <absolute path to python.exe>
rem  first that runs  -> PY
rem  first with deps  -> PY_OK
rem ================================================================
:probe
if defined PY_OK goto :eof
if "%~1"=="" goto :eof
if not exist "%~1" goto :eof
"%~1" -c "import sys" >nul 2>&1
if errorlevel 1 goto :eof
if not defined PY set "PY=%~1"
"%~1" -c "import numpy, tensorflow, onnx" >nul 2>&1
if not errorlevel 1 set "PY_OK=%~1"
goto :eof
