@echo off
setlocal

set ROOT=%~dp0
set OUT=%ROOT%build
set SRC=%ROOT%src\main.cpp
set VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat

if not exist "%OUT%" mkdir "%OUT%"

call "%VCVARS%" >nul
if errorlevel 1 exit /b 1

cl /nologo /std:c++17 /EHsc /O2 /utf-8 /DUNICODE /D_UNICODE "%SRC%" ^
  /Fe:"%OUT%\TBH_Companion.exe" ^
  /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib comctl32.lib winhttp.lib shell32.lib advapi32.lib bcrypt.lib

endlocal
