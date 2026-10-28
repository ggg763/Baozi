@echo off
REM =====================================================================
REM  build.bat - compile esp32_baozi with a PINNED board configuration
REM
REM  Why this exists:
REM    The board settings live in the Arduino IDE's UI, not in the sketch.
REM    If the IDE is on the wrong board (e.g. "Arduino Uno" by default, or an
REM    ESP32-C3/C6 target), the build fails at "#include esp_camera.h" because
REM    only esp32 / esp32s2 / esp32s3 targets ship the camera component.
REM    This script pins the FQBN so the result never depends on the UI.
REM
REM  Usage: double-click, or run "build.bat" from a terminal.
REM  Output: esp32_baozi\firmware\*.bin (previous files are overwritten)
REM =====================================================================
setlocal
chcp 65001 >nul 2>&1

set "HERE=%~dp0"
set "SKETCH=%HERE%esp32_baozi"
set "OUTDIR=%TEMP%\baozi_build"

REM ---- locate arduino-cli (Arduino IDE 2.x bundles one) ----
set "CLI="
if exist "%LOCALAPPDATA%\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe" (
  set "CLI=%LOCALAPPDATA%\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
)
if not defined CLI (
  for /f "delims=" %%i in ('where arduino-cli 2^>nul') do set "CLI=%%i"
)
if not defined CLI (
  echo [x] arduino-cli not found.
  echo     Install Arduino IDE 2.x, or put arduino-cli.exe on PATH.
  pause
  exit /b 1
)

REM ---- core data dir (where esp32 core is installed) ----
set "ARDUINO_DIRECTORIES_DATA=%LOCALAPPDATA%\Arduino15"

REM ---- pinned board: ESP32S3 Dev Module / 16MB / OPI PSRAM / 3MB APP / USB CDC ----
set "FQBN=esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc"

echo.
echo   arduino-cli : %CLI%
echo   sketch      : %SKETCH%
echo   board       : %FQBN%
echo.

if not exist "%SKETCH%\esp32_baozi.ino" (
  echo [x] sketch not found: %SKETCH%\esp32_baozi.ino
  pause
  exit /b 1
)

REM ---- build cache self-heal ----
REM  arduino-cli reuses the .o files in %OUTDIR%. When the source file SET changes
REM  (a .h appears/disappears, e.g. the first time wifi_secrets.h shows up) the old
REM  cache can corrupt the compile command line:
REM      fatal error: cannot specify '-o' with '-c', '-S' or '-E' with multiple files
REM  That is not a code problem - wiping the cache fixes it. We fingerprint the file
REM  list: unchanged -> incremental build (fast); changed -> wipe once.
if /i "%~1"=="clean" (
  echo [i] clean: wiping build cache
  rmdir /s /q "%OUTDIR%" 2>nul
)

set "LISTOLD=%TEMP%\baozi_srclist.old"
set "LISTNEW=%TEMP%\baozi_srclist.new"
(dir /b "%SKETCH%\*.h" "%SKETCH%\*.cpp" "%SKETCH%\*.ino" 2>nul) > "%LISTNEW%"
if not exist "%LISTOLD%" goto :cache_ready
fc /b "%LISTOLD%" "%LISTNEW%" >nul 2>&1
if not errorlevel 1 goto :cache_ready
echo [i] source list changed - wiping build cache
rmdir /s /q "%OUTDIR%" 2>nul
:cache_ready
copy /y "%LISTNEW%" "%LISTOLD%" >nul

echo [1/2] compiling ...
"%CLI%" compile --fqbn "%FQBN%" --additional-urls https://arduino.me/packages/esp32.json --output-dir "%OUTDIR%" "%SKETCH%"
if errorlevel 1 (
  echo.
  echo [x] build FAILED - see messages above
  pause
  exit /b 1
)

echo.
echo [2/2] copying bins to %SKETCH%\firmware ...
copy /y "%OUTDIR%\esp32_baozi.ino.bin"              "%SKETCH%\firmware\" >nul
copy /y "%OUTDIR%\esp32_baozi.ino.bootloader.bin"   "%SKETCH%\firmware\" >nul
copy /y "%OUTDIR%\esp32_baozi.ino.partitions.bin"   "%SKETCH%\firmware\" >nul
if errorlevel 1 (
  echo [x] copy failed
  pause
  exit /b 1
)

echo.
echo   OK - firmware updated:
dir /b "%SKETCH%\firmware\*.bin"
echo.
echo   Next: flash.bat COMx   (or Arduino IDE -^> Upload)
pause
