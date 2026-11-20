@echo off
rem Run a command inside the Visual Studio 18 developer environment (cl, clang-cl,
rem cmake, ninja), propagating its exit code. From Git Bash, set MSYS_NO_PATHCONV=1
rem when passing /flags through this script.
rem   tools\devshell.cmd cmake --preset native-clang
setlocal
set "VSDEV=%ProgramFiles%\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VSDEV%" (
  echo devshell: %VSDEV% not found 1>&2
  exit /b 1
)
call "%VSDEV%" >nul 2>&1
%*
exit /b %ERRORLEVEL%
