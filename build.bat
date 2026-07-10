@echo off
REM SPDX-License-Identifier: BSD-3-Clause
REM Build PolyMesh (CLI + GUI) and copy binaries into the repo root.
REM Usage:  build.bat
REM         build.bat Debug
REM Requires: CMake >= 3.24, a C++20 compiler (MSVC 2022/2026 OK), Ninja recommended.
REM Windows deps (vcpkg): eigen3, nlohmann-json; for GUI also glad.
setlocal EnableExtensions EnableDelayedExpansion

set "ROOT=%~dp0"
cd /d "%ROOT%" || exit /b 1

set "BUILD_TYPE=Release"
if /I "%~1"=="Debug" set "BUILD_TYPE=Debug"
if /I "%~1"=="debug" set "BUILD_TYPE=Debug"

REM --- locate cmake / ninja (PATH first, then VS bundled tools) ---
set "CMAKE=cmake"
where cmake >nul 2>&1
if errorlevel 1 (
  if exist "%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
    set "CMAKE=%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
  ) else if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
    set "CMAKE=%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
  ) else (
    echo [polymesh] cmake not found. Install CMake or Visual Studio C++ workload.
    exit /b 1
  )
)

set "GEN=Ninja"
where ninja >nul 2>&1
if errorlevel 1 (
  if exist "%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" (
    set "PATH=%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
  ) else if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" (
    set "PATH=%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
  ) else (
    set "GEN=Visual Studio 17 2022"
  )
)

REM Load MSVC env when using Ninja (needs cl.exe on PATH).
where cl >nul 2>&1
if errorlevel 1 (
  if exist "%ProgramFiles%\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" (
    call "%ProgramFiles%\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
  ) else if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
    call "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
  )
)

REM vcpkg toolchain (Eigen, nlohmann_json, glad on Windows)
set "VCPKG_TOOLCHAIN="
if defined CMAKE_TOOLCHAIN_FILE set "VCPKG_TOOLCHAIN=-DCMAKE_TOOLCHAIN_FILE=%CMAKE_TOOLCHAIN_FILE%"
if not defined VCPKG_TOOLCHAIN if exist "%USERPROFILE%\vcpkg\scripts\buildsystems\vcpkg.cmake" (
  set "VCPKG_TOOLCHAIN=-DCMAKE_TOOLCHAIN_FILE=%USERPROFILE%\vcpkg\scripts\buildsystems\vcpkg.cmake"
)
if not defined VCPKG_TOOLCHAIN if exist "C:\vcpkg\scripts\buildsystems\vcpkg.cmake" (
  set "VCPKG_TOOLCHAIN=-DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake"
)

echo [polymesh] configure (%BUILD_TYPE%, generator=%GEN%)...
if /I "%GEN%"=="Ninja" (
  "%CMAKE%" -S . -B build -G Ninja ^
    -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
    %VCPKG_TOOLCHAIN% ^
    -DPOLYMESH_WITH_GUI=ON ^
    -DPOLYMESH_WITH_OCC=OFF ^
    -DPOLYMESH_WITH_CUDA=OFF ^
    -DPOLYMESH_WITH_OPENMP=ON ^
    -DPOLYMESH_NATIVE_ARCH=OFF ^
    -DPOLYMESH_ENABLE_LTO=OFF
) else (
  "%CMAKE%" -S . -B build -G "%GEN%" -A x64 ^
    -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
    %VCPKG_TOOLCHAIN% ^
    -DPOLYMESH_WITH_GUI=ON ^
    -DPOLYMESH_WITH_OCC=OFF ^
    -DPOLYMESH_WITH_CUDA=OFF ^
    -DPOLYMESH_WITH_OPENMP=ON ^
    -DPOLYMESH_NATIVE_ARCH=OFF ^
    -DPOLYMESH_ENABLE_LTO=OFF
)
if errorlevel 1 (
  echo [polymesh] configure failed
  echo [polymesh] On Windows, install deps with vcpkg:
  echo   vcpkg install eigen3:x64-windows nlohmann-json:x64-windows glad:x64-windows
  exit /b 1
)

echo [polymesh] build...
"%CMAKE%" --build build --config %BUILD_TYPE% -j
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
