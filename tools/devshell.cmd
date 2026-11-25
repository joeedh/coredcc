@echo off
rem Run a command inside the Visual Studio 18 developer environment (cl, clang-cl,
rem cmake, ninja), propagating its exit code. From Git Bash, set MSYS_NO_PATHCONV=1
rem when passing /flags through this script.
rem   tools\devshell.cmd cmake --preset native-clang
setlocal
rem Any edition. vswhere is on the PATH so that its command line starts without a quote,
rem which cmd would strip.
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
set "VSROOT="
for /f "usebackq delims=" %%i in (`vswhere -latest -products * -version "[18.0,19.0)" -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT (
  echo devshell: no Visual Studio 18 with the C++ tools 1>&2
  exit /b 1
)
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
%*
exit /b %ERRORLEVEL%
