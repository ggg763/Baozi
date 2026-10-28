@echo off
chcp 65001 >nul
echo ============================================
echo  Baozi Backend - allow inbound TCP 8000
echo  (must run as Administrator)
echo ============================================
echo.

net session >nul 2>&1
if %errorlevel% neq 0 (
  echo [ERROR] Not running as Administrator.
  echo Please right-click this file and choose "Run as administrator".
  pause
  exit /b 1
)

netsh advfirewall firewall delete rule name="BaoziBackend8000" >nul 2>&1
netsh advfirewall firewall add rule name="BaoziBackend8000" dir=in action=allow protocol=TCP localport=8000 profile=any

if %errorlevel% equ 0 (
  echo.
  echo [OK] Firewall rule added. Your phone can now reach this PC on port 8000.
) else (
  echo.
  echo [FAIL] Could not add the rule.
)
echo.
pause
