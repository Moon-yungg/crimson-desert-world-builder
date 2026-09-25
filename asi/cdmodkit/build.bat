@echo off
rem an MSVC environment that is already set up (CI, a developer command prompt) is used as is; otherwise the local Build Tools
where cl >nul 2>nul || call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
if not exist build mkdir build
python ..\..\scripts\check_locales.py || exit /b 1
python ..\..\scripts\pack_index.py || exit /b 1
rc /nologo /fo build\cdmodkit.res cdmodkit.rc || exit /b 1
set MH=..\..\tools\minhook
set IM=..\..\tools\imgui
python ..\..\scripts\patch_minhook.py "%MH%\src\trampoline.c" || exit /b 1
cl /nologo /std:c++17 /utf-8 /O2 /W3 /EHa /MT /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /DIMGUI_DISABLE_OBSOLETE_FUNCTIONS=0 /DIMGUI_USER_CONFIG=\"cd_imconfig.h\" ^
   /I. /I%MH%\include /I%IM% /I%IM%\backends /Fo:build\ /LD ^
   cdmodkit.cpp http_api.cpp diag.cpp overlay.cpp input.cpp editor.cpp thumbgen.cpp heap.cpp icons.cpp i18n.cpp ^
   %IM%\imgui.cpp %IM%\imgui_draw.cpp %IM%\imgui_tables.cpp %IM%\imgui_widgets.cpp %IM%\backends\imgui_impl_dx12.cpp %IM%\backends\imgui_impl_win32.cpp ^
   %MH%\src\buffer.c %MH%\src\hook.c %MH%\src\trampoline.c %MH%\src\hde\hde64.c ^
   build\cdmodkit.res user32.lib comdlg32.lib shell32.lib d3d12.lib dxgi.lib d3dcompiler.lib ws2_32.lib /link /OUT:build\cdmodkit.asi
