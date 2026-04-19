@echo off
REM ============================================================
REM  PxrMaskProjection - Windows build (MSVC v143 / toolset 14.44)
REM  Houdini 21.0.559 + RenderMan 27.2 + VS 2026 Community
REM  VsDevCmd can't discover this install (no vswhere) -- env set manually.
REM
REM  Adjust the four paths at the top (RMANTREE, HFS, MSVC, WINSDK)
REM  for your install, then run this .bat from anywhere.
REM ============================================================

setlocal

set "RMANTREE=D:\Softwares\Pixar\RenderManProServer-27.2"
set "HFS=D:\Softwares\Side Effects Software\Houdini 21.0.559"

set "MSVC=D:\Softwares\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.44.35207"
set "WINSDK=C:\Program Files (x86)\Windows Kits\10"
set "WINSDK_VER=10.0.19041.0"

set "PATH=%MSVC%\bin\HostX64\x64;%WINSDK%\bin\%WINSDK_VER%\x64;%PATH%"

set "INCLUDE=%MSVC%\include;%WINSDK%\Include\%WINSDK_VER%\ucrt;%WINSDK%\Include\%WINSDK_VER%\shared;%WINSDK%\Include\%WINSDK_VER%\um;%WINSDK%\Include\%WINSDK_VER%\winrt"

set "LIB=%MSVC%\lib\x64;%WINSDK%\Lib\%WINSDK_VER%\ucrt\x64;%WINSDK%\Lib\%WINSDK_VER%\um\x64"

cd /d "%~dp0"

cl.exe ^
    /nologo /LD /EHsc /O2 /std:c++17 /MD ^
    /DNOMINMAX /D_USE_MATH_DEFINES /DWIN32 ^
    /I"%RMANTREE%\include" ^
    /I"%~dp0thirdparty" ^
    PxrMaskProjection.cpp thirdparty\miniz.c ^
    /Fe:PxrMaskProjection.dll ^
    /link ^
    /LIBPATH:"%RMANTREE%\lib" ^
    libprman.lib libpxrcore.lib

if errorlevel 1 (
    echo [ERROR] Build failed.
    exit /b 1
)

del /q PxrMaskProjection.obj miniz.obj PxrMaskProjection.exp PxrMaskProjection.lib 2>nul
echo.
echo [OK] Built PxrMaskProjection.dll (MSVC v14.44 / v143)
endlocal
