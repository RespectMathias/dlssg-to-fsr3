@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "SCRIPT_DIR=%~dp0"
set "SCRIPT_PATH=%~f0"
set "SCRIPT_NAME=%~nx0"
for %%I in ("%~dp0.") do set "SCRIPT_ROOT=%%~fI"
set "MODE="
set "TARGET=!SCRIPT_ROOT!"
set "FORCE=0"
set "SIGNATURE_OVERRIDE=0"
set "UNINSTALL=0"
set "PAUSE_ON_EXIT=0"

if "%~1"=="" set "PAUSE_ON_EXIT=1"

:parse_args
if "%~1"=="" goto args_done

if /i "%~1"=="-Mode" (
    if "%~2"=="" goto usage_error
    set "MODE=%~2"
    shift /1
    shift /1
    goto parse_args
)

if /i "%~1"=="-TargetPath" (
    if "%~2"=="" goto usage_error
    set "TARGET=%~2"
    shift /1
    shift /1
    goto parse_args
)

if /i "%~1"=="-Force" (
    set "FORCE=1"
    shift /1
    goto parse_args
)

if /i "%~1"=="-EnableSignatureOverride" (
    set "SIGNATURE_OVERRIDE=1"
    shift /1
    goto parse_args
)

if /i "%~1"=="-Uninstall" (
    set "UNINSTALL=1"
    shift /1
    goto parse_args
)

if /i "%~1"=="-Help" goto usage
if /i "%~1"=="--help" goto usage

echo Unknown argument: %~1>&2
goto usage_error

:args_done
for %%I in ("!TARGET!") do set "TARGET=%%~fI"
set "MANIFEST=!TARGET!\.dlssg_to_fsr_install"
set "MANIFEST_TMP=!MANIFEST!.tmp"

if /i "!SCRIPT_NAME!"=="windows_uninstall.bat" set "UNINSTALL=1"
if exist "!MANIFEST!" if not defined MODE set "UNINSTALL=1"
if "!UNINSTALL!"=="1" goto uninstall

if not defined MODE goto interactive_menu
goto mode_selected

:interactive_menu
echo [1] version.dll
echo [2] winhttp.dll
echo [3] dbghelp.dll
echo [4] ASI plugin
echo [5] RED4ext plugin
echo [6] nvngx.dll
echo [7] OptiScaler
echo [8] Uninstall
echo.

set "SELECTION="
set /p "SELECTION=Enter 1-8 (or press Enter for default): "

if not defined SELECTION set "SELECTION=1"
if "!SELECTION!"=="1" set "MODE=version"
if "!SELECTION!"=="2" set "MODE=winhttp"
if "!SELECTION!"=="3" set "MODE=dbghelp"
if "!SELECTION!"=="4" set "MODE=asi"
if "!SELECTION!"=="5" set "MODE=red4ext"
if "!SELECTION!"=="6" set "MODE=nvngx"
if "!SELECTION!"=="7" set "MODE=optiscaler"
if "!SELECTION!"=="8" goto uninstall

:mode_selected
set "INSTALL_NAME="

if /i "!MODE!"=="version" (
    set "MODE=version"
    set "INSTALL_NAME=version.dll"
)

if /i "!MODE!"=="winhttp" (
    set "MODE=winhttp"
    set "INSTALL_NAME=winhttp.dll"
)

if /i "!MODE!"=="dbghelp" (
    set "MODE=dbghelp"
    set "INSTALL_NAME=dbghelp.dll"
)

if /i "!MODE!"=="asi" (
    set "MODE=asi"
    set "INSTALL_NAME=dlssg_to_fsr.asi"
)

if /i "!MODE!"=="red4ext" (
    set "MODE=red4ext"
    set "INSTALL_NAME=dlssg_to_fsr.dll"
)

if /i "!MODE!"=="nvngx" (
    set "MODE=nvngx"
    set "INSTALL_NAME=nvngx.dll"
)

if /i "!MODE!"=="optiscaler" (
    set "MODE=optiscaler"
    set "INSTALL_NAME=dlssg_to_fsr3_amd_is_better.dll"
)

if not defined INSTALL_NAME (
    echo Invalid selection or mode: !MODE!>&2
    exit /b 1
)

if not exist "!TARGET!\" mkdir "!TARGET!" || exit /b 1

if exist "!MANIFEST!" (
    echo Existing installation found. Run windows_uninstall.bat first.>&2
    exit /b 1
)

