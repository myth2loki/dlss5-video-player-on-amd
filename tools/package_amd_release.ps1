[CmdletBinding()]
param(
    # Package a build for the AMD backend: the player, its neural helper, the
    # FFX DLLs it loads, and the LmxxfNrRuntime pack (amd-neural-runtime/).
    #
    # This is deliberately a separate entry point from tools/package_release.ps1.
    # That one assembles the upstream NVIDIA release: it demands a clean tagged
    # tree, a hardware test report and the locked NVIDIA NR/runtime inputs under
    # external/runtime, none of which a local AMD build has - and it knows
    # nothing about a several-hundred-megabyte runtime *directory*, because on
    # the NVIDIA backend the runtime is a set of single DLLs.
    [string]$Version,
    [string]$PackageSuffix = '-amd',
    [string]$BuildDirectory = 'build-ninja',
    # The runtime pack. Not committed (see .gitignore): it is the tree the
    # player actually loads, and the one that gets verified at startup.
    [string]$RuntimeDirectory,
    [switch]$IncludeFfmpeg,
    # The NVIDIA backend's SDK DLLs are ~66 MB and dead weight on an AMD-only
    # machine, so they can be dropped.
    [switch]$WithoutNvidiaBackend,
    # shaders\shader-cache\ is compiled output: the runtime regenerates it on
    # first use, and the render identity deliberately excludes it.
    [switch]$IncludeShaderCache,
    [switch]$NoZip,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repositoryRoot = Split-Path -Parent $PSScriptRoot
if (-not $Version) {
    $Version = (Get-Content -LiteralPath (Join-Path $repositoryRoot 'VERSION') -Raw).Trim()
}
if ($Version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') {
    throw "VERSION must read major.minor.patch; received '$Version'."
}
if (-not $RuntimeDirectory) {
    $RuntimeDirectory = Join-Path $repositoryRoot 'amd-neural-runtime'
}

$distRoot = Join-Path $repositoryRoot 'dist'
$stageName = "DLSSVideoPlayer-v$Version$PackageSuffix-win64"
$stageRoot = Join-Path $distRoot $stageName
$zipPath = Join-Path $distRoot "$stageName.zip"
$runtimeSource = (Resolve-Path -LiteralPath $RuntimeDirectory).Path
$buildRoot = Join-Path $repositoryRoot $BuildDirectory

function Get-Sha256 {
    param([string]$Path)
    $stream = [IO.File]::OpenRead($Path)
    try {
        $sha256 = [Security.Cryptography.SHA256]::Create()
        try { return [BitConverter]::ToString($sha256.ComputeHash($stream)).Replace('-', '') }
        finally { $sha256.Dispose() }
    }
    finally { $stream.Dispose() }
}

function Get-RelativePackagePath {
    param([string]$Root, [string]$Path)
    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    $pathFull = [IO.Path]::GetFullPath($Path)
    if (-not $pathFull.StartsWith($rootFull, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path is outside package root: $pathFull"
    }
    return $pathFull.Substring($rootFull.Length).Replace('\', '/')
}

function Get-OrdinalPackageFiles {
    param([string]$Root)
    $filesByPath = @{}
    foreach ($file in Get-ChildItem -LiteralPath $Root -Recurse -File) {
        $relative = Get-RelativePackagePath -Root $Root -Path $file.FullName
        $filesByPath[$relative] = $file
    }
    [string[]]$relativePaths = @($filesByPath.Keys)
    [Array]::Sort($relativePaths, [StringComparer]::Ordinal)
    foreach ($relative in $relativePaths) { $filesByPath[$relative] }
}

# The runtime's own manifests, checked here so a package is never built from a
# tree the player would refuse at startup ("SHA256SUMS incomplete" from Create,
# or a drift report on the start screen). Same files, same rule: each line is
# `<64 hex>  <path relative to the manifest's directory>`.
function Assert-RuntimeModules {
    param([string]$Root)
    $total = 0
    foreach ($arch in @('gfx1200', 'gfx1201')) {
        $archRoot = Join-Path (Join-Path $Root 'lmxxf-modules') $arch
        if (-not (Test-Path -LiteralPath $archRoot -PathType Container)) { throw "Runtime pack is missing lmxxf-modules\$arch." }
        $manifest = Join-Path $archRoot 'SHA256SUMS'
        if (-not (Test-Path -LiteralPath $manifest -PathType Leaf)) { throw "Runtime pack is missing lmxxf-modules\$arch\SHA256SUMS." }
        $entries = @(Get-Content -LiteralPath $manifest | Where-Object { $_ -match '\S' })
        # Create wants the 40-per-arch set; the stale native-game-tiled-assets\HIP
        # subset is 24 and is rejected, so the count is part of the contract.
        if ($entries.Count -ne 40) {
            throw "lmxxf-modules\$arch\SHA256SUMS lists $($entries.Count) modules; the runtime wants 40."
        }
        foreach ($entry in $entries) {
            if ($entry -notmatch '^([0-9a-fA-F]{64})\s+(\S.*)$') { throw "Unreadable SHA256SUMS line in $arch`: $entry" }
            $expected = $Matches[1].ToLowerInvariant()
            $relative = $Matches[2].Trim()
            $path = Join-Path $archRoot $relative
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Module listed but absent: lmxxf-modules\$arch\$relative" }
            # -ne, not -cne: Get-Sha256 formats the digest in upper case.
            if ((Get-Sha256 -Path $path) -ne $expected) { throw "Module drifted from its manifest: lmxxf-modules\$arch\$relative" }
            $total++
        }
    }
    Write-Host "Runtime modules verified: $total HSACOs against their SHA256SUMS."
}

function Assert-RuntimeLayout {
    param([string]$Root)
    foreach ($required in @('LmxxfNrRuntime.dll', 'lmxxf-modules', 'native-game-tiled-assets', 'shaders')) {
        $path = Join-Path $Root $required
        if (-not (Test-Path -LiteralPath $path)) { throw "Runtime pack is missing $required`: $path" }
    }
    $dll = Join-Path $Root 'LmxxfNrRuntime.dll'
    $dllInfo = Get-Item -LiteralPath $dll
    Write-Host "Runtime pack: $Root"
    Write-Host ("  LmxxfNrRuntime.dll {0:N0} bytes, {1:yyyy-MM-dd HH:mm}" -f $dllInfo.Length, $dllInfo.LastWriteTime)

    $weights = @(Get-ChildItem -LiteralPath (Join-Path $Root 'native-game-tiled-assets') -File)
    if ($weights.Count -eq 0) { throw 'native-game-tiled-assets is empty; the network has no weights.' }
    $shaders = @(Get-ChildItem -LiteralPath (Join-Path $Root 'shaders') -File -Filter '*.hlsl')
    if ($shaders.Count -eq 0) { throw 'shaders/ carries no .hlsl sources.' }
    $config = Join-Path (Join-Path $Root 'DLSS5-AMD') 'custom-config.txt'
    if (-not (Test-Path -LiteralPath $config -PathType Leaf)) {
        Write-Warning "Runtime pack has no DLSS5-AMD\custom-config.txt; the player writes one on the first AMD job."
    }
    Assert-RuntimeModules -Root $Root
}

# packaging/tool-lock.json pins the helper binaries by size and SHA-256, and the
# provenance written beside them is only true for those bytes: staging a
# rebuild (or a different BtbN autobuild) would ship a package that no longer
# matches the notice it carries.
$script:toolLock = $null
function Assert-ToolLock {
    param([string]$Name, [string]$Path)
    $lockPath = Join-Path (Join-Path $repositoryRoot 'packaging') 'tool-lock.json'
    if ($null -eq $script:toolLock) { $script:toolLock = Get-Content -LiteralPath $lockPath -Raw | ConvertFrom-Json }
    $entry = @($script:toolLock.entries | Where-Object { [string]$_.name -ceq $Name })
    if ($entry.Count -ne 1) { throw "tool-lock.json has $($entry.Count) entries for '$Name'." }
    $item = Get-Item -LiteralPath $Path
    if ($item.Length -ne [int64]$entry[0].size) {
        throw "Locked helper drifted: $Name is $($item.Length) bytes, tool-lock.json says $($entry[0].size)."
    }
    if ((Get-Sha256 -Path $Path) -ne [string]$entry[0].sha256) {
        throw "Locked helper drifted: $Name does not match tool-lock.json's SHA-256."
    }
    Write-Host "Tool lock verified: $Name"
}

# Single-file inputs. $Source is resolved from the first existing candidate.
$sources = @(
    @{ Destination = 'DLSSVideoPlayer.exe'; Required = $true; Candidates = @((Join-Path $buildRoot 'DLSSVideoPlayer.exe')) },
    @{ Destination = 'neural-runtime/NeuralWorker.exe'; Required = $true; Candidates = @(
        (Join-Path $buildRoot 'Release\neural-runtime\NeuralWorker.exe'),
        (Join-Path $buildRoot 'neural-runtime\NeuralWorker.exe'),
        (Join-Path $repositoryRoot 'run-fg\neural-runtime\NeuralWorker.exe')) },
    @{ Destination = 'amd_fidelityfx_framegeneration_dx12.dll'; Required = $true; Candidates = @(
        (Join-Path $buildRoot 'amd_fidelityfx_framegeneration_dx12.dll'),
        (Join-Path $repositoryRoot 'run-fg\amd_fidelityfx_framegeneration_dx12.dll')) },
    @{ Destination = 'amd_fidelityfx_loader_dx12.dll'; Required = $true; Candidates = @(
        (Join-Path $buildRoot 'amd_fidelityfx_loader_dx12.dll'),
        (Join-Path $repositoryRoot 'run-fg\amd_fidelityfx_loader_dx12.dll')) },
    # The FSR4/FSR1 upscaler DLL is a manual SDK drop, not a build output, so
    # the run directory is the only place that has it.
    @{ Destination = 'amd_fidelityfx_upscaler_dx12.dll'; Required = $false; Candidates = @(
        (Join-Path $buildRoot 'amd_fidelityfx_upscaler_dx12.dll'),
        (Join-Path $repositoryRoot 'run-fg\amd_fidelityfx_upscaler_dx12.dll')) },
    @{ Destination = 'nvngx_dlss.dll'; Required = $true; SkipWhen = $WithoutNvidiaBackend; Candidates = @((Join-Path $buildRoot 'nvngx_dlss.dll')) },
    @{ Destination = 'nvngx_dlssg.dll'; Required = $true; SkipWhen = $WithoutNvidiaBackend; Candidates = @((Join-Path $buildRoot 'nvngx_dlssg.dll')) },
    @{ Destination = 'LICENSE'; Required = $false; Candidates = @((Join-Path $repositoryRoot 'LICENSE')) },
    @{ Destination = 'AMD_RUNTIME_NOTICE.txt'; Required = $true; Candidates = @((Join-Path $repositoryRoot 'packaging\AMD_RUNTIME_NOTICE.txt')) }
)
if ($IncludeFfmpeg) {
    # The player runs ffmpeg and ffprobe as child processes and links no libav*,
    # so these two files are the whole decoder, the whole probe and the encoder
    # the export path shells out to: without them the package opens no video at
    # all (AudioPlayer logs "ffmpeg.exe not found" and the decode child cannot
    # start). Kept opt-in only because the pair is ~330 MB.
    $sources += @(
        @{ Destination = 'ffmpeg.exe'; Required = $true; Locked = $true; Candidates = @((Join-Path $repositoryRoot 'external\ffmpeg\bin\ffmpeg.exe'), (Join-Path $repositoryRoot 'run-fg\ffmpeg.exe')) },
        @{ Destination = 'ffprobe.exe'; Required = $true; Locked = $true; Candidates = @((Join-Path $repositoryRoot 'external\ffmpeg\bin\ffprobe.exe'), (Join-Path $repositoryRoot 'run-fg\ffprobe.exe')) },
        # GPLv3 section 4: a binary copy is conveyed with its license, and this
        # build carries permissively licensed libraries whose notices travel
        # with it too. The upstream assembler stages the same three files.
        @{ Destination = 'THIRD_PARTY_LICENSES/ffmpeg.txt'; Required = $true; Candidates = @((Join-Path $repositoryRoot 'THIRD_PARTY_LICENSES\ffmpeg.txt')) },
        @{ Destination = 'THIRD_PARTY_LICENSES/ffmpeg-GPL-3.0.txt'; Required = $true; Candidates = @((Join-Path $repositoryRoot 'THIRD_PARTY_LICENSES\ffmpeg-GPL-3.0.txt')) },
        @{ Destination = 'THIRD_PARTY_LICENSES/ffmpeg-bundled-notices.txt'; Required = $true; Candidates = @((Join-Path $repositoryRoot 'THIRD_PARTY_LICENSES\ffmpeg-bundled-notices.txt')) }
    )
}

$resolved = New-Object Collections.Generic.List[object]
foreach ($source in $sources) {
    if ($source.ContainsKey('SkipWhen') -and $source.SkipWhen) { continue }
    $found = $null
    foreach ($candidate in $source.Candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { $found = $candidate; break }
    }
    if (-not $found) {
        if ($source.Required) { throw "Required package input is missing: $($source.Candidates -join ' | ')" }
        Write-Warning "Skipping optional input '$($source.Destination)': no candidate exists."
        continue
    }
    if ($source.ContainsKey('Locked') -and $source.Locked) { Assert-ToolLock -Name $source.Destination -Path $found }
    $resolved.Add([pscustomobject]@{ Destination = $source.Destination; Source = $found })
}

Assert-RuntimeLayout -Root $runtimeSource

New-Item -ItemType Directory -Path $distRoot -Force | Out-Null
$distFull = [IO.Path]::GetFullPath($distRoot).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
$stageFull = [IO.Path]::GetFullPath($stageRoot)
if (-not $stageFull.StartsWith($distFull, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe stage path.' }
if (Test-Path -LiteralPath $stageFull) {
    if (-not $Force) { throw "Package output already exists: $stageFull (pass -Force to replace it)." }
    Remove-Item -LiteralPath $stageFull -Recurse -Force
}
New-Item -ItemType Directory -Path $stageFull | Out-Null

foreach ($item in $resolved) {
    $destination = Join-Path $stageFull $item.Destination
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath $item.Source -Destination $destination
    Write-Host "Staged $($item.Destination) <- $($item.Source)"
}

# The runtime tree is copied file by file so the compiled shader cache and the
# runtime's own scratch log can be left out without carrying an exclusion
# mechanism the release script does not have.
$runtimeDestination = Join-Path $stageFull 'amd-neural-runtime'
$copied = 0
$excluded = 0
foreach ($file in Get-ChildItem -LiteralPath $runtimeSource -Recurse -File) {
    $relative = Get-RelativePackagePath -Root $runtimeSource -Path $file.FullName
    if (-not $IncludeShaderCache -and $relative.StartsWith('shaders/shader-cache/', [StringComparison]::OrdinalIgnoreCase)) { $excluded++; continue }
    if ($relative -like '*.log') { $excluded++; continue }
    $destination = Join-Path $runtimeDestination $relative.Replace('/', '\')
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath $file.FullName -Destination $destination
    $copied++
}
Write-Host "Staged amd-neural-runtime: $copied files ($excluded skipped)."

$files = @(Get-OrdinalPackageFiles -Root $stageFull)
$manifest = New-Object Collections.Generic.List[string]
$manifest.Add("ProductVersion=$Version")
$manifest.Add("RuntimePack=amd-neural-runtime (LmxxfNrRuntime.dll $(Get-Sha256 -Path (Join-Path $runtimeSource 'LmxxfNrRuntime.dll')))")
$manifest.Add('Path|Size|SHA256')
foreach ($file in $files) {
    $relative = Get-RelativePackagePath -Root $stageFull -Path $file.FullName
    $manifest.Add("$relative|$($file.Length)|$(Get-Sha256 -Path $file.FullName)")
}
$utf8NoBom = New-Object Text.UTF8Encoding $false
[IO.File]::WriteAllLines((Join-Path $stageFull 'PACKAGE_MANIFEST.txt'), $manifest, $utf8NoBom)

$stageBytes = ($files | Measure-Object -Property Length -Sum).Sum
Write-Host ("Staged {0} files, {1:N0} bytes: {2}" -f $files.Count, $stageBytes, $stageFull)

if ($NoZip) {
    Write-Host 'Zip skipped (-NoZip).'
    return
}

if (Test-Path -LiteralPath $zipPath) {
    if (-not $Force) { throw "Archive already exists: $zipPath (pass -Force to replace it)." }
    Remove-Item -LiteralPath $zipPath -Force
}

# Compress-Archive rather than a hand-rolled ZipArchive loop: Add-Type (which
# the loop needs to load System.IO.Compression in a bare host) is refused by
# this machine's policy, and the cmdlet streams the same deflate output.
Write-Host "Compressing $stageName ..."
Compress-Archive -LiteralPath $stageFull -DestinationPath $zipPath -CompressionLevel Optimal

$zipBytes = (Get-Item -LiteralPath $zipPath).Length
Write-Host ("Created '{0}' ({1:N0} bytes, SHA-256 {2})." -f $zipPath, $zipBytes, (Get-Sha256 -Path $zipPath))
