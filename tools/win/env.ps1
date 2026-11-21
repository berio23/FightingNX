# Loads the Windows build environment of the port into the current PowerShell session:
#   MSVC x64 libraries and Windows SDK (VS 2022 Build Tools, for clang's msvc target),
#   LLVM clang 20 (ReXGlue needs clang), and one single CMake + Ninja.
#
#   . tools\win\env.ps1
#
# Override the locations with $env:SA2_LLVM, $env:SA2_VSDIR, $env:SA2_CMAKE_DIR if yours differ.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
# This machine's paths (SA2_LLVM, SA2_WINSYSROOT, ...): tools\win\env.local.ps1, git-ignored, optional.
$localEnv = Join-Path $PSScriptRoot 'env.local.ps1'
if (Test-Path $localEnv) { . $localEnv }
$llvm = if ($env:SA2_LLVM) { $env:SA2_LLVM } else { Join-Path $root '..\_tools\llvm' }
$vs = if ($env:SA2_VSDIR) { $env:SA2_VSDIR } else { 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools' }
$cm = if ($env:SA2_CMAKE_DIR) { $env:SA2_CMAKE_DIR } else { Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake' }

$llvm = (Resolve-Path $llvm).Path
if (-not (Test-Path "$llvm\bin\clang++.exe")) { throw "clang not found in $llvm\bin" }
# fnc-nx: SA2_WINSYSROOT = an xwin-style sysroot (VC\Tools\MSVC\<ver>, Windows Kits\10) instead of an installed VS
# 2022. It holds the same MSVC/Windows SDK libraries as the Build Tools: INCLUDE/LIB/VCToolsInstallDir are set the way
# vcvars64.bat sets them; the sysroot has no debug CRT, so configure with -DCMAKE_TRY_COMPILE_CONFIGURATION=Release (the presets do), and CMake/Ninja come from SA2_CMAKE_BIN and
# SA2_NINJA_BIN.
$sysroot = $env:SA2_WINSYSROOT
if (-not $sysroot) {
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found under $vs (or set SA2_WINSYSROOT)" }
}

if (-not $env:SA2_ENV_LOADED) {
    # devkitPro puts MSYS2's usr/bin on PATH, which has its own `cmd`, `link` and `find`: keep it out of this
    # session, and call Windows' cmd.exe by its full path.
    $env:PATH = (($env:PATH -split ';') | Where-Object { $_ -and $_ -notmatch 'msys2\\usr\\bin' }) -join ';'
    $env:PATH = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer;$env:PATH"
    if ($sysroot) {
        $sysroot = (Resolve-Path $sysroot).Path
        $msvc = (Get-ChildItem (Join-Path $sysroot 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1).FullName
        $kits = Join-Path $sysroot 'Windows Kits\10'
        $sdkv = (Get-ChildItem (Join-Path $kits 'Include') -Directory | Sort-Object Name | Select-Object -Last 1).Name
        $env:VCToolsInstallDir = "$msvc\"
        $env:WindowsSdkDir = "$kits\"
        $env:WindowsSDKVersion = "$sdkv\"
        $env:INCLUDE = (@("$msvc\include") + (@('ucrt', 'shared', 'um', 'winrt', 'cppwinrt') |
            ForEach-Object { "$kits\Include\$sdkv\$_" })) -join ';'
        $env:LIB = (@("$msvc\lib\x64") + (@('ucrt', 'um') | ForEach-Object { "$kits\Lib\$sdkv\$_\x64" })) -join ';'
        $env:PATH = "$llvm\bin;$($env:SA2_CMAKE_BIN);$($env:SA2_NINJA_BIN);$env:PATH"
    } else {
        & $env:ComSpec /c "`"$vcvars`" >nul && set" | ForEach-Object {
            if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
        }
        if (-not $env:VCToolsInstallDir) { throw "vcvars64.bat did not set up MSVC" }
        $env:PATH = "$llvm\bin;$cm\CMake\bin;$cm\Ninja;$env:PATH"
    }
    $env:CC = 'clang'
    $env:CXX = 'clang++'
    $env:SA2_ENV_LOADED = '1'
}
$ErrorActionPreference = 'Continue'