if not exist "!SCRIPT_DIR!dlssg_to_fsr.dll" (
    echo Package file missing: !SCRIPT_DIR!dlssg_to_fsr.dll>&2
    exit /b 1
)

if not exist "!SCRIPT_DIR!amd_fidelityfx_dx12.dll" (
    echo Package file missing: !SCRIPT_DIR!amd_fidelityfx_dx12.dll>&2
    exit /b 1
)

>"!MANIFEST_TMP!" echo mode^|!MODE!^|^|

call :install_moved_file "!SCRIPT_DIR!dlssg_to_fsr.dll" "!TARGET!\!INSTALL_NAME!" || goto install_failed
call :install_file "!SCRIPT_DIR!amd_fidelityfx_dx12.dll" "!TARGET!\amd_fidelityfx_dx12.dll" || goto install_failed

if /i "!MODE!"=="nvngx" (
    call :configure_signature_override
    if errorlevel 1 goto install_failed
)

move /y "!MANIFEST_TMP!" "!MANIFEST!" >nul || goto install_failed

echo Installed !MODE! mode to !TARGET!

if /i not "!TARGET!"=="!SCRIPT_ROOT!" goto install_complete
if /i not "!SCRIPT_NAME!"=="windows_install.bat" goto install_complete
goto rename_to_uninstaller

:install_complete
call :pause_if_interactive
exit /b 0

:install_moved_file
set "SOURCE=%~f1"
set "DESTINATION=%~f2"
set "BACKUP=NONE"

if /i "!SOURCE!"=="!DESTINATION!" goto record_moved_file
if not exist "!DESTINATION!" goto move_file

if "!FORCE!"=="0" (
    set "ANSWER="
    set /p "ANSWER=!DESTINATION! exists. Back up and replace? [y/N] "
    if /i not "!ANSWER!"=="y" if /i not "!ANSWER!"=="yes" exit /b 1
)

set "BACKUP=!DESTINATION!.dlssg_to_fsr.bak"

if exist "!BACKUP!" (
    echo Backup already exists: !BACKUP!>&2
    exit /b 1
)

move /y "!DESTINATION!" "!BACKUP!" >nul || exit /b 1

:move_file
move /y "!SOURCE!" "!DESTINATION!" >nul || exit /b 1

:record_moved_file
call :get_hash "!DESTINATION!"

if not defined FILE_HASH (
    echo Failed to hash !DESTINATION!.>&2
    exit /b 1
)

>>"!MANIFEST_TMP!" echo move^|!DESTINATION!^|!FILE_HASH!^|!BACKUP!^|!SOURCE!
exit /b 0

:install_file
set "SOURCE=%~f1"
set "DESTINATION=%~f2"
set "BACKUP="

if not exist "!SOURCE!" (
    echo Package file missing: !SOURCE!>&2
    exit /b 1
)

if /i "!SOURCE!"=="!DESTINATION!" exit /b 0
if not exist "!DESTINATION!" goto copy_file

if "!FORCE!"=="0" (
    set "ANSWER="
    set /p "ANSWER=!DESTINATION! exists. Back up and replace? [y/N] "
    if /i not "!ANSWER!"=="y" if /i not "!ANSWER!"=="yes" exit /b 1
)

set "BACKUP=!DESTINATION!.dlssg_to_fsr.bak"

if exist "!BACKUP!" (
    echo Backup already exists: !BACKUP!>&2
    exit /b 1
)

move /y "!DESTINATION!" "!BACKUP!" >nul || exit /b 1

:copy_file
copy /y "!SOURCE!" "!DESTINATION!" >nul || exit /b 1

:record_file
call :get_hash "!DESTINATION!"

if not defined FILE_HASH (
    echo Failed to hash !DESTINATION!.>&2
    exit /b 1
)

>>"!MANIFEST_TMP!" echo file^|!DESTINATION!^|!FILE_HASH!^|!BACKUP!
exit /b 0

:get_hash
set "FILE_HASH="

for /f "skip=1 tokens=* delims=" %%H in ('certutil -hashfile "%~1" SHA256 2^>nul') do (
    if not defined FILE_HASH set "FILE_HASH=%%H"
)

set "FILE_HASH=!FILE_HASH: =!"
exit /b 0

