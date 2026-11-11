@echo off
rem One-step setup for Windows: builds the co-op mod from YOUR OWN copy of the
rem game and installs it into Ryujinx.
rem
rem   setup.bat "<base game .nsp or .xci>" "<1.9.0 update .nsp>"
rem
rem You can also drag the two files onto this file together.
rem For a modded Switch, add:  --switch <folder>   (see docs\SWITCH.md)
rem Everything it does is in tools\coop_setup.py; this only finds Python.
setlocal
set "HERE=%~dp0"
set "SCRIPT=%HERE%coop_setup.py"
if not exist "%SCRIPT%" set "SCRIPT=%HERE%tools\coop_setup.py"

where py >nul 2>nul
if not errorlevel 1 goto usepy
where python >nul 2>nul
if not errorlevel 1 goto usepython
echo Setup stopped: Python isn't installed.
goto nopython

:usepy
py -3 "%SCRIPT%" %*
goto done

:usepython
python "%SCRIPT%" %*
goto done

:nopython
echo.
echo If Python wasn't found: get Python 3.12 from https://www.python.org/downloads/
echo and tick "Add python.exe to PATH" in its installer. Then run this again.

:done
echo.
pause
