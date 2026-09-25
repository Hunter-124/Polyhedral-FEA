@echo off
REM SPDX-License-Identifier: BSD-3-Clause
REM Build PolyMesh (CLI + GUI) with the CMake presets and copy the binaries into
REM the repo root.
REM Usage:  build.bat          preset windows-msvc       (build\)
REM         build.bat Debug    preset windows-msvc-debug (build-debug\)
REM Needs: CMake 3.25+, Ninja, MSVC 2022/2026, and a classic-mode vcpkg with
REM eigen3, nlohmann-json, glad and opencascade (x64-windows).
setlocal EnableExtensions

set "ROOT=%~dp0"
cd /d "%ROOT%" || exit /b 1

REM No compiler on PATH: re-run inside the MSVC x64 environment. msvcbuild.bat
REM owns Visual Studio discovery; its vcvars also puts VS's cmake/ninja on PATH.
where cl >nul 2>&1
if errorlevel 1 (
  if defined POLYMESH_IN_MSVCBUILD (
    echo [polymesh] cl.exe not on PATH even after vcvars64.bat
    exit /b 1
  )
  set "POLYMESH_IN_MSVCBUILD=1"
  call "%ROOT%scripts\msvcbuild.bat" call "%~f0" %*
  exit /b
)

where cmake >nul 2>&1
if errorlevel 1 (
  echo [polymesh] cmake not found. Install CMake or the Visual Studio "C++ CMake tools" component.
  exit /b 1
)
where ninja >nul 2>&1
if errorlevel 1 (
  echo [polymesh] ninja not found. Install Ninja or the Visual Studio "C++ CMake tools" component.
  exit /b 1
)

REM The windows-msvc preset takes its toolchain from VCPKG_ROOT. vcvars points
REM VCPKG_ROOT at VS's bundled manifest-mode vcpkg, so pick the classic install
REM here; an explicit CMAKE_TOOLCHAIN_FILE in the environment wins.
set "TOOLCHAIN_ARG="
if defined CMAKE_TOOLCHAIN_FILE set "TOOLCHAIN_ARG=-DCMAKE_TOOLCHAIN_FILE=%CMAKE_TOOLCHAIN_FILE%"
set "VCPKG_ROOT="
if exist "%USERPROFILE%\vcpkg\scripts\buildsystems\vcpkg.cmake" set "VCPKG_ROOT=%USERPROFILE%\vcpkg"
if not defined VCPKG_ROOT if exist "C:\vcpkg\scripts\buildsystems\vcpkg.cmake" set "VCPKG_ROOT=C:\vcpkg"
if not defined VCPKG_ROOT if not defined TOOLCHAIN_ARG (
  echo [polymesh] vcpkg not found at %USERPROFILE%\vcpkg or C:\vcpkg; set CMAKE_TOOLCHAIN_FILE.
  goto :deps_hint
)

set "PRESET=windows-msvc"
set "BIN=build"
if /I "%~1"=="Debug" (
  set "PRESET=windows-msvc-debug"
  set "BIN=build-debug"
)

echo [polymesh] configure (preset %PRESET%)...
cmake --preset %PRESET% %TOOLCHAIN_ARG%
if errorlevel 1 (
  echo [polymesh] configure failed
  goto :deps_hint
)

echo [polymesh] build...
cmake --build --preset %PRESET%
if errorlevel 1 (
  echo [polymesh] build failed
  exit /b 1
)

copy /Y "%BIN%\apps\cli\polymesh.exe" "%ROOT%polymesh.exe" >nul || exit /b 1
copy /Y "%BIN%\apps\gui\polymesh-gui.exe" "%ROOT%polymesh-gui.exe" >nul || exit /b 1

echo.
echo [polymesh] done. Binaries in repo root:
echo   %ROOT%polymesh.exe
echo   %ROOT%polymesh-gui.exe
echo.
echo Try:
echo   polymesh-gui.exe bench\geometries\public\unit_box.step
echo   polymesh.exe mesh bench\geometries\public\unit_box.step -o box.vtu
exit /b 0

:deps_hint
echo [polymesh] On Windows, install deps with vcpkg:
echo   vcpkg install eigen3:x64-windows nlohmann-json:x64-windows glad:x64-windows opencascade:x64-windows
exit /b 1
