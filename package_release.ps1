param(
    [ValidatePattern('^\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.-]+)?$')]
    [string]$Version = '1.1.0',
    [ValidateSet('Release', 'RelWithDebInfo')]
    [string]$Configuration = 'Release',
    [string]$LoaderPath = '',
    [string]$LoaderVersion = '9.7.4',
    [string]$ExpectedLoaderSha256 =
        'D5A059AA467A7A7127C8F6169F79FA63FF0F55986EE9EB2FD9A281BEBF2AA2E6'
)

$ErrorActionPreference = 'Stop'

$sourceDirectory = $PSScriptRoot
$buildDirectory = Join-Path $sourceDirectory 'build-package-x86'
$distDirectory = Join-Path $sourceDirectory 'dist'
$pluginPackageName = "DS3RawMouseFix-$Version-PluginOnly"
$bundlePackageName = "DS3RawMouseFix-$Version-WithUltimateASILoader-$LoaderVersion"
$pluginStage = Join-Path $distDirectory $pluginPackageName
$bundleStage = Join-Path $distDirectory $bundlePackageName
$pluginArchive = Join-Path $distDirectory "$pluginPackageName.zip"
$bundleArchive = Join-Path $distDirectory "$bundlePackageName.zip"
$releaseChecksumPath = Join-Path $distDirectory "DS3RawMouseFix-$Version-ARCHIVES-SHA256.txt"

$projectText = Get-Content -Raw -LiteralPath (Join-Path $sourceDirectory 'CMakeLists.txt')
if ($projectText -notmatch "project\(DS3RawMouseFix VERSION $([regex]::Escape($Version)) ") {
    throw "CMake project version does not match package version $Version."
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw "Visual Studio locator not found: $vswhere"
}
$visualStudioRoot = (& $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath | Select-Object -First 1)
if (-not $visualStudioRoot) {
    throw 'A Visual Studio installation with x86 C++ build tools was not found.'
}

