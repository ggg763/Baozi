@echo off
chcp 65001 >nul
rem ===================================================================
rem  Baozi - allow LAN inbound for the voice server
rem
rem  Opens:
rem    TCP 8765  PC voice server  <- ESP32 connects in
rem    UDP 8766   discovery       <- ESP32 broadcasts / PC answers
rem
rem  Why this is needed after switching hotspot:
rem    Windows treats a NEW Wi-Fi as "Public" by default, and the Public
rem    profile blocks inbound connections. Without these rules the ESP32
rem    can join the hotspot but never reach the PC - the symptom is
rem    "PC 一直显示等待 ESP32 连接" or "ESP32 一直在找 PC".
rem
rem  Must run as Administrator (right-click -> Run as administrator).
rem ===================================================================

net session >nul 2>&1
if %errorlevel% neq 0 (
  echo [ERROR] Not running as Administrator.
  echo         Right-click this file and choose "Run as administrator".
  pause
  exit /b 1
)

echo === Baozi firewall setup ===
echo.

netsh advfirewall firewall delete rule name="BaoziVoice8765" >nul 2>&1
netsh advfirewall firewall delete rule name="BaoziDiscovery8766" >nul 2>&1

netsh advfirewall firewall add rule name="BaoziVoice8765" dir=in action=allow protocol=TCP localport=8765 profile=any
netsh advfirewall firewall add rule name="BaoziDiscovery8766" dir=in action=allow protocol=UDP localport=8766 profile=any

if %errorlevel% neq 0 (
  echo.
  echo [FAIL] Could not add rules.
  pause
  exit /b 1
)

echo.
echo [OK] Inbound allowed: TCP 8765 (voice), UDP 8766 (discovery), all profiles.
echo.
echo Tip: to also make Windows treat this Wi-Fi as "Private"
echo      (optional, only if you still have trouble):
echo        Set-NetConnectionProfile -Name "YOUR-SSID" -NetworkCategory Private
echo.
pause
