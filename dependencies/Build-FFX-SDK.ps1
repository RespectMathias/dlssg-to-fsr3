$ErrorActionPreference = "Stop"

# Ensure TLS 1.2 for NuGet endpoints (required on older PowerShell/Windows)
try { [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 } catch {}

# Ensure WinPixEventRuntime binaries exist in the FidelityFX SDK PIX lib before building
Write-Host "Checking WinPixEventRuntime (PIX) binaries..."

# Locations
$pixDir = Join-Path $PSScriptRoot "FidelityFX-SDK\sdk\libs\pix"
New-Item -ItemType Directory -Force -Path $pixDir | Out-Null

$pixDll = Join-Path $pixDir 'WinPixEventRuntime.dll'
$pixLib = Join-Path $pixDir 'WinPixEventRuntime.lib'

if (-not (Test-Path $pixDll) -and -not (Test-Path $pixLib)) {
    Write-Host "WinPixEventRuntime not found in $pixDir. Downloading from NuGet..."

    # Download latest WinPixEventRuntime nupkg from NuGet V3 flat container API
    function Get-LatestWinPixEventRuntimeVersion {
        $indexUrl = 'https://api.nuget.org/v3-flatcontainer/winpixeventruntime/index.json'
        try {
            $resp = Invoke-RestMethod -Uri $indexUrl -ErrorAction Stop
            $versions = @($resp.versions)
            if ($versions.Count -eq 0) { throw "No versions returned from NuGet for WinPixEventRuntime." }
            return $versions[-1]
        }
        catch {
            throw "Failed to query latest WinPixEventRuntime version from NuGet: $($_.Exception.Message)"
        }
    }

    try {
        $version = Get-LatestWinPixEventRuntimeVersion
        Write-Host "Latest WinPixEventRuntime version: $version"

        $tempBase = Join-Path $env:TEMP ("pix-nuget-" + (Get-Date -Format 'yyyyMMddHHmmss'))
        New-Item -ItemType Directory -Force -Path $tempBase | Out-Null

        $nupkgUrl = "https://api.nuget.org/v3-flatcontainer/winpixeventruntime/$version/winpixeventruntime.$version.nupkg"
        $nupkgPath = Join-Path $tempBase "WinPixEventRuntime.$version.nupkg"
        Write-Host "Downloading $nupkgUrl ..."
        Invoke-WebRequest -Uri $nupkgUrl -OutFile $nupkgPath -ErrorAction Stop

        # Rename .nupkg to .zip and extract
        $zipPath = [System.IO.Path]::ChangeExtension($nupkgPath, '.zip')
        Move-Item -Force -Path $nupkgPath -Destination $zipPath

        $extractDir = Join-Path $tempBase 'extracted'
        Expand-Archive -Path $zipPath -DestinationPath $extractDir -Force

        # The NuGet package contains the binaries under bin/x64; copy them into the pix root
        $pkgBinX64 = Join-Path $extractDir 'bin\x64'
        if (-not (Test-Path (Join-Path $pkgBinX64 'WinPixEventRuntime.dll')) -or -not (Test-Path (Join-Path $pkgBinX64 'WinPixEventRuntime.lib'))) {
            throw "Could not locate bin/x64/WinPixEventRuntime.{dll,lib} in the downloaded package."
        }

        Copy-Item -Path (Join-Path $pkgBinX64 'WinPixEventRuntime.dll') -Destination $pixDir -Force
        Copy-Item -Path (Join-Path $pkgBinX64 'WinPixEventRuntime.lib') -Destination $pixDir -Force
        Write-Host "WinPixEventRuntime binaries copied to: $pixDir"
    }
    finally {
        if ($tempBase -and (Test-Path $tempBase)) {
            try { Remove-Item -LiteralPath $tempBase -Recurse -Force -ErrorAction SilentlyContinue } catch {}
        }
    }
} else {
    Write-Host "WinPixEventRuntime already present. Skipping download."
}

# Set up powershell equivalent of vcvarsall.bat when CMake/CPack aren't in PATH
if ((Get-Command "cmake" -ErrorAction SilentlyContinue) -eq $null) {
    $vsPath = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationpath

    Import-Module (Get-ChildItem $vsPath -Recurse -File -Filter Microsoft.VisualStudio.DevShell.dll).FullName
    Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64'
}

Push-Location "$PSScriptRoot\FidelityFX-SDK\sdk\"

Write-Host "Building FFX_API_BACKEND DX12_X64..."
& .\BuildFidelityFXSDK.bat -DFFX_API_BACKEND=DX12_X64 -DFFX_FI=ON -DFFX_OF=ON -DFFX_AUTO_COMPILE_SHADERS=1 -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded

Write-Host "Building FFX_API_BACKEND VK_X64..."
& .\BuildFidelityFXSDK.bat -DFFX_API_BACKEND=VK_X64 -DFFX_FI=ON -DFFX_OF=ON -DFFX_AUTO_COMPILE_SHADERS=1 -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded

Pop-Location
