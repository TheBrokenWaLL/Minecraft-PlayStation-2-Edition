#requires -Version 7.3
# Run with PowerShell 7 (pwsh). Profiles follow .github/workflows/build.yml.
param(
    [ValidateSet('gcc', 'wii', 'ps2')][string]$Platform,
    [switch]$Debug,
    [switch]$Release,
    [ValidateRange(1, 256)][int]$Jobs = 4,
    [switch]$ConfigureOnly,
    [switch]$NoAssets,
    [switch]$DryRun,
    [switch]$Help,
    [string[]]$CMakeArgs = @()
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
# Preserve embedded quotes in include flags and paths with spaces.
$PSNativeCommandArgumentPassing = 'Standard'
$Root = $PSScriptRoot.Replace('\', '/')
if ($Help) {
    Write-Host @'
pwsh -File .\build_windows.ps1 [gcc|wii|ps2] [-Debug|-Release] [-Jobs 4]
    [-ConfigureOnly] [-NoAssets] [-DryRun] [-CMakeArgs '-DNAME=value']
No platform: show the selection menu. Default configuration: Release.
Requires native Windows CMake/Ninja and the selected SDK on PATH.
GCC means Windows MinGW GCC. No automatic installs, downloads, or cleanup.
'@
    exit 0
}
if ($Debug -and $Release) { throw 'Choose either -Debug or -Release.' }
if (-not $Platform) {
    Write-Host "1) Windows GCC (MinGW)`n2) Nintendo Wii (full game)`n3) PlayStation 2"
    switch (Read-Host 'Build target') {
        '1' { $Platform = 'gcc' }
        '2' { $Platform = 'wii' }
        '3' { $Platform = 'ps2' }
        default { throw 'Invalid target.' }
    }
}
$Platform = $Platform.ToLowerInvariant()
$Configuration = if ($Debug) { 'Debug' } else { 'Release' }
$BuildDir = "$Root/build/$Platform-$($Configuration.ToLowerInvariant())"
function Require-Tool([string]$Name) {
    if (-not $DryRun -and -not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "Missing $Name. Install the required build dependencies."
    }
}
function Require-File([string]$Path) {
    if (-not $DryRun -and -not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing $Path. Install the required build dependencies."
    }
}
function Require-Directory([string]$Path) {
    if (-not $DryRun -and -not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "Missing $Path. Restore assets or use -NoAssets to compile only."
    }
}
function Invoke-BuildCommand([string]$Command, [string[]]$Arguments) {
    Write-Host ('+ ' + $Command + ' ' + (($Arguments | ForEach-Object { '"' + $_ + '"' }) -join ' '))
    if (-not $DryRun) {
        & $Command @Arguments
        if ($LASTEXITCODE -ne 0) { throw "$Command failed with exit code $LASTEXITCODE" }
    }
}
Require-Tool cmake
Require-Tool ninja
Require-File "$Root/external/zlib/CMakeLists.txt"
Require-File "$Root/external/stb/stb_image.h"
$ConfigureArgs = @('-S', $Root, '-B', $BuildDir, '-G', 'Ninja',
    '-DCMAKE_MAKE_PROGRAM=ninja', "-DCMAKE_BUILD_TYPE=$Configuration")
if (Get-Command ccache -ErrorAction SilentlyContinue) {
    $ConfigureArgs += @('-DCMAKE_C_COMPILER_LAUNCHER=ccache', '-DCMAKE_CXX_COMPILER_LAUNCHER=ccache')
}
switch ($Platform) {
    'gcc' {
        Require-Tool gcc
        Require-Tool g++
        if (-not $DryRun) {
            $Triple = & gcc -dumpmachine
            if ($LASTEXITCODE -ne 0 -or $Triple -notmatch 'mingw') {
                throw 'Windows GCC must be MinGW (for example MSYS2 UCRT64), not MSYS/Cygwin GCC.'
            }
        }
        foreach ($File in @('SDL2/CMakeLists.txt', 'SDL_net/CMakeLists.txt',
            'gzip-hpp/include/gzip/compress.hpp', 'miniaudio/miniaudio.h')) {
            Require-File "$Root/external/$File"
        }
        $ConfigureArgs += @('-DCMAKE_C_COMPILER=gcc', '-DCMAKE_CXX_COMPILER=g++',
            '-DMC_LOG_LEVEL=2', "-DCMAKE_CXX_FLAGS=-I`"$Root/src/net/minecraft/src`"")
    }
    'wii' {
        if (-not $env:DEVKITPRO) { $env:DEVKITPRO = 'C:/devkitPro' }
        if (-not $env:DEVKITPPC) { $env:DEVKITPPC = "$env:DEVKITPRO/devkitPPC" }
        $env:PATH = "$env:DEVKITPPC/bin;$env:DEVKITPRO/tools/bin;$env:PATH"
        Require-Tool powerpc-eabi-g++
        Require-Tool elf2dol
        Require-File "$env:DEVKITPRO/libogc/include/gccore.h"
        $ConfigureArgs += @("-DCMAKE_TOOLCHAIN_FILE=$Root/cmake/wii_toolchain.cmake",
            '-DPLATFORM=WII', "-DDEVKITPRO=$env:DEVKITPRO", "-DDEVKITPPC=$env:DEVKITPPC",
            '-DWII_BRINGUP=OFF', '-DWII_ENABLE_SOUND=ON', '-DWII_ENABLE_NETWORK=ON',
            "-DCMAKE_CXX_FLAGS=-DHW_RVL -mrvl -mcpu=750 -meabi -mhard-float -I`"$Root/src/net/minecraft/src`"")
    }
    'ps2' {
        if (-not $env:PS2DEV) { $env:PS2DEV = "$Root/psdevwindows" }
        if (-not $env:PS2SDK) { $env:PS2SDK = "$env:PS2DEV/ps2sdk" }
        if (-not $env:GSKIT) { $env:GSKIT = "$env:PS2DEV/gsKit" }
        $env:PATH = "$env:PS2DEV/ee/bin;$env:PS2DEV/iop/bin;$env:PS2DEV/dvp/bin;$env:PS2DEV/bin;$env:PS2SDK/bin;$env:PATH"
        Require-Tool mips64r5900el-ps2-elf-g++
        Require-File "$env:PS2SDK/ee/include/kernel.h"
        $ConfigureArgs += @("-DCMAKE_TOOLCHAIN_FILE=$Root/cmake/ps2_toolchain.cmake",
            '-DPLATFORM=PS2', '-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY',
            '-DPS2_ENABLE_NETWORK=ON', '-DPS2_ENABLE_SOUND=ON', '-DPS2_ENABLE_PERSPECTIVE_TEXTURES=ON',
            '-DPS2_RENDER_STATS=OFF', '-DPS2_ENABLE_VU1_TERRAIN=ON', '-DMC_LOG_LEVEL=0')
    }
}
if (-not $NoAssets -and -not $ConfigureOnly) {
    Require-Directory "$Root/data/assets"
    if ($Platform -eq 'ps2') {
        Require-Directory "$Root/data/startup"
        Require-Directory "$Root/data/resources_ps2"
        Require-Tool python
        Require-File "$Root/scripts/make_pak.py"
    } elseif ($Platform -eq 'wii') { Require-Directory "$Root/data/resources" }
}
$TranscriptStarted = $false
try {
    if (-not $DryRun) {
        New-Item -ItemType Directory -Force -Path "$Root/build/logs" | Out-Null
        Start-Transcript -Path "$Root/build/logs/windows-$Platform-$($Configuration.ToLowerInvariant()).log" -Force | Out-Null
        $TranscriptStarted = $true
    }
    Invoke-BuildCommand 'cmake' ($ConfigureArgs + $CMakeArgs)
    if (-not $ConfigureOnly) {
        Invoke-BuildCommand 'cmake' @('--build', $BuildDir, '--target', 'OptiCraft', '--parallel', "$Jobs")
        if (-not $NoAssets -and $Platform -eq 'gcc') {
            Invoke-BuildCommand 'cmake' @('-E', 'copy_directory', "$Root/data/assets", "$Root/bin/$Configuration/assets")
            if (Test-Path -LiteralPath "$Root/data/resources" -PathType Container) {
                Invoke-BuildCommand 'cmake' @('-E', 'copy_directory', "$Root/data/resources", "$Root/bin/$Configuration/resources")
            } else {
                Write-Warning 'data/resources is missing; desktop audio is not staged (resources_ps2 is not a substitute).'
            }
            if (Test-Path -LiteralPath "$Root/bin/$Configuration/assets.pak") {
                Write-Warning 'Existing assets.pak takes priority over loose files; update it if it contains older assets.'
            }
        }
        if (-not $NoAssets -and $Platform -ne 'gcc') {
            Invoke-BuildCommand 'cmake' @('--build', $BuildDir, '--target', "$Platform-data", '--parallel', "$Jobs")
            if ($Platform -eq 'ps2') {
                Invoke-BuildCommand 'python' @("$Root/scripts/make_pak.py", "$Root/bin/ps2/usb/MCBETA/data", "$Root/bin/ps2/usb/MCBETA/assets.pak")
            }
        }
        switch ($Platform) {
            'gcc' { Write-Host "Executable: $Root/bin/$Configuration/OptiCraft.exe (keep MinGW DLLs available; run from project root)" }
            'wii' { Write-Host "SD files: $Root/bin/wii/sd/" }
            'ps2' { Write-Host "USB application: $Root/bin/ps2/usb/MCBETA/" }
        }
    }
} catch {
    Write-Error $_ -ErrorAction Continue
    exit 1
} finally {
    if ($TranscriptStarted) { Stop-Transcript | Out-Null }
}
