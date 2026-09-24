@echo off
rem Fly Mode (standalone ASI): output asi\freecam\build\FlyMode.asi
where cl >nul 2>nul || call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
if not exist build mkdir build
set MH=..\..\tools\minhook
cl /nologo /std:c++17 /utf-8 /O1 /Gy /W3 /EHa /MD /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /I%MH%\include /Fo:build\ /LD ^
   freecam.cpp %MH%\src\buffer.c %MH%\src\hook.c %MH%\src\trampoline.c %MH%\src\hde\hde64.c ^
   user32.lib /link /OUT:build\FlyMode.asi
