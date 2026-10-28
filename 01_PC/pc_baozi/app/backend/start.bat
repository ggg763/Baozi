@echo off
chcp 936 >nul
setlocal
title BaoZi Backend :8000

REM ============================================================
REM  BaoZi backend launcher (FastAPI / uvicorn)
REM  Rules for .bat in this project (see transfer doc 4.5):
REM    - pure ASCII only, no Chinese
REM    - must be CRLF
REM    - pause at the end so the window does not vanish
REM ============================================================

REM Self-locating: works no matter where this folder is copied to.
cd /d "%~dp0"

REM Python is located at run time so this folder works on any PC.
call "%~dp0..\_find_python.bat"
if errorlevel 1 goto :end
set "PYEXE=%PY%"
set "PORT=8000"

echo ============================================================
echo  BaoZi backend
echo  python : %PYEXE%
echo  listen : 0.0.0.0:%PORT%
echo ============================================================
echo.

echo [1/3] Python packages ...
"%PYEXE%" -c "import fastapi, uvicorn" >nul 2>&1
if errorlevel 1 (
  echo    missing fastapi/uvicorn, installing ...
  "%PYEXE%" -m pip install -r requirements.txt
  if errorlevel 1 (
    echo [ERROR] pip install failed. Check network / proxy.
    goto :end
  )
) else (
  echo    OK
)

echo [2/3] LAN address of this PC ...
for /f "usebackq tokens=2 delims=:" %%a in (`ipconfig ^| findstr /c:"IPv4"`) do (
  for /f "tokens=1" %%b in ("%%a") do echo    http://%%b:%PORT%
)
echo    ^> put that address into miniprogram app.js globalData.apiBase
echo    ^> phone and PC must be on the SAME WiFi
echo.

echo [3/3] Firewall inbound rule ...
netsh advfirewall firewall show rule name="BaoziBackend8000" >nul 2>&1
if errorlevel 1 (
  echo    rule not found. If the phone cannot connect, run
  echo    allow_firewall.bat AS ADMINISTRATOR once.
) else (
  echo    rule exists
)

echo.
echo ============================================================
echo  Starting server ... keep this window open
echo  Local check: http://127.0.0.1:%PORT%/api/v1/events
echo ============================================================
echo.

"%PYEXE%" -m uvicorn main:app --host 0.0.0.0 --port %PORT%

echo.
echo [server exited]
:end
echo.
pause >nul