:configure_signature_override
if "!SIGNATURE_OVERRIDE!"=="0" (
    set "ANSWER="
    set /p "ANSWER=Enable Nvidia signature override in HKLM? [y/N] "
    if /i "!ANSWER!"=="y" set "SIGNATURE_OVERRIDE=1"
    if /i "!ANSWER!"=="yes" set "SIGNATURE_OVERRIDE=1"
)

if "!SIGNATURE_OVERRIDE!"=="0" exit /b 0

set "VALUE_NAME={41FCC608-8496-4DEF-B43E-7D9BD675A6FF}"

call :configure_registry_key "HKLM\SOFTWARE\NVIDIA Corporation\Global" "!VALUE_NAME!" || exit /b 1
call :configure_registry_key "HKLM\SYSTEM\ControlSet001\Services\nvlddmkm" "!VALUE_NAME!" || exit /b 1
exit /b 0

:configure_registry_key
set "REG_KEY=%~1"
set "REG_NAME=%~2"
set "OLD_VALUE="

if exist "!MANIFEST!" (
    for /f "usebackq tokens=1-4 delims=|" %%A in ("!MANIFEST!") do (
        if /i "%%A"=="reg" if /i "%%B"=="!REG_KEY!" if /i "%%C"=="!REG_NAME!" set "OLD_VALUE=%%D"
    )
)

if not defined OLD_VALUE (
    for /f "skip=2 tokens=3" %%V in ('reg query "!REG_KEY!" /v "!REG_NAME!" 2^>nul') do (
        set "OLD_VALUE=%%V"
    )
)

if not defined OLD_VALUE set "OLD_VALUE=MISSING"

>>"!MANIFEST_TMP!" echo reg^|!REG_KEY!^|!REG_NAME!^|!OLD_VALUE!

reg add "!REG_KEY!" /v "!REG_NAME!" /t REG_DWORD /d 1 /f >nul
exit /b !errorlevel!

:uninstall
if not exist "!MANIFEST!" (
    echo Install manifest not found: !MANIFEST!>&2
    call :pause_if_interactive
    exit /b 1
)

set "MOVE_DESTINATION="
set "FILE_DESTINATION="

for /f "usebackq tokens=1-5 delims=|" %%A in ("!MANIFEST!") do (
    if /i "%%A"=="move" (
        set "MOVE_DESTINATION=%%B"
        set "MOVE_HASH=%%C"
        set "MOVE_BACKUP=%%D"
        set "MOVE_SOURCE=%%E"
    )

    if /i "%%A"=="file" (
        set "FILE_DESTINATION=%%B"
        set "FILE_EXPECTED_HASH=%%C"
        set "FILE_BACKUP=%%D"
    )

    if /i "%%A"=="reg" (
        if /i "%%D"=="MISSING" reg delete "%%B" /v "%%C" /f >nul 2>nul
        if /i not "%%D"=="MISSING" reg add "%%B" /v "%%C" /t REG_DWORD /d "%%D" /f >nul 2>nul
    )
)

if defined MOVE_DESTINATION (
    call :restoremove "!MOVE_DESTINATION!" "!MOVE_HASH!" "!MOVE_BACKUP!" "!MOVE_SOURCE!"
)

if defined FILE_DESTINATION (
    call :remove_file "!FILE_DESTINATION!" "!FILE_EXPECTED_HASH!" "!FILE_BACKUP!"
)

del /q "!MANIFEST!" >nul

echo dlssg_to_fsr removed from !TARGET!

if /i not "!TARGET!"=="!SCRIPT_ROOT!" goto uninstall_complete
if /i not "!SCRIPT_NAME!"=="windows_uninstall.bat" goto uninstall_complete
goto rename_to_installer

:uninstall_complete
call :pause_if_interactive
exit /b 0

:restoremove
set "DESTINATION=%~1"
set "EXPECTED_HASH=%~2"
set "BACKUP=%~3"
set "SOURCE=%~4"

if /i "!BACKUP!"=="NONE" set "BACKUP="
if /i "!SOURCE!"=="!DESTINATION!" exit /b 0
if not exist "!DESTINATION!" goto restore_moved_backup

call :get_hash "!DESTINATION!"

if /i not "!FILE_HASH!"=="!EXPECTED_HASH!" (
    echo Warning: modified installed file kept: !DESTINATION!>&2
    exit /b 0
)

if exist "!SOURCE!" (
    echo Warning: original package DLL already exists: !SOURCE!>&2
    exit /b 0
)

