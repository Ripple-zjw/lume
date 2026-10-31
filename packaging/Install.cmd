@echo off
setlocal
taskkill /im Lume.exe /f >nul 2>&1
timeout /t 1 /nobreak >nul
set "DEST=%LOCALAPPDATA%\Programs\Lume"
if not exist "%DEST%" mkdir "%DEST%" 2>nul
copy /y "%~dp0Lume.exe" "%DEST%\Lume.exe" >nul
if errorlevel 1 (
  echo Failed to copy Lume.exe to "%DEST%".
  echo Copy Lume.exe to your Desktop or C:\Apps\Lume and run it there.
  pause
  exit /b 1
)
icacls "%DEST%\Lume.exe" /setintegritylevel Medium >nul 2>&1
start "" "%DEST%\Lume.exe"
exit /b 0