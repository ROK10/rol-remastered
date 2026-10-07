@echo off
rem HD pass control: hd plan | status | run [phase ...] | stop | reset <phase>   (see hd_full.py)
"X:\AI\ComfyUI_windows_portable\python_embeded\python.exe" -u "%~dp0hd_full.py" %*
