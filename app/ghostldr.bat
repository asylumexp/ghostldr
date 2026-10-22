@echo off
rem ghostldr app for Windows: double-click this file.
setlocal
cd /d "%~dp0"
set "PY="
py -3 -c "import sys; sys.exit(sys.version_info < (3, 11))" >nul 2>nul && set "PY=py -3"
if not defined PY python -c "import sys; sys.exit(sys.version_info < (3, 11))" >nul 2>nul && set "PY=python"
if not defined PY goto nopython
title ghostldr
%PY% launch.py %*
if errorlevel 1 pause
exit /b

:nopython
echo ghostldr needs Python 3.11 or newer.
echo Get it from https://www.python.org/downloads/ and tick "Add python.exe to PATH" when installing,
echo then double-click ghostldr.bat again.
start "" "https://www.python.org/downloads/windows/"
pause
exit /b 1
