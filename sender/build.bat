@echo off
REM ---------------------------------------------------------------
REM  Build af_send.exe - one portable file, no install, no admin.
REM  Run this on Windows with Python 3.8+ installed.
REM ---------------------------------------------------------------

echo.
echo === ALWAYS FLASH sender - build ===
echo.

echo [1/3] Installing build dependencies...
python -m pip install --quiet --upgrade pyserial pyinstaller
if errorlevel 1 goto :err

echo [2/3] Building single-file executable...
REM --onefile     : everything in one .exe
REM --noconsole   : GUI only, no black console window
REM --strip / UPX : shrink it further if UPX is on PATH
python -m PyInstaller ^
  --onefile ^
  --noconsole ^
  --name af_send ^
  --exclude-module numpy ^
  --exclude-module PIL ^
  --exclude-module unittest ^
  --exclude-module pydoc ^
  --exclude-module email ^
  --exclude-module http ^
  --exclude-module xml ^
  af_send.py
if errorlevel 1 goto :err

echo [3/3] Done.
echo.
echo   Your executable:  dist\af_send.exe
echo   Copy it anywhere - it needs nothing else.
echo.
goto :eof

:err
echo.
echo BUILD FAILED. Make sure Python is installed and on PATH.
pause
