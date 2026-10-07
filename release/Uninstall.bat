@echo off
rem Rise of Legends Remastered - takes the modpack out of the game: modkit\tools\modmgr.py uninstall-all puts back
rem every original file it backed up and removes every file it added (the modkit folder can be deleted afterwards).
setlocal
cd /d "%~dp0"
chcp 65001 >nul
tasklist /fi "imagename eq legends.exe" 2>nul | find /i "legends.exe" >nul
if not errorlevel 1 (
    echo Close the game first.
    echo Сначала закройте игру.
    pause
    exit /b 1
)
"modkit\python\python.exe" "modkit\tools\modmgr.py" uninstall-all
if errorlevel 1 (
    echo.
    echo Uninstalling failed - see the messages above.
    echo Удаление не удалось - см. сообщения выше.
    pause
    exit /b 1
)
echo.
echo Done: the game is as it was before. The modkit folder, Install.bat and Uninstall.bat can be deleted now.
echo Готово: игра такая же, как до установки. Папку modkit, Install.bat и Uninstall.bat теперь можно удалить.
pause
