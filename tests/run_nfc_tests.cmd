@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
cl /nologo /W4 /std:c11 test_nfc.c ..\main\nfc_mcu.c /Fe:test_nfc.exe
if errorlevel 1 exit /b 1
test_nfc.exe
