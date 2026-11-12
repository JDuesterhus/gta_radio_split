@echo off
setlocal
cd /d "%~dp0"
if not exist build mkdir build

rem CI sets up the x86 compiler before this script. Local shells find the
rem newest Visual Studio installation, or use VCVARS when it is set.
where cl >nul 2>nul
if not errorlevel 1 if /i "%VSCMD_ARG_TGT_ARCH%"=="x86" goto compile

if defined VCVARS (
  if not exist "%VCVARS%" (
    echo VCVARS is set but does not exist: %VCVARS%
    exit /b 1
  )
  call "%VCVARS%" x86
  goto check_compiler
)

set "VSPATH="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
  for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
)
if defined VSPATH if exist "%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat" (
  call "%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat" x86
  goto check_compiler
)
if exist "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" (
  call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x86
  goto check_compiler
)

echo vcvarsall.bat not found.
echo Install Visual Studio Build Tools with the "Desktop development with C++" workload (x86).
exit /b 1

:check_compiler
if errorlevel 1 exit /b 1
where cl >nul 2>nul
if errorlevel 1 (
  echo cl.exe is not on PATH after vcvarsall.
  exit /b 1
)

:compile
cl /nologo /std:c++17 /MT /EHsc /W3 /Isrc /Fobuild/ /Febuild/gate_test.exe ^
  src\split\gate.cpp src\split\byte_ring.cpp src\games\sa\version.cpp ^
  src\games\miles\radio_file.cpp src\games\miles\detour.cpp ^
  src\output\volume.cpp src\output\device_match.cpp tests\test_main.cpp
if errorlevel 1 exit /b 1
build\gate_test.exe
if errorlevel 1 exit /b 1

cl /nologo /std:c++17 /MT /O2 /EHsc /W3 /LD /Isrc /Ithird_party\miniaudio /Fobuild/ /FeRadioSplit.asi ^
  src\loader\asi_main.cpp src\common\log.cpp src\common\ini.cpp ^
  src\split\gate.cpp src\split\byte_ring.cpp src\games\sa\version.cpp src\games\sa\sa_radio.cpp ^
  src\games\miles\radio_file.cpp src\games\miles\detour.cpp src\games\miles\miles_radio.cpp ^
  src\output\volume.cpp src\output\device_match.cpp src\output\wasapi_out.cpp ^
  /link dsound.lib ole32.lib uuid.lib avrt.lib user32.lib
if errorlevel 1 exit /b 1

cl /nologo /std:c++17 /MT /O2 /EHsc /W3 /Isrc /Ithird_party\miniaudio /Fobuild/ /FeRadioSplit.exe ^
  src\player\player.cpp src\output\device_match.cpp ^
  /link /SUBSYSTEM:WINDOWS ole32.lib uuid.lib avrt.lib user32.lib
if errorlevel 1 exit /b 1

echo Built RadioSplit.asi and RadioSplit.exe
exit /b 0
