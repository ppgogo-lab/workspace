@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "ROOT=%~dp0.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"

set "EXE=%ROOT%\build-win-gui\optionmm_trader_gui.exe"
set "ENDPOINT=%~1"

if "%ENDPOINT%"=="" (
  set "WSL_IP="
  for /f "tokens=1" %%I in ('wsl hostname -I 2^>nul') do (
    if not defined WSL_IP set "WSL_IP=%%I"
  )

  if not defined WSL_IP (
    echo Failed to resolve WSL IP address.
    echo Start WSL and pass an explicit endpoint if needed, for example:
    echo   %~nx0 172.20.10.2:50051
    exit /b 1
  )

  set "ENDPOINT=!WSL_IP!:50051"
)

if not exist "%EXE%" (
  echo Missing GUI executable:
  echo   %EXE%
  echo Run scripts\build_windows_gui.cmd first.
  exit /b 1
)

pushd "%ROOT%" >nul
start "" "%EXE%" "%ENDPOINT%"
popd >nul
exit /b 0
