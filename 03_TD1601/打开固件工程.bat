@echo off
rem ===================================================================
rem  Open the TD1601 firmware project (it MUST stay inside the CDK SDK)
rem
rem  Why not copy the project here:
rem    The CDK build system resolves its dependencies by RELATIVE paths
rem    (../../components, ../../boards) and the generated Baozi.mk points
rem    at the local toolchain. Moving the project out of
rem    <SDK>\projects\ breaks both compile and flash.
rem
rem  Double-click this file to open the project folder in Explorer.
rem ===================================================================

set "PROJ=D:\Baozi\CDK\official_SDK\td1601_evb_v0_2_0_cdk_2026-06-16-18_41_59\td1601_evb_v0.2.0_cdk_2026-06-16-18_41_59\td1601_evb_v0.2.0_cdk\projects\baozi"

if not exist "%PROJ%" (
  echo [ERROR] Project folder not found:
  echo         %PROJ%
  echo.
  echo The SDK may have been moved or renamed. Update PROJ in this file.
  pause
  exit /b 1
)

start "" explorer "%PROJ%"
