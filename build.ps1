# Configure + build the DTH Character Studio Runner plugin (or just the parser tests).
#
# Examples:
#   .\build.ps1 -TestsOnly
#   .\build.ps1 -SdkDir "C:\Users\Public\Documents\My DAZ 3D Library\Daz Studio 6 SDK" -QtDir C:\Qt\6.10.3\msvc2022_64
#   .\build.ps1 -SdkDir "C:\Users\Public\Documents\My DAZ 3D Library\DAZStudio4.5+ SDK" -SdkVersion 4
#   ... -Install   # copies the DLL into the Daz plugins folder (needs an admin shell)

param(
    [string]$SdkDir,
    [ValidateSet("4", "6")]
    [string]$SdkVersion = "6",
    [string]$QtDir,                # Qt6 devkit root, e.g. C:\Qt\6.10.3\msvc2022_64 (SDK 6 only)
    [string]$DazStudioDir,         # for -Install; defaults per SDK version
    [switch]$Install,
    [switch]$TestsOnly
)

$ErrorActionPreference = "Stop"

# Prefer PATH cmake, fall back to the one bundled with VS Build Tools.
$cmake = "cmake"
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    $cmake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if (-not (Test-Path $cmake)) { throw "cmake not found (PATH or VS Build Tools bundle)" }
}
$ctest = Join-Path (Split-Path $cmake) "ctest.exe"

$buildDir = if ($TestsOnly) { "build-tests" } else { "build-sdk$SdkVersion" }

$configureArgs = @("-S", ".", "-B", $buildDir, "-G", "Visual Studio 17 2022", "-A", "x64")
if (-not $TestsOnly) {
    if (-not $SdkDir) { throw "Pass -SdkDir <path to the Daz Studio SDK root> (or use -TestsOnly)" }
    $configureArgs += "-DDAZ_SDK_DIR=$SdkDir", "-DDAZ_SDK_VERSION=$SdkVersion"
    if ($SdkVersion -eq "6") {
        if (-not $QtDir) { throw "SDK 6 needs -QtDir <Qt 6.10.x msvc2022_64 root> (see README for aqtinstall)" }
        $configureArgs += "-DQt6_DIR=$QtDir\lib\cmake\Qt6"
    }
}

& $cmake @configureArgs
if ($LASTEXITCODE -ne 0) { throw "configure failed" }

& $cmake --build $buildDir --config Release
if ($LASTEXITCODE -ne 0) { throw "build failed" }

& $ctest --test-dir $buildDir -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "tests failed" }

if ($Install -and -not $TestsOnly) {
    if (-not $DazStudioDir) {
        $DazStudioDir = if ($SdkVersion -eq "6") { "C:\Program Files\DAZ 3D\DAZStudio6" } else { "C:\Program Files\DAZ 3D\DAZStudio4" }
    }
    $dllName = if ($SdkVersion -eq "6") { "dsp_dthcharacterstudiorunner.dll" } else { "dthcharacterstudiorunner.dll" }
    $src = Join-Path $buildDir "Release\$dllName"
    $dst = Join-Path $DazStudioDir "plugins"
    Copy-Item $src $dst -Force
    Copy-Item ($src -replace "\.dll$", ".pdb") $dst -Force -ErrorAction SilentlyContinue
    Write-Host "Installed $dllName to $dst (restart Daz Studio to load it)"
}