move /y "!DESTINATION!" "!SOURCE!" >nul || exit /b 1

:restore_moved_backup
if defined BACKUP if exist "!BACKUP!" (
    move /y "!BACKUP!" "!DESTINATION!" >nul
)

exit /b 0

:remove_file
set "DESTINATION=%~1"
set "EXPECTED_HASH=%~2"
set "BACKUP=%~3"

if not exist "!DESTINATION!" goto restore_backup

call :get_hash "!DESTINATION!"

if /i not "!FILE_HASH!"=="!EXPECTED_HASH!" (
    echo Warning: modified installed file kept: !DESTINATION!>&2
    exit /b 0
)

del /q "!DESTINATION!" >nul || exit /b 1

:restore_backup
if defined BACKUP if exist "!BACKUP!" (
    move /y "!BACKUP!" "!DESTINATION!" >nul
)

exit /b 0

:restore_registry
if /i "%~3"=="MISSING" (
    reg delete "%~1" /v "%~2" /f >nul 2>nul
) else (
    reg add "%~1" /v "%~2" /t REG_DWORD /d "%~3" /f >nul 2>nul
)

exit /b 0

:install_failed
set "MOVE_DESTINATION="
set "FILE_DESTINATION="

if exist "!MANIFEST_TMP!" (
    for /f "usebackq tokens=1-5 delims=|" %%A in ("!MANIFEST_TMP!") do (
        if /i "%%A"=="move" (
            set "MOVE_DESTINATION=%%B"
            set "MOVE_HASH=%%C"
            set "MOVE_BACKUP=%%D"
            set "MOVE_SOURCE=%%E"
        )

        if /i "%%A"=="file" (
            set "FILE_DESTINATION=%%B"
            set "FILE_EXPECTED_HASH=%%C"
            set "FILE_BACKUP=%%D"
        )
    )
)

if defined MOVE_DESTINATION (
    call :restoremove "!MOVE_DESTINATION!" "!MOVE_HASH!" "!MOVE_BACKUP!" "!MOVE_SOURCE!"
)

if defined FILE_DESTINATION (
    call :remove_file "!FILE_DESTINATION!" "!FILE_EXPECTED_HASH!" "!FILE_BACKUP!"
)

del /q "!MANIFEST_TMP!" >nul 2>nul

echo Installation failed.>&2
call :pause_if_interactive
exit /b 1

:usage
echo Usage: windows_install.bat [-Mode version^|winhttp^|dbghelp^|asi^|red4ext^|nvngx^|optiscaler] [-TargetPath PATH] [-Force] [-EnableSignatureOverride] [-Uninstall]
exit /b 0

:usage_error
call :usage >&2
call :pause_if_interactive
exit /b 1

:pause_if_interactive
if "!PAUSE_ON_EXIT!"=="1" (
    echo.
    echo Press any key to continue...
    >nul pause
)
exit /b 0

:rename_to_uninstaller
call :pause_if_interactive
>"!SCRIPT_ROOT!\_dlssg_ren.bat" echo @echo off
>>"!SCRIPT_ROOT!\_dlssg_ren.bat" echo ping 127.0.0.1 -n 2 ^>nul
>>"!SCRIPT_ROOT!\_dlssg_ren.bat" echo move /y "%%~dp0windows_install.bat" "%%~dp0windows_uninstall.bat" ^>nul
>>"!SCRIPT_ROOT!\_dlssg_ren.bat" echo del "%%~f0" ^>nul
start "" /b cmd.exe /d /c call "!SCRIPT_ROOT!\_dlssg_ren.bat" >nul 2>&1 <nul
exit /b 0

:rename_to_installer
call :pause_if_interactive
>"!SCRIPT_ROOT!\_dlssg_ren.bat" echo @echo off
>>"!SCRIPT_ROOT!\_dlssg_ren.bat" echo ping 127.0.0.1 -n 2 ^>nul
>>"!SCRIPT_ROOT!\_dlssg_ren.bat" echo move /y "%%~dp0windows_uninstall.bat" "%%~dp0windows_install.bat" ^>nul
>>"!SCRIPT_ROOT!\_dlssg_ren.bat" echo del "%%~f0" ^>nul
start "" /b cmd.exe /d /c call "!SCRIPT_ROOT!\_dlssg_ren.bat" >nul 2>&1 <nul
exit /b 0