$vcvars = Join-Path $visualStudioRoot 'VC\Auxiliary\Build\vcvarsall.bat'
$cmake = Join-Path $visualStudioRoot `
    'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path -LiteralPath $vcvars -PathType Leaf) -or
    -not (Test-Path -LiteralPath $cmake -PathType Leaf)) {
    throw 'The Visual Studio C++ or CMake tools are incomplete.'
}

New-Item -ItemType Directory -Path $distDirectory -Force | Out-Null
foreach ($output in @($pluginStage, $pluginArchive, $releaseChecksumPath)) {
    if (Test-Path -LiteralPath $output) {
        throw "Release output already exists; refusing to overwrite it: $output"
    }
}

$resolvedLoader = ''
$actualLoaderHash = ''
if ($LoaderPath) {
    foreach ($output in @($bundleStage, $bundleArchive)) {
        if (Test-Path -LiteralPath $output) {
            throw "Release output already exists; refusing to overwrite it: $output"
        }
    }
    $resolvedLoader = (Resolve-Path -LiteralPath $LoaderPath).Path
    $loaderItem = Get-Item -LiteralPath $resolvedLoader
    if ($loaderItem.Name -ine 'dinput8.dll') {
        throw "Ultimate ASI Loader must be supplied as dinput8.dll: $resolvedLoader"
    }
    $actualLoaderVersion = $loaderItem.VersionInfo.FileVersion
    if ($actualLoaderVersion -ne $LoaderVersion) {
        throw "Loader version is $actualLoaderVersion; expected $LoaderVersion."
    }
    $actualLoaderHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $resolvedLoader).Hash
    if ($actualLoaderHash -ne $ExpectedLoaderSha256) {
        throw "Loader SHA-256 mismatch. Actual: $actualLoaderHash"
    }
    $loaderLicense = Join-Path $sourceDirectory 'third_party\Ultimate-ASI-Loader-LICENSE.txt'
    if (-not (Test-Path -LiteralPath $loaderLicense -PathType Leaf)) {
        throw "Ultimate ASI Loader license is missing: $loaderLicense"
    }
}

$buildCommand = 'call "{0}" x86 && "{1}" -S "{2}" -B "{3}" -G Ninja ' +
    '-DCMAKE_BUILD_TYPE={4} -DDS3_RAW_MOUSE_VERSION={5} ' +
    '-DDS3_RAW_MOUSE_BUILD_LEGACY_LOADER=OFF && "{1}" --build "{3}" --parallel'
$buildCommand = $buildCommand -f $vcvars, $cmake, $sourceDirectory, $buildDirectory,
    $Configuration, $Version
& cmd.exe /d /s /c $buildCommand
if ($LASTEXITCODE -ne 0) {
    throw "Release build failed with exit code $LASTEXITCODE."
}

$commonArtifacts = [ordered]@{
    'DS3RawMouse.asi' = Join-Path $buildDirectory 'DS3RawMouse.asi'
    'ds3_raw_mouse.ini' = Join-Path $buildDirectory 'ds3_raw_mouse.ini'
    'dinput8.ini' = Join-Path $buildDirectory 'dinput8.ini'
    'README.txt' = Join-Path $sourceDirectory 'README.md'
    'CHANGELOG.md' = Join-Path $sourceDirectory 'CHANGELOG.md'
    'LICENSE.txt' = Join-Path $sourceDirectory 'LICENSE'
}
foreach ($artifact in $commonArtifacts.GetEnumerator()) {
    if (-not (Test-Path -LiteralPath $artifact.Value -PathType Leaf)) {
        throw "Required release artifact is missing: $($artifact.Value)"
    }
}

function Copy-CommonArtifacts([string]$Destination) {
    New-Item -ItemType Directory -Path $Destination | Out-Null
    foreach ($artifact in $commonArtifacts.GetEnumerator()) {
        Copy-Item -LiteralPath $artifact.Value -Destination (Join-Path $Destination $artifact.Key)
    }
}

function Write-PackageChecksums([string]$Directory) {
    $checksumPath = Join-Path $Directory 'SHA256SUMS.txt'
    $lines = Get-ChildItem -LiteralPath $Directory -File |
        Where-Object { $_.Name -ne 'SHA256SUMS.txt' } |
        Sort-Object Name |
        ForEach-Object {
            $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash
            "$hash  $($_.Name)"
        }
    Set-Content -LiteralPath $checksumPath -Value $lines -Encoding ascii
}

Copy-CommonArtifacts $pluginStage
Write-PackageChecksums $pluginStage
Compress-Archive -Path (Join-Path $pluginStage '*') `
    -DestinationPath $pluginArchive -CompressionLevel Optimal

$createdArchives = @($pluginArchive)
if ($LoaderPath) {
    Copy-CommonArtifacts $bundleStage
    Copy-Item -LiteralPath $resolvedLoader -Destination (Join-Path $bundleStage 'dinput8.dll')
    Copy-Item -LiteralPath $loaderLicense -Destination $bundleStage

    $notices = @"
THIRD-PARTY NOTICES

Ultimate ASI Loader $LoaderVersion
Copyright (c) 2023 ThirteenAG
Upstream: https://github.com/ThirteenAG/Ultimate-ASI-Loader
Release: https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/tag/v$LoaderVersion
Bundled file: dinput8.dll
SHA-256: $actualLoaderHash
License: MIT; see Ultimate-ASI-Loader-LICENSE.txt

The loader binary is redistributed unmodified. DS3 Raw Mouse Fix is a separate project and is not
affiliated with or endorsed by Ultimate ASI Loader or its author.
"@
    Set-Content -LiteralPath (Join-Path $bundleStage 'THIRD_PARTY_NOTICES.txt') `
        -Value $notices -Encoding utf8
    Write-PackageChecksums $bundleStage
    Compress-Archive -Path (Join-Path $bundleStage '*') `
        -DestinationPath $bundleArchive -CompressionLevel Optimal
    $createdArchives += $bundleArchive
}

$archiveChecksums = $createdArchives | ForEach-Object {
    $archive = Get-Item -LiteralPath $_
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $archive.FullName).Hash
    "$hash  $($archive.Name)"
}
Set-Content -LiteralPath $releaseChecksumPath -Value $archiveChecksums -Encoding ascii

Write-Output "Created release packages:"
$createdArchives | ForEach-Object { Write-Output "  $_" }
Write-Output "Archive checksums: $releaseChecksumPath"
