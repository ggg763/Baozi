@echo off
rem ============================================================
rem  build_demo.bat - build & run TD1601 behavior layer demo
rem
rem  Usage: double-click this file, or run build_demo.bat in CMD
rem
rem  Note: the demo needs multiple source files and the inc/
rem  include dir. Compiling only the active file (VS Code
rem  "Run Active File") fails with:
rem     robot_types.h: No such file or directory
rem ============================================================

rem Switch to this script's directory (td1601_code/)
cd /d %~dp0

echo === build demo_td1601 ===
gcc -std=c99 -Wall -Wextra -Iinc -I. ^
    tests/demo_td1601.c ^
    src/uart_comm.c src/emotion_state.c src/behavior.c ^
    src/motion.c src/servo_control.c src/screen_display.c ^
    src/led_control.c ^
    hal/hal_pwm.c hal/hal_lcd.c hal/hal_led.c ^
    hal/hal_gpio.c hal/hal_uart.c ^
    -o tests\demo_td1601.exe

if errorlevel 1 (
    echo.
    echo [BUILD FAILED] make sure gcc is in PATH.
    pause
    exit /b 1
)

echo [BUILD OK] tests\demo_td1601.exe
echo.
echo === run demo ===
tests\demo_td1601.exe

echo.
pause
