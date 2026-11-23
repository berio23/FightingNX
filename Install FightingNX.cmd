@echo off
where pyw.exe >nul 2>nul
if not errorlevel 1 (
    start "" pyw.exe -3 "%~dp0tools\win\installer.py"
    exit /b
)
where pythonw.exe >nul 2>nul
if not errorlevel 1 (
    start "" pythonw.exe "%~dp0tools\win\installer.py"
    exit /b
)
echo Install Python 3.10 or newer with Tcl/Tk, then run this launcher again.
pause
