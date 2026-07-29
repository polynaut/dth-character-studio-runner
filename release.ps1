# Cut a versioned release: build both SDK variants, tag, and publish a
# GitHub release with the two DLLs (zipped so the required install names -
# dsp_dthcharacterstudiorunner.dll for DS6, dthcharacterstudiorunner.dll for DS4 - stay intact).
#
# Releases are built LOCALLY because the Daz SDKs are store downloads that
# cannot exist on CI runners. GitHub Actions only guards the parser tests.
#
# Flow:
#   1. bump the version in src/version.h and commit
#   2. .\release.ps1            (add -Draft to publish as a draft release)
#
# The version in src/version.h is the single source of truth; the script
# refuses to run on a dirty tree or an already-released version.

param(
    # Build DS6 releases against the OLDEST supported 6.25 SDK, never a newer
    # "Latest" SDK drop: a newer SDK's import lib may reference dzcore exports
    # an older installed Studio lacks, and the DLL then fails to load. (The
    # v1.0.0/v1.0.1 load failures were the missing extern "C" on the plugin
    # entry points - see pluginmain.cpp - but the SDK-age rule stays as a
    # compatibility precaution.)
    [string]$Sdk6Dir = "D:\DAZ 3D\My DAZ 3D Library\Daz Studio 6.25+ BETA SDK",
    [string]$Sdk4Dir = "D:\DAZ 3D\My DAZ 3D Library\DAZStudio4.5+ SDK",
    [string]$QtDir = "C:\Qt\6.10.3\msvc2022_64",
    [switch]$Draft
)

$ErrorActionPreference = "Stop"

# -- Version from src/version.h -----------------------------------------------
$versionHeader = Get-Content src\version.h -Raw
$major = [regex]::Match($versionHeader, '#define PLUGIN_MAJOR\s+(\d+)').Groups[1].Value
$minor = [regex]::Match($versionHeader, '#define PLUGIN_MINOR\s+(\d+)').Groups[1].Value
$rev   = [regex]::Match($versionHeader, '#define PLUGIN_REV\s+(\d+)').Groups[1].Value
if (-not $major -or -not $minor -or -not $rev) { throw "could not parse src/version.h" }
$version = "$major.$minor.$rev"
$tag = "v$version"

# -- Preconditions ------------------------------------------------------------
git diff --quiet
if ($LASTEXITCODE -ne 0) { throw "working tree is dirty - commit first" }
git diff --cached --quiet
if ($LASTEXITCODE -ne 0) { throw "index has staged changes - commit first" }
git rev-parse -q --verify "refs/tags/$tag" *> $null
if ($LASTEXITCODE -eq 0) { throw "tag $tag already exists - bump src/version.h first" }

# -- Build both variants (each also runs the parser tests) --------------------
.\build.ps1 -SdkDir $Sdk6Dir -SdkVersion 6 -QtDir $QtDir
.\build.ps1 -SdkDir $Sdk4Dir -SdkVersion 4

# -- Stage versioned artifacts ------------------------------------------------
$stage = "build-release\$tag"
New-Item -ItemType Directory -Force $stage | Out-Null

$ds6Zip = "$stage\dth-character-studio-runner-$version-ds6.zip"
$ds4Zip = "$stage\dth-character-studio-runner-$version-ds4.zip"
Compress-Archive -Force -DestinationPath $ds6Zip -Path `
    "build-sdk6\Release\dsp_dthcharacterstudiorunner.dll", "build-sdk6\Release\dsp_dthcharacterstudiorunner.pdb"
Compress-Archive -Force -DestinationPath $ds4Zip -Path `
    "build-sdk4\Release\dthcharacterstudiorunner.dll", "build-sdk4\Release\dthcharacterstudiorunner.pdb"

# -- Tag + GitHub release -----------------------------------------------------
git tag $tag
if ($LASTEXITCODE -ne 0) { throw "git tag failed" }
git push origin $tag
if ($LASTEXITCODE -ne 0) { throw "git push failed" }

$notes = @"
DTH Character Studio Runner $version - Daz Studio plugin builds.

| File | Daz Studio | Install to |
| --- | --- | --- |
| ``dth-character-studio-runner-$version-ds6.zip`` | 6.25+ | ``<DAZStudio6>\plugins\`` (keep the name ``dsp_dthcharacterstudiorunner.dll``) |
| ``dth-character-studio-runner-$version-ds4.zip`` | 4.x | ``<DAZStudio4>\plugins\`` (keep the name ``dthcharacterstudiorunner.dll``) |

Unzip, copy the DLL into the plugins folder of the matching Daz Studio
(admin rights required), restart Daz Studio, then verify under
Help > About Installed Plugins. The PDB is optional (crash symbols).

Built against: Daz Studio 6.25 BETA SDK + Qt 6.10.3 (DS6), DAZ Studio 4.5+ SDK (DS4).
"@

$releaseArgs = @($tag, $ds6Zip, $ds4Zip, "--title", "DTH Character Studio Runner $version", "--notes", $notes)
if ($Draft) { $releaseArgs += "--draft" }
gh release create @releaseArgs
if ($LASTEXITCODE -ne 0) { throw "gh release create failed" }

Write-Host "`nReleased $tag"
