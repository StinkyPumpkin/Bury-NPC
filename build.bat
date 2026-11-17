@echo off
REM BuryTakeBodies 1.6.1+: CommonLibSSE-NG built from the pinned submodule extern\CommonLibSSE-NG (tag v9.1.0).
REM VS2022 Community (14.44) environment; the overlay triplet in cmake\ pins vcpkg deps to the same 14.44 toolset.
REM Configures build\ng\release on first run, then builds. POST_BUILD copies
REM BuryTakeBodies.dll into mods\BuryTakeBodies--Claude\SKSE\Plugins, so never run this while the game is running.
REM Edit the two paths below to match your Visual Studio and vcpkg installs.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" amd64
set VCPKG_ROOT=C:\vcpkg
cd /d %~dp0
if not exist extern\CommonLibSSE-NG\cmake\CommonLibSSE.cmake (
    echo === extern\CommonLibSSE-NG missing: git submodule update --init --recursive --depth 1 ===
    exit /b 1
)
if not exist build\ng\release\build.ninja (
    echo === CONFIGURING ===
    cmake --preset release
    if errorlevel 1 ( echo === CONFIGURE FAILED === & exit /b 1 )
)
echo === BUILDING ===
cmake --build build\ng\release
if errorlevel 1 ( echo === BUILD FAILED === & exit /b 1 )
echo === BUILD SUCCEEDED ===
if exist build\ng\release\BuryTakeBodies.dll (
    echo DLL: build\ng\release\BuryTakeBodies.dll
) else (
    echo === DLL NOT FOUND ===
    exit /b 1
)
