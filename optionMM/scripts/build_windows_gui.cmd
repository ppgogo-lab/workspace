@echo off
setlocal

set "ROOT=%~dp0.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"

set "VS_VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set "CMAKE_EXE=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "NINJA_DIR=%LOCALAPPDATA%\Microsoft\WinGet\Packages\Ninja-build.Ninja_Microsoft.Winget.Source_8wekyb3d8bbwe"
set "QT_DIR=C:\Qt\6.8.3\msvc2022_64"
set "VCPKG_ROOT=%ROOT%\..\vcpkg"
set "BUILD_DIR=%ROOT%\build-win-gui"

if not exist "%VS_VCVARS%" (
  echo Missing MSVC environment script:
  echo   %VS_VCVARS%
  exit /b 1
)

if not exist "%CMAKE_EXE%" (
  echo Missing CMake:
  echo   %CMAKE_EXE%
  exit /b 1
)

if not exist "%QT_DIR%\lib\cmake\Qt6\Qt6Config.cmake" (
  echo Missing Qt6 installation:
  echo   %QT_DIR%
  exit /b 1
)

if not exist "%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" (
  echo Missing vcpkg toolchain:
  echo   %VCPKG_ROOT%
  exit /b 1
)

call "%VS_VCVARS%" || exit /b 1
set "PATH=%NINJA_DIR%;%PATH%"
set "CXX=cl"

"%CMAKE_EXE%" -S "%ROOT%" -B "%BUILD_DIR%" -G Ninja ^
  -DOMM_BUILD_TRADER_GUI=ON ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_CXX_COMPILER=cl ^
  -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows ^
  -DQt6_DIR=%QT_DIR%\lib\cmake\Qt6 || exit /b 1

"%CMAKE_EXE%" --build "%BUILD_DIR%" --config Release --target optionmm_trader_gui -j 8 || exit /b 1

if exist "%QT_DIR%\bin\windeployqt.exe" (
  "%QT_DIR%\bin\windeployqt.exe" --release --no-translations --no-system-d3d-compiler --dir "%BUILD_DIR%" "%BUILD_DIR%\optionmm_trader_gui.exe"
)

for %%F in (
  libprotobuf.dll
  re2.dll
  zlib1.dll
  cares.dll
  libssl-3-x64.dll
  libcrypto-3-x64.dll
  abseil_dll.dll
) do (
  if exist "%VCPKG_ROOT%\installed\x64-windows\bin\%%F" copy /Y "%VCPKG_ROOT%\installed\x64-windows\bin\%%F" "%BUILD_DIR%\" >nul
)

echo Built:
echo   %BUILD_DIR%\optionmm_trader_gui.exe
exit /b 0
