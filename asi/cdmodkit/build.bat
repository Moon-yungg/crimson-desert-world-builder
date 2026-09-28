@echo off
rem An MSVC environment that is already set up (CI, a developer command prompt) is used as is.
rem For this workspace we also support the portable toolchain in WB\toolchains\msvc before falling back to installed Build Tools.
where cl >nul 2>nul || call :setup_msvc
where cl >nul 2>nul || (echo MSVC cl.exe not found & exit /b 1)
where rc >nul 2>nul || (echo Windows SDK rc.exe not found & exit /b 1)
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
   cdmodkit.cpp environment.cpp terrain.cpp terrain_research.cpp travel.cpp terrain_live.cpp terrain_physics.cpp gpu_research.cpp http_api.cpp diag.cpp overlay.cpp overlay_discovery.cpp input.cpp editor.cpp thumbgen.cpp heap.cpp icons.cpp i18n.cpp proj_codec.cpp wb_group_math.cpp report_projection.cpp ^
   %IM%\imgui.cpp %IM%\imgui_draw.cpp %IM%\imgui_tables.cpp %IM%\imgui_widgets.cpp %IM%\backends\imgui_impl_dx12.cpp %IM%\backends\imgui_impl_win32.cpp ^
   %MH%\src\buffer.c %MH%\src\hook.c %MH%\src\trampoline.c %MH%\src\hde\hde64.c ^
   build\cdmodkit.res user32.lib imm32.lib comdlg32.lib shell32.lib d3d12.lib dxgi.lib d3dcompiler.lib ws2_32.lib /link /OUT:build\cdmodkit.asi
exit /b %errorlevel%

:setup_msvc
set "PORTABLE_MSVC=%~dp0..\..\..\toolchains\msvc"
if exist "%PORTABLE_MSVC%\VC\Tools\MSVC" (
    for /d %%D in ("%PORTABLE_MSVC%\VC\Tools\MSVC\*") do set "VCTOOLS=%%~fD"
    for /d %%D in ("%PORTABLE_MSVC%\Windows Kits\10\Include\*") do set "SDKVER=%%~nxD"
)
if defined VCTOOLS if defined SDKVER (
    set "SDKROOT=%PORTABLE_MSVC%\Windows Kits\10"
    set "PATH=%VCTOOLS%\bin\Hostx64\x64;%PORTABLE_MSVC%\Windows Kits\10\bin\%SDKVER%\x64;%PATH%"
    set "INCLUDE=%VCTOOLS%\include;%PORTABLE_MSVC%\Windows Kits\10\Include\%SDKVER%\ucrt;%PORTABLE_MSVC%\Windows Kits\10\Include\%SDKVER%\shared;%PORTABLE_MSVC%\Windows Kits\10\Include\%SDKVER%\um;%PORTABLE_MSVC%\Windows Kits\10\Include\%SDKVER%\winrt;%PORTABLE_MSVC%\Windows Kits\10\Include\%SDKVER%\cppwinrt"
    set "LIB=%VCTOOLS%\lib\x64;%PORTABLE_MSVC%\Windows Kits\10\Lib\%SDKVER%\ucrt\x64;%PORTABLE_MSVC%\Windows Kits\10\Lib\%SDKVER%\um\x64"
    exit /b 0
)
if exist "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
exit /b 0
