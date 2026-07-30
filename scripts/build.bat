@echo off
setlocal enabledelayedexpansion

set "ROOT=%~dp0.."
pushd "%ROOT%"

set "VSWHERE=!ProgramFiles(x86)!\Microsoft Visual Studio\Installer\vswhere.exe"
set "API_DLL=%ROOT%\external\FidelityFX-SDK\PrebuiltSignedDLL\amd_fidelityfx_dx12.dll"
if not exist "%API_DLL%" (
    echo ERROR: Missing FidelityFX API DLL: %API_DLL%
    echo Confirm the DLL exists in the external tree.
    popd & pause
    exit /b 1
)

if not defined VSINSTALLDIR (
    if not exist "!VSWHERE!" (
        echo ERROR: Visual Studio Installer vswhere.exe not found. Install VS 18 Build Tools with Desktop development with C++.
        popd & pause
        exit /b 1
    )

    "!VSWHERE!" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%TEMP%\dlssg_vs_path.txt" 2>nul
    set /p VS_PATH=<"%TEMP%\dlssg_vs_path.txt"
    del "%TEMP%\dlssg_vs_path.txt" 2>nul

    if not defined VS_PATH (
        echo ERROR: Visual Studio 18 with x64 C++ tools not found.
        popd & pause
        exit /b 1
    )

    set "VCVARSALL=!VS_PATH!\VC\Auxiliary\Build\vcvarsall.bat"
    if not exist "!VCVARSALL!" (
        echo ERROR: vcvarsall.bat not found at !VCVARSALL!.
        popd & pause
        exit /b 1
    )

    call "!VCVARSALL!" x64 >nul 2>&1
    if errorlevel 1 (
        echo ERROR: vcvarsall.bat failed.
        popd & pause
        exit /b 1
    )
)

if not defined VCPKG_ROOT (
    for %%p in ("!VSINSTALLDIR!") do set "VC_PARENT=%%~dpp"
    set "BUNDLED_VCPKG=!VC_PARENT!VC\vcpkg"
    if exist "!BUNDLED_VCPKG!\scripts\buildsystems\vcpkg.cmake" (
        set "VCPKG_ROOT=!BUNDLED_VCPKG!"
    ) else (
        echo ERROR: Bundled vcpkg not found at !BUNDLED_VCPKG!.
        popd & pause
        exit /b 1
    )
)

echo Configuring final preset...
cmake --preset final
if errorlevel 1 ( popd & pause & exit /b 1 )

echo Building final-release...
cmake --build --preset final-release
if errorlevel 1 ( popd & pause & exit /b 1 )

echo Packaging...
cpack --preset final
if errorlevel 1 ( popd & pause & exit /b 1 )

popd
echo Build complete.
pause
exit /b 0
