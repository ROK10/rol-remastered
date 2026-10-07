@echo off
rem Builds rolhook (ddraw.dll proxy) - the full build with the developer tools and the players' build - and the
rem test harnesses: x86, static CRT.
setlocal
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
set OUT=..\..\build\native
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
if not exist "%OUT%\obj_smoke" mkdir "%OUT%\obj_smoke"
rem /Oy- keeps frame pointers so rolhook's own frames are walkable in stack traces; /utf-8: sources and log text are UTF-8
cl /nologo /c /O2 /Oy- /MT /W3 /EHsc /std:c++17 /utf-8 /DNDEBUG /Fo"%OUT%\obj\\" main.cpp log.cpp util.cpp files.cpp gfx.cpp crash.cpp ui.cpp camera.cpp capture.cpp probe.cpp uiscale.cpp uilayout.cpp scriptui.cpp motion.cpp calls.cpp anim.cpp shaderfx.cpp shadergen.cpp ssao.cpp ao.cpp post.cpp lod.cpp window.cpp shots.cpp shadow.cpp || exit /b 1
cl /nologo /c /O2 /MT /W3 /Fo"%OUT%\obj\\" vtidx.c || exit /b 1
link /nologo /DLL /DEF:ddraw.def /OUT:"%OUT%\ddraw.dll" "%OUT%\obj\*.obj" user32.lib advapi32.lib dbghelp.lib psapi.lib ole32.lib oleaut32.lib windowscodecs.lib || exit /b 1
rem the players' build, the one the release ships (tools\release.py): no developer tools - probes, test runs, frame
rem capture, call counting, motion and script traces, the sprite log, memory dumps (ROLHOOK_PLAYER, player_stubs.cpp)
if not exist "%OUT%\player\obj" mkdir "%OUT%\player\obj"
cl /nologo /c /O2 /Oy- /MT /W3 /EHsc /std:c++17 /utf-8 /DNDEBUG /DROLHOOK_PLAYER /Fo"%OUT%\player\obj\\" main.cpp log.cpp util.cpp files.cpp gfx.cpp crash.cpp ui.cpp camera.cpp uiscale.cpp uilayout.cpp anim.cpp shaderfx.cpp shadergen.cpp ssao.cpp ao.cpp post.cpp lod.cpp window.cpp shadow.cpp player_stubs.cpp || exit /b 1
cl /nologo /c /O2 /MT /W3 /Fo"%OUT%\player\obj\\" vtidx.c || exit /b 1
link /nologo /DLL /DEF:ddraw.def /OUT:"%OUT%\player\ddraw.dll" "%OUT%\player\obj\*.obj" user32.lib advapi32.lib dbghelp.lib psapi.lib ole32.lib oleaut32.lib || exit /b 1
cl /nologo /O2 /MT /W3 /EHsc /std:c++17 /Fo"%OUT%\obj_smoke\\" /Fe"%OUT%\smoke.exe" smoke.cpp d3d9.lib user32.lib || exit /b 1
rem the shader rewrites checked offline on dumped shaders (shadergen.cpp, the same code as in rolhook)
if not exist "%OUT%\obj_bench" mkdir "%OUT%\obj_bench"
cl /nologo /O2 /MT /W3 /EHsc /std:c++17 /utf-8 /Fo"%OUT%\obj_bench\\" /Fe"%OUT%\shadertest.exe" shadertest.cpp shadergen.cpp ao.cpp post.cpp d3d9.lib user32.lib || exit /b 1
echo BUILD OK
