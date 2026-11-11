@echo off
setlocal
cd /d "%~dp0"
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" (
  echo vcvarsall.bat not found
  exit /b 1
)
if not exist build mkdir build
call "%VCVARS%" x86
if errorlevel 1 exit /b 1

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
