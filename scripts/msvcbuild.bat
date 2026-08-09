@echo off
REM Load the MSVC x64 environment then run whatever was passed in.
REM Used by tooling whose shell does not inherit vcvars (INCLUDE/LIB unset ->
REM "Cannot open include file: 'cmath'"). Mirrors build.bat's VS discovery.
setlocal
set "VCVARS="
for %%D in (
  "C:\Program Files\Microsoft Visual Studio\18\Community"
  "C:\Program Files\Microsoft Visual Studio\2022\Community"
  "C:\Program Files\Microsoft Visual Studio\2022\Professional"
  "C:\Program Files\Microsoft Visual Studio\2022\Enterprise"
  "C:\Program Files\Microsoft Visual Studio\2022\BuildTools"
) do (
  if exist "%%~D\VC\Auxiliary\Build\vcvars64.bat" (
    if not defined VCVARS set "VCVARS=%%~D\VC\Auxiliary\Build\vcvars64.bat"
  )
)
if not defined VCVARS (
  echo msvcbuild: no vcvars64.bat found 1>&2
  exit /b 1
)
call "%VCVARS%" >nul || exit /b 1
%*
exit /b %ERRORLEVEL%
