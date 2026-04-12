@echo off
setlocal

set "ROOT=%~dp0.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"

set "EXE=%ROOT%\build-win-gui\optionmm_trader_gui.exe"
set "ENDPOINT=%~1"

if "%ENDPOINT%"=="" set "ENDPOINT=127.0.0.1:50051"

if not exist "%EXE%" (
  echo Missing GUI executable:
  echo   %EXE%
  echo Run scripts\build_windows_gui.cmd first.
  exit /b 1
)

start "" "%EXE%" "%ENDPOINT%"
exit /b 0
