@echo off
REM SPDX-License-Identifier: BSD-3-Clause
REM Build PolyMesh (CLI + GUI) and copy binaries into the repo root.
REM Usage:  build.bat
REM         build.bat Debug
REM Requires: CMake >= 3.24, a C++20 compiler, Ninja recommended.
setlocal EnableExtensions

set "ROOT=%~dp0"
cd /d "%ROOT%" || exit /b 1

set "BUILD_TYPE=Release"
if /I "%~1"=="Debug" set "BUILD_TYPE=Debug"
if /I "%~1"=="debug" set "BUILD_TYPE=Debug"

set "GEN=Ninja"
where ninja >nul 2>&1
if errorlevel 1 set "GEN=Visual Studio 17 2022"

echo [polymesh] configure (%BUILD_TYPE%, generator=%GEN%)...
REM Release = -O3; OpenMP + native arch + LTO for performance (no /fp:fast).
if /I "%GEN%"=="Ninja" (
  cmake -S . -B build -G Ninja ^
    -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
    -DPOLYMESH_WITH_GUI=ON ^
    -DPOLYMESH_WITH_OCC=OFF ^
    -DPOLYMESH_WITH_CUDA=OFF ^
    -DPOLYMESH_WITH_OPENMP=ON ^
    -DPOLYMESH_NATIVE_ARCH=OFF ^
    -DPOLYMESH_ENABLE_LTO=OFF
) else (
  cmake -S . -B build -G "%GEN%" -A x64 ^
    -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
    -DPOLYMESH_WITH_GUI=ON ^
    -DPOLYMESH_WITH_OCC=OFF ^
    -DPOLYMESH_WITH_CUDA=OFF ^
    -DPOLYMESH_WITH_OPENMP=ON ^
    -DPOLYMESH_NATIVE_ARCH=OFF ^
    -DPOLYMESH_ENABLE_LTO=OFF
)
if errorlevel 1 (
  echo [polymesh] configure failed
  exit /b 1
)

echo [polymesh] build...
cmake --build build --config %BUILD_TYPE% -j
if errorlevel 1 (
  echo [polymesh] build failed
  exit /b 1
)

REM Locate built executables (Ninja vs multi-config VS layouts).
set "CLI="
set "GUI="
if exist "build\apps\cli\polymesh.exe" set "CLI=build\apps\cli\polymesh.exe"
if exist "build\apps\cli\%BUILD_TYPE%\polymesh.exe" set "CLI=build\apps\cli\%BUILD_TYPE%\polymesh.exe"
if exist "build\apps\gui\polymesh-gui.exe" set "GUI=build\apps\gui\polymesh-gui.exe"
if exist "build\apps\gui\%BUILD_TYPE%\polymesh-gui.exe" set "GUI=build\apps\gui\%BUILD_TYPE%\polymesh-gui.exe"

if not defined CLI (
  echo [polymesh] could not find polymesh.exe under build\
  exit /b 1
)

copy /Y "%CLI%" "%ROOT%polymesh.exe" >nul
if defined GUI copy /Y "%GUI%" "%ROOT%polymesh-gui.exe" >nul

echo.
echo [polymesh] done. Binaries in repo root:
echo   %ROOT%polymesh.exe
if defined GUI echo   %ROOT%polymesh-gui.exe
echo.
echo Try:
echo   polymesh-gui.exe bench\geometries\public\unit_box.stl
echo   polymesh.exe mesh bench\geometries\public\unit_box.stl -o box.vtu
exit /b 0
