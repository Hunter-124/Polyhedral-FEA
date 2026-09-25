@echo off
REM Load the MSVC x64 environment, then run whatever was passed in, e.g.
REM   scripts\msvcbuild.bat cmake --build --preset windows-msvc
REM For tooling whose shell does not inherit vcvars (INCLUDE/LIB unset ->
REM "Cannot open include file: 'cmath'"). build.bat re-runs itself through this.
REM Note: vcvars points VCPKG_ROOT at VS's bundled manifest-mode vcpkg.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo msvcbuild: vswhere.exe not found; install Visual Studio 2022+ with the C++ workload 1>&2
  exit /b 1
)
set "VSDIR="
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%I"
if not defined VSDIR (
  echo msvcbuild: no Visual Studio with the x64 C++ tools found 1>&2
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
%*
exit /b %ERRORLEVEL%
