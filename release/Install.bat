@echo off
rem Rise of Legends Remastered - puts the modpack into the game: modkit\tools\modmgr.py setup (modpack\release.ini).
rem Every file it changes is backed up into modkit\backup first; Uninstall.bat puts them back.
setlocal
cd /d "%~dp0"
chcp 65001 >nul
if not exist "legends.exe" (
    echo Unpack the archive into the game folder, next to legends.exe, and run Install.bat there.
    echo Распакуйте архив в папку игры, рядом с legends.exe, и запустите Install.bat оттуда.
    pause
    exit /b 1
)
tasklist /fi "imagename eq legends.exe" 2>nul | find /i "legends.exe" >nul
if not errorlevel 1 (
    echo Close the game first.
    echo Сначала закройте игру.
    pause
    exit /b 1
)
"modkit\python\python.exe" "modkit\tools\modmgr.py" setup
if errorlevel 1 (
    echo.
    echo Installation failed - see the messages above.
    echo Установка не удалась - см. сообщения выше.
    pause
    exit /b 1
)
echo.
echo Done. Start the game; the settings are in Options - Mods.
echo Готово. Запускайте игру; вкладка «Моды» - в окне «Настройки» (F11).
pause
