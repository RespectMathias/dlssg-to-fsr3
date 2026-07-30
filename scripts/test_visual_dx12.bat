@echo off
setlocal enabledelayedexpansion

set "ROOT=%~dp0.."
set "TESTBED=%ROOT%\bin\test\tests\Debug\FrameGenerationTestbed.exe"
set "DLSSG_TO_FSR3_DLL=%ROOT%\bin\test\src\Debug\dlssg_to_fsr3.dll"

if not exist "%TESTBED%" (
    echo ERROR: Testbed not found: %TESTBED%
    echo Build test configuration first: scripts\test.bat
    pause
    exit /b 1
)

if not exist "%DLSSG_TO_FSR3_DLL%" (
    echo ERROR: DLL not found: %DLSSG_TO_FSR3_DLL%
    echo Build test configuration first: scripts\test.bat
    pause
    exit /b 1
)

echo Launching DX12 visual test (FG ON, 30 real FPS)...
"%TESTBED%" --backend=dx12 --fg=on --visual --render-fps=30
pause
