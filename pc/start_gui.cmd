@echo off
setlocal
rem Use desktop Python even when launched from an ESP-IDF terminal.
set "PYTHONHOME="
set "PYTHONPATH="
set "TCL_LIBRARY="
set "TK_LIBRARY="
set "GUI_PYTHON="
rem Some desktop installations are not registered with the py launcher.
for /d %%D in ("%LOCALAPPDATA%\Programs\Python\Python3*") do call :candidate "%%~fD\python.exe"
if defined GUI_PYTHON goto run
for /f "delims=" %%P in ('py -3 -c "import sys; print(sys.executable)" 2^>nul') do call :candidate "%%P"
if not defined GUI_PYTHON goto missing

:run
if /i "%~1"=="--check" (
    "%GUI_PYTHON%" -c "import sys, tkinter as tk, serial; r=tk.Tk(); r.withdraw(); r.destroy(); print('GUI ready: ' + sys.executable)"
    exit /b
)
"%GUI_PYTHON%" "%~dp0controller.py"
exit /b %errorlevel%

:candidate
if defined GUI_PYTHON exit /b
if not exist "%~1" exit /b
"%~1" -c "import tkinter, serial" >nul 2>&1
if not errorlevel 1 set "GUI_PYTHON=%~1"
exit /b

:missing
echo Desktop Python with Tcl/Tk and pyserial is required.
echo With desktop Python, run: python -m pip install -r "%~dp0requirements.txt"
echo ESP-IDF Python does not include Tkinter.
exit /b 1
