@echo off
REM =====================================================================
REM  flash.bat - write the prebuilt firmware to the board via esptool
REM
REM  Usage:  flash.bat COM5          (port is required)
REM          flash.bat               (lists available serial ports)
REM
REM  Uses Arduino15's bundled esptool, so Arduino IDE being installed is
REM  enough - no extra download, no board-menu settings involved.
REM =====================================================================
setlocal enabledelayedexpansion
chcp 65001 >nul 2>&1

set "HERE=%~dp0"
set "FW=%HERE%esp32_baozi\firmware"
set "PORT=%~1"

REM ---- locate esptool.exe (shipped with esp32 core) ----
set "ESPTOOL="
for /d %%D in ("%LOCALAPPDATA%\Arduino15\packages\esp32\tools\esptool_py\*") do (
  if exist "%%D\esptool.exe" set "ESPTOOL=%%D\esptool.exe"
)
if not defined ESPTOOL (
  echo [x] esptool.exe not found under:
  echo     %LOCALAPPDATA%\Arduino15\packages\esp32\tools\esptool_py
  echo     Install the esp32 core first ^(see README 3.4^).
  pause
  exit /b 1
)

if not exist "%FW%\esp32_baozi.ino.bin" (
  echo [x] firmware not found: %FW%\esp32_baozi.ino.bin
  echo     Run build.bat first.
  pause
  exit /b 1
)

if not defined PORT (
  echo.
  echo   Available serial ports:
  powershell -NoProfile -Command "Get-CimInstance Win32_PnPEntity | Where-Object { $_.Name -match 'COM\d+' } | ForEach-Object { '    ' + $_.Name }"
  echo.
  echo   Usage:  flash.bat COM5
  echo   Tip:    unplug the board, run this again, plug it in - the new port is it.
  pause
  exit /b 1
)

echo.
echo   port    : %PORT%
echo   esptool : %ESPTOOL%
echo   bins    : %FW%
echo.
echo   flashing ...
"%ESPTOOL%" --chip esp32s3 -p %PORT% -b 921600 write-flash ^
  --flash-mode dio --flash-freq 80m --flash-size 16MB ^
  0x0     "%FW%\esp32_baozi.ino.bootloader.bin" ^
  0x8000  "%FW%\esp32_baozi.ino.partitions.bin" ^
  0x10000 "%FW%\esp32_baozi.ino.bin"

if errorlevel 1 (
  echo.
  echo [x] flash FAILED.
  echo     - close the Arduino IDE serial monitor ^(it locks the port^)
  echo     - try a lower baud: edit -b 921600 to -b 460800
  pause
  exit /b 1
)

echo.
echo   OK - board rebooted. Open a 115200 serial monitor to see its log.
pause
