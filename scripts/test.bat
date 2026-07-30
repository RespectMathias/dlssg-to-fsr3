@echo off
setlocal enabledelayedexpansion

set "CONFIG=Debug"
set "FILTER="
set "SKIP_GPU=0"

:parse_args
if "%~1"=="" goto :done_args
if /I "%~1"=="Debug" ( set "CONFIG=Debug" & shift & goto :parse_args )
if /I "%~1"=="Release" ( set "CONFIG=Release" & shift & goto :parse_args )
if /I "%~1"=="--skip-gpu" ( set "SKIP_GPU=1" & shift & goto :parse_args )
set "FILTER=%~1"
shift
goto :parse_args
:done_args

set "ROOT=%~dp0.."
pushd "%ROOT%"

set "VSWHERE=!ProgramFiles(x86)!\Microsoft Visual Studio\Installer\vswhere.exe"

if not defined VSINSTALLDIR (
    if not exist "!VSWHERE!" (
        echo ERROR: Visual Studio Installer vswhere.exe not found. Install VS 18 Build Tools with Desktop development with C++.
        popd & pause
        exit /b 1
    )

    "!VSWHERE!" -latest -products * -version "[18.0,19.0)" -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%TEMP%\dlssg_vs_path.txt" 2>nul
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

set "API_DLL=%ROOT%\external\FidelityFX-SDK\PrebuiltSignedDLL\amd_fidelityfx_dx12.dll"
if not exist "%API_DLL%" (
    echo ERROR: Missing FidelityFX API DLL: %API_DLL%
    echo Confirm the DLL exists in the external tree.
    popd & pause
    exit /b 1
)

if "%CONFIG%"=="Debug" ( set "PRESET=test-debug" ) else ( set "PRESET=test-release" )

echo Configuring test preset...
cmake --preset test
if errorlevel 1 ( popd & pause & exit /b 1 )

echo Building !PRESET!...
cmake --build --preset !PRESET!
if errorlevel 1 ( popd & pause & exit /b 1 )

set "CTEST_ARGS=--preset !PRESET! --no-tests=error"

if "%SKIP_GPU%"=="1" (
    set "CTEST_ARGS=!CTEST_ARGS! -LE GPU -E Dx12Ngx^|VulkanNgx"
)

if defined FILTER (
    set "CTEST_ARGS=!CTEST_ARGS! -R "!FILTER!""
)

echo Running tests...
ctest !CTEST_ARGS!
if errorlevel 1 ( popd & pause & exit /b 1 )

popd
echo Verification passed.
pause
exit /b 0
