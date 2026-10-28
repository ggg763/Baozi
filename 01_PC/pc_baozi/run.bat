@echo off
rem ===================================================================
rem  BaoZi PC launcher  (one process: voice + vision + emotion + app)
rem
rem    run.bat                  full start
rem    run.bat --no-app         voice/vision/emotion only
rem    run.bat --no-vision      no camera
rem    run.bat --transport serial   ESP32 over USB serial
rem
rem  Replaces the old two-window setup (backend 8000 + bridge 8765):
rem  main.py now starts the backend child process and the screen bridge
rem  thread itself. Closing this window stops everything.
rem ===================================================================
setlocal
cd /d "%~dp0"

if not exist ".venv\Scripts\python.exe" (
  echo [ERROR] venv not found: .venv\Scripts\python.exe
  echo         python -m venv .venv
  echo         .venv\Scripts\python.exe -m pip install -r requirements.txt
  pause
  exit /b 2
)

".venv\Scripts\python.exe" main.py %*
set RC=%ERRORLEVEL%
if not "%RC%"=="0" (
  echo.
  echo [exit code %RC%]
  pause
)
exit /b %RC%
