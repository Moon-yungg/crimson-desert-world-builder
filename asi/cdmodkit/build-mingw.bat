@echo off
rem World Builder with GCC / MinGW-w64 (x86_64, UCRT or MSVCRT runtime). MSVC stays the reference toolchain (build.bat);
rem this script exists so that people without Visual Studio can build the same sources. g++ must be on PATH, or set GXX.
rem Needs: tools\minhook, tools\imgui (v1.91.5), python with the lz4 module (prefab index), windres (part of MinGW).
setlocal
cd /d "%~dp0"
if "%GXX%"=="" set GXX=g++
if "%GCC%"=="" set GCC=gcc
if "%WINDRES%"=="" set WINDRES=windres
if not exist build mkdir build
python ..\..\scripts\check_locales.py || exit /b 1
python ..\..\scripts\pack_index.py || exit /b 1
set MH=..\..\tools\minhook
set IM=..\..\tools\imgui
python ..\..\scripts\patch_minhook.py "%MH%\src\trampoline.c" || exit /b 1
set CXXFLAGS=-std=c++17 -O2 -w -DNDEBUG -D_CRT_SECURE_NO_WARNINGS -DMINGW_HAS_SECURE_API=1 -DIMGUI_DISABLE_OBSOLETE_FUNCTIONS=0 -DIMGUI_USER_CONFIG=\"cd_imconfig.h\" -I. -I%MH%\include -I%IM% -I%IM%\backends
%WINDRES% -O coff -o build\cdmodkit_res.o cdmodkit.rc || exit /b 1
%GCC% -O2 -w -c -I%MH%\include -I%MH%\src %MH%\src\buffer.c -o build\mh_buffer.o || exit /b 1
%GCC% -O2 -w -c -I%MH%\include -I%MH%\src %MH%\src\hook.c -o build\mh_hook.o || exit /b 1
%GCC% -O2 -w -c -I%MH%\include -I%MH%\src %MH%\src\trampoline.c -o build\mh_trampoline.o || exit /b 1
%GCC% -O2 -w -c -I%MH%\include -I%MH%\src %MH%\src\hde\hde64.c -o build\mh_hde64.o || exit /b 1
%GXX% %CXXFLAGS% -shared -static -static-libgcc -static-libstdc++ -s -o build\cdmodkit.asi ^
   cdmodkit.cpp environment.cpp http_api.cpp diag.cpp overlay.cpp input.cpp editor.cpp thumbgen.cpp heap.cpp icons.cpp i18n.cpp ^
   %IM%\imgui.cpp %IM%\imgui_draw.cpp %IM%\imgui_tables.cpp %IM%\imgui_widgets.cpp %IM%\backends\imgui_impl_dx12.cpp %IM%\backends\imgui_impl_win32.cpp ^
   build\mh_buffer.o build\mh_hook.o build\mh_trampoline.o build\mh_hde64.o build\cdmodkit_res.o ^
   -luser32 -lgdi32 -limm32 -lcomdlg32 -lshell32 -ld3d12 -ldxgi -ld3dcompiler -lversion -lws2_32 -lole32 -luuid -ladvapi32 -ldwmapi || exit /b 1
echo built build\cdmodkit.asi (GCC)
