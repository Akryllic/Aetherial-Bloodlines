@echo off
setlocal
rem Builds the plugin with the MSVC environment set. Without vcvars64 the compiler cannot
rem find <array>, <algorithm> or <cstdint>, which reads as broken code and is not.
rem   build.bat                       build build/release
rem   build.bat debugpanel            build build/debugpanel
rem   build.bat release configure     re-run the configure preset first
rem Configure is needed after adding a .cpp, because sources are globbed at configure time
rem and a new translation unit otherwise goes missing from the link as LNK2001.

set "PRESET=%~1"
if not defined PRESET set "PRESET=release"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSPATH="
if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
)
if not defined VSPATH (
  echo build: no Visual Studio install with the C++ toolset found.
  exit /b 1
)

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 (
  echo build: vcvars64.bat reported failure.
  exit /b 1
)
if not defined INCLUDE (
  echo build: INCLUDE is unset, so the MSVC environment did not initialise.
  exit /b 1
)

cd /d "%~dp0"

if /i "%~2"=="configure" (
  cmake --preset %PRESET%
  if errorlevel 1 exit /b 1
)

if not exist "build/%PRESET%" (
  echo build: build/%PRESET% does not exist. Run: build.bat %PRESET% configure
  exit /b 1
)

cmake --build "build/%PRESET%"
exit /b %ERRORLEVEL%
