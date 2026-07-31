@echo off
setlocal enabledelayedexpansion

set "ROOT=%~dp0.."
set "TESTBED=%ROOT%\bin\test\tests\Debug\FrameGenerationTestbed.exe"
set "DLSSG_TO_FSR_DLL=%ROOT%\bin\test\src\Debug\dlssg_to_fsr.dll"
set "VK_LOADER_LAYERS_DISABLE=~implicit~"

if not exist "%TESTBED%" (
    echo ERROR: Testbed not found: %TESTBED%
    echo Build test configuration first: scripts\test.bat
    pause
    exit /b 1
)

if not exist "%DLSSG_TO_FSR_DLL%" (
    echo ERROR: DLL not found: %DLSSG_TO_FSR_DLL%
    echo Build test configuration first: scripts\test.bat
    pause
    exit /b 1
)

echo Launching Vulkan visual test (FG ON, 30 real FPS)...
"%TESTBED%" --backend=vulkan --fg=on --visual --render-fps=30 --submit=legacy --copy=shader
pause
