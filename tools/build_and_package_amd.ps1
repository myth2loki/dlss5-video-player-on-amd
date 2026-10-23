[CmdletBinding()]
param(
    # Builds the AMD player from source and assembles the release package under
    # dist\ in one step: the same build tools\build_windows.bat would drive, then
    # tools\package_amd_release.ps1.
    #
    # Two reasons this exists rather than "run build_windows.bat, then
    # package_amd_release.bat":
    #   * the local AMD tree is configured as build-ninja (single-config Ninja),
    #     which build_windows.bat does not know about, and
    #   * packaging picks NeuralWorker.exe from build\ first and from run-fg\
    #     second. A build that did not stage the worker therefore ships the run
    #     directory's copy without a word - the exact failure that made an A/B
    #     of two different evaluators produce byte-identical frames. This script
    #     refuses to package when the build did not produce the worker.
    [string]$BuildDirectory = 'build-ninja',
    [string]$Configuration = 'Release',
    [string[]]$Targets = @('DLSSVideoPlayer'),
    # 0 uses one job per logical processor.
    [int]$Jobs = 0,
    # Configure even when the build directory already has a cache.
    [switch]$Configure,
    [string]$Generator = 'Ninja',
    # Run the non-GPU test suite before packaging; a failure stops the package.
    [switch]$Test,

    [switch]$SkipBuild,
    [switch]$NoPackage,

    # Packaging options, forwarded as-is.
    [string]$Version,
    [string]$PackageSuffix = '-amd',
    [switch]$IncludeFfmpeg,
    [switch]$WithoutNvidiaBackend,
    [switch]$IncludeShaderCache,
    [switch]$NoZip,
    # Without this, an existing dist\<stage> aborts the run instead of being
    # replaced.
    [switch]$NoForce,

    # Toolchain overrides. Both default to autodetection: the MSVC toolset is
    # read back from the build directory's own CMakeCache so an incremental
    # build keeps the compiler it was configured with.
    [string]$MsvcRoot,
    [string]$WindowsSdkRoot,
    [string]$CMake
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$script:overallTimer = [Diagnostics.Stopwatch]::StartNew()

# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

function Write-Step {
    param([string]$Text)
    Write-Host ''
    Write-Host "==> $Text"
}

function Get-CMakeCacheValue {
    param([string]$CachePath, [string]$Name)

    if (-not (Test-Path -LiteralPath $CachePath -PathType Leaf)) { return $null }
    $pattern = '^' + [regex]::Escape($Name) + ':[^=]*=(.*)$'
    foreach ($line in Get-Content -LiteralPath $CachePath) {
        if ($line -match $pattern) { return $Matches[1] }
    }
    return $null
}

function Add-UniquePath {
    param($List, [string]$Value)

    if (-not $Value) { return }
    if (-not $List.Contains($Value)) { $List.Add($Value) }
}

# Visual Studio / Build Tools installation roots (the directory that holds
# Common7 and VC). vswhere is authoritative when present; the explicit list
# afterwards covers Build Tools installed outside the two Program Files trees,
# which is how this workstation has it (F:\DevTools\BuildTools).
function Get-VisualStudioRoots {
    $roots = New-Object Collections.Generic.List[string]
    if ($MsvcRoot) {
        Add-UniquePath $roots $MsvcRoot
        return $roots
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
        try {
            $found = @(& $vswhere -products * -property installationPath)
            foreach ($item in $found) {
                if ($item) { Add-UniquePath $roots $item.Trim() }
            }
        }
        catch {
            # vswhere shells out to the registry through COM; a sandbox that
            # blocks that must not turn into "no toolchain found" before the
            # explicit candidates below have had their turn.
        }
    }

    foreach ($base in @($env:ProgramFiles, ${env:ProgramFiles(x86)})) {
        if (-not $base) { continue }
        foreach ($flavour in @('18', '2022')) {
            foreach ($edition in @('Community', 'Professional', 'Enterprise', 'BuildTools')) {
                $candidate = Join-Path $base "Microsoft Visual Studio\$flavour\$edition"
                if (Test-Path -LiteralPath (Join-Path $candidate 'VC\Tools\MSVC') -PathType Container) {
                    Add-UniquePath $roots $candidate
                }
            }
        }
    }
    foreach ($drive in @('C', 'D', 'E', 'F', 'G')) {
        foreach ($layout in @('DevTools\BuildTools', 'BuildTools')) {
            $candidate = "${drive}:\$layout"
            if (Test-Path -LiteralPath (Join-Path $candidate 'VC\Tools\MSVC') -PathType Container) {
                Add-UniquePath $roots $candidate
            }
        }
    }
    return $roots
}

# The build directory records the exact compiler it was configured with. An
# incremental build must keep using it: picking the newest installed toolset
# instead silently reconfigures and rebuilds everything.
function Get-ConfiguredMsvcRoot {
    param([string]$CachePath)

    $compiler = Get-CMakeCacheValue -CachePath $CachePath -Name 'CMAKE_CXX_COMPILER'
    if (-not $compiler) { return $null }
    if ($compiler -match '^(.*[\\/]VC[\\/]Tools[\\/]MSVC[\\/][^\\/]+)[\\/]bin[\\/]Hostx64[\\/]x64[\\/]cl\.exe$') {
        if (Test-Path -LiteralPath $Matches[1] -PathType Container) { return $Matches[1] }
    }
    return $null
}

function Resolve-MsvcToolset {
    param([string]$CachePath)

    $configured = Get-ConfiguredMsvcRoot -CachePath $CachePath
    if ($configured) { return $configured }

    $candidates = New-Object Collections.Generic.List[string]
    foreach ($root in Get-VisualStudioRoots) {
        $toolsRoot = Join-Path $root 'VC\Tools\MSVC'
        if (-not (Test-Path -LiteralPath $toolsRoot -PathType Container)) { continue }
        foreach ($dir in Get-ChildItem -LiteralPath $toolsRoot -Directory) {
            if (Test-Path -LiteralPath (Join-Path $dir.FullName 'bin\Hostx64\x64\cl.exe') -PathType Leaf) {
                $candidates.Add($dir.FullName)
            }
        }
    }
    if ($candidates.Count -eq 0) {
        throw "No MSVC x64 toolset was found. Point -MsvcRoot at a Visual Studio or Build Tools installation root (the directory that holds VC\Tools\MSVC)."
    }
    $sorted = @($candidates | Sort-Object -Property @{ Expression = { [version](Split-Path -Leaf $_) } } -Descending)
    return $sorted[0]
}

function Resolve-WindowsSdk {
    param([string]$PreferredVersion)

    $roots = New-Object Collections.Generic.List[string]
    if ($WindowsSdkRoot) {
        Add-UniquePath $roots $WindowsSdkRoot
    }
    else {
        Add-UniquePath $roots (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10')
        Add-UniquePath $roots 'C:\Program Files (x86)\Windows Kits\10'
    }

    foreach ($root in $roots) {
        $include = Join-Path $root 'Include'
        if (-not (Test-Path -LiteralPath $include -PathType Container)) { continue }
        $available = @(Get-ChildItem -LiteralPath $include -Directory |
            Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } |
            Where-Object { Test-Path -LiteralPath (Join-Path $root ("Lib\{0}\um\x64\kernel32.lib" -f $_.Name)) -PathType Leaf })
        if ($available.Count -eq 0) { continue }
        if ($PreferredVersion -and ($available | Where-Object { $_.Name -eq $PreferredVersion })) {
            return @{ Root = $root; Version = $PreferredVersion }
        }
        $sorted = @($available | Sort-Object -Property @{ Expression = { [version]$_.Name } } -Descending)
        return @{ Root = $root; Version = $sorted[0].Name }
    }
    throw "No Windows 10/11 SDK was found. Point -WindowsSdkRoot at the 'Windows Kits\10' directory."
}

function Test-ExistingBuildEnvironment {
    if (-not $env:INCLUDE) { return $false }
    if (-not $env:LIB) { return $false }
    return [bool](Get-Command 'cl.exe' -ErrorAction SilentlyContinue)
}

# Ninja invokes cl.exe with an absolute path but takes the include and library
# search paths from the environment, exactly like an MSBuild build started from
# a Developer prompt. vcvars64.bat would set them up, but it goes through
# reg.exe, which is not always callable (and is unnecessary: the four variables
# are derivable from the toolset and SDK directories).
function Set-MsvcBuildEnvironment {
    param([string]$CachePath, [string]$CMakeDirectory)

    if (Test-ExistingBuildEnvironment) {
        Write-Host '    Using the MSVC environment already present in this shell.'
    }
    else {
        $toolset = Resolve-MsvcToolset -CachePath $CachePath
        $systemVersion = Get-CMakeCacheValue -CachePath $CachePath -Name 'CMAKE_SYSTEM_VERSION'
        $sdk = Resolve-WindowsSdk -PreferredVersion $systemVersion

        Write-Host "    MSVC toolset : $toolset"
        Write-Host ("    Windows SDK  : {0} ({1})" -f $sdk.Version, $sdk.Root)

        $env:PATH = "$toolset\bin\Hostx64\x64;$($sdk.Root)\bin\$($sdk.Version)\x64;$env:PATH"
        $env:INCLUDE = "$toolset\include;$($sdk.Root)\Include\$($sdk.Version)\um;$($sdk.Root)\Include\$($sdk.Version)\shared;$($sdk.Root)\Include\$($sdk.Version)\winrt;$($sdk.Root)\Include\$($sdk.Version)\ucrt"
        $env:LIB = "$toolset\lib\x64;$($sdk.Root)\Lib\$($sdk.Version)\um\x64;$($sdk.Root)\Lib\$($sdk.Version)\ucrt\x64"
    }

    if ($CMakeDirectory -and (Test-Path -LiteralPath $CMakeDirectory -PathType Container)) {
        $env:PATH = "$CMakeDirectory;$env:PATH"
    }
}

function Resolve-CMakeExe {
    param([string]$CachePath, [string]$Explicit)

    if ($Explicit) {
        if (-not (Test-Path -LiteralPath $Explicit -PathType Leaf)) { throw "cmake.exe was not found at '$Explicit'." }
        return (Resolve-Path -LiteralPath $Explicit).Path
    }
    $cached = Get-CMakeCacheValue -CachePath $CachePath -Name 'CMAKE_COMMAND'
    if ($cached -and (Test-Path -LiteralPath $cached -PathType Leaf)) { return $cached }
    $onPath = Get-Command 'cmake.exe' -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    foreach ($root in Get-VisualStudioRoots) {
        $candidate = Join-Path $root 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
    }
    throw 'cmake.exe was not found: it is neither -CMake, nor in the build cache, nor on PATH, nor in a Visual Studio install.'
}

function Show-Artifact {
    param([string]$Path, [string]$Label)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label was not produced: $Path"
    }
    $item = Get-Item -LiteralPath $Path
    $hash = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.Substring(0, 12)
    # Printed, not assumed: packaging takes NeuralWorker.exe from the build
    # directory first and from run-fg\ second, so the bytes that ship are worth
    # a look even on a run where nothing looked wrong.
    Write-Host ("    {0,-16} {1,12:N0} bytes  {2:yyyy-MM-dd HH:mm:ss}  sha256:{3}" -f $Label, $item.Length, $item.LastWriteTime, $hash)
}

# Candidate order matches package_amd_release.ps1 exactly, so the hash printed
# below is the file that actually ships rather than a sibling copy of it.
function Resolve-WorkerPath {
    param([string]$BuildRoot, [string]$BuildConfiguration)

    foreach ($candidate in @(
            (Join-Path $BuildRoot "$BuildConfiguration\neural-runtime\NeuralWorker.exe"),
            (Join-Path $BuildRoot "neural-runtime\NeuralWorker.exe"))) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
    }
    return $null
}

# ---------------------------------------------------------------------------
# build
# ---------------------------------------------------------------------------

$buildFull = Join-Path $repositoryRoot $BuildDirectory
if ([IO.Path]::IsPathRooted($BuildDirectory)) {
    throw "-BuildDirectory must be relative to the repository root; received '$BuildDirectory'."
}
$cachePath = Join-Path $buildFull 'CMakeCache.txt'

Push-Location -LiteralPath $repositoryRoot
try {
    $cmakeExe = Resolve-CMakeExe -CachePath $cachePath -Explicit $CMake
    Write-Host "==> dlss5-video-player AMD build + package"
    Write-Host "    repository : $repositoryRoot"
    Write-Host "    build dir  : $BuildDirectory"
    Write-Host "    cmake      : $cmakeExe"

    $generator = Get-CMakeCacheValue -CachePath $cachePath -Name 'CMAKE_GENERATOR'
    $multiConfig = [bool]($generator -and ($generator -match 'Visual Studio|Xcode|Multi-Config'))

    if (-not $SkipBuild) {
        Write-Step 'Toolchain'
        Set-MsvcBuildEnvironment -CachePath $cachePath -CMakeDirectory (Split-Path -Parent $cmakeExe)

        $needsConfigure = $Configure -or (-not (Test-Path -LiteralPath $cachePath -PathType Leaf))
        if ($needsConfigure) {
            $dlssSdk = Join-Path $repositoryRoot 'external\DLSS'
            $ffmpegBin = Join-Path $repositoryRoot 'external\ffmpeg\bin'
            if (-not (Test-Path -LiteralPath (Join-Path $dlssSdk 'include\nvsdk_ngx.h') -PathType Leaf)) {
                throw "The pinned NVIDIA DLSS SDK checkout is missing: $dlssSdk. See docs\BUILDING.md."
            }
            if (-not (Test-Path -LiteralPath (Join-Path $ffmpegBin 'ffmpeg.exe') -PathType Leaf)) {
                throw "Verified FFmpeg input is missing: $ffmpegBin\ffmpeg.exe. See docs\BUILDING.md."
            }

            Write-Step "Configuring $Generator $Configuration"
            $configureArgs = @('-S', '.', '-B', $BuildDirectory, '-G', $Generator)
            if (-not $multiConfig) { $configureArgs += "-DCMAKE_BUILD_TYPE=$Configuration" }
            $configureArgs += @(
                '-DBUILD_TESTING=ON',
                "-DDLSS_SDK=$dlssSdk",
                "-DFFMPEG_STAGED_DIR=$ffmpegBin"
            )
            & $cmakeExe @configureArgs
            if ($LASTEXITCODE -ne 0) { throw "Configure failed (exit $LASTEXITCODE)." }
            $generator = Get-CMakeCacheValue -CachePath $cachePath -Name 'CMAKE_GENERATOR'
            $multiConfig = [bool]($generator -and ($generator -match 'Visual Studio|Xcode|Multi-Config'))
        }
        else {
            Write-Host ("    Reusing the existing cache (generator: {0}). Pass -Configure to reconfigure." -f $generator)
        }

        if ($Jobs -le 0) { $Jobs = [Math]::Max(1, [Environment]::ProcessorCount) }
        $buildTimer = [Diagnostics.Stopwatch]::StartNew()
        Write-Step ("Building {0} ({1} jobs)" -f ($Targets -join ', '), $Jobs)
        $buildArgs = @('--build', $BuildDirectory, '--parallel', $Jobs)
        if ($multiConfig) { $buildArgs += @('--config', $Configuration) }
        if ($Targets.Count -gt 0) {
            $buildArgs += '--target'
            $buildArgs += $Targets
        }
        & $cmakeExe @buildArgs
        $buildExit = $LASTEXITCODE
        $buildTimer.Stop()
        if ($buildExit -ne 0) { throw "Build failed (exit $buildExit)." }
        Write-Host ("    Build finished in {0:N1}s." -f $buildTimer.Elapsed.TotalSeconds)

        if ($Test) {
            Write-Step 'Testing (non-GPU suites)'
            $ctestExe = Join-Path (Split-Path -Parent $cmakeExe) 'ctest.exe'
            if (-not (Test-Path -LiteralPath $ctestExe -PathType Leaf)) { $ctestExe = 'ctest.exe' }
            $ctestArgs = @('--test-dir', $BuildDirectory)
            if ($multiConfig) { $ctestArgs += @('-C', $Configuration) }
            $ctestArgs += @('-LE', 'gpu|audio', '--output-on-failure')
            & $ctestExe @ctestArgs
            if ($LASTEXITCODE -ne 0) {
                throw ("ctest failed (exit {0}); nothing was packaged. If the failures are the known PlayerUiRegressionTests cases, run again without -Test to package anyway." -f $LASTEXITCODE)
            }
        }

        Write-Step 'Build outputs'
        # package_amd_release.ps1 reads the player from the build directory's
        # root, which is where the single-config generators put it. A
        # multi-config generator writes <build>\Release instead, so say that
        # here rather than let packaging fail with "required input is missing".
        $playerExe = Join-Path $buildFull 'DLSSVideoPlayer.exe'
        if (-not (Test-Path -LiteralPath $playerExe -PathType Leaf)) {
            $nested = Join-Path $buildFull "$Configuration\DLSSVideoPlayer.exe"
            if (Test-Path -LiteralPath $nested -PathType Leaf) {
                throw ("The player was built into {0}, but the packaging script only reads the build directory's root. Use the default single-config Ninja build for packaging." -f $nested)
            }
            throw "The player was not produced: $playerExe"
        }
        Show-Artifact -Path $playerExe -Label 'player'

        $workerExe = Resolve-WorkerPath -BuildRoot $buildFull -BuildConfiguration $Configuration
        if (-not $workerExe) {
            throw ("The build did not stage NeuralWorker.exe under {0}, and package_amd_release.ps1 would then fall back to run-fg\neural-runtime\NeuralWorker.exe - a worker from another build. Refusing to package." -f $buildFull)
        }
        Show-Artifact -Path $workerExe -Label 'worker'
    }
    else {
        Write-Step 'Skipping the build (-SkipBuild)'
    }

    # -----------------------------------------------------------------------
    # package
    # -----------------------------------------------------------------------
    if (-not $NoPackage) {
        $packArgs = @{ BuildDirectory = $BuildDirectory; PackageSuffix = $PackageSuffix }
        if (-not $NoForce) { $packArgs['Force'] = $true }
        if ($Version) { $packArgs['Version'] = $Version }
        if ($IncludeFfmpeg) { $packArgs['IncludeFfmpeg'] = $true }
        if ($WithoutNvidiaBackend) { $packArgs['WithoutNvidiaBackend'] = $true }
        if ($IncludeShaderCache) { $packArgs['IncludeShaderCache'] = $true }
        if ($NoZip) { $packArgs['NoZip'] = $true }

        Write-Step 'Packaging into dist\'
        & (Join-Path $PSScriptRoot 'package_amd_release.ps1') @packArgs

        if (-not $Version) {
            $Version = (Get-Content -LiteralPath (Join-Path $repositoryRoot 'VERSION') -Raw).Trim()
        }
        $stagePath = Join-Path (Join-Path $repositoryRoot 'dist') "DLSSVideoPlayer-v$Version$PackageSuffix-win64"
        if (Test-Path -LiteralPath $stagePath -PathType Container) {
            $files = @(Get-ChildItem -LiteralPath $stagePath -Recurse -File)
            $bytes = ($files | Measure-Object -Property Length -Sum).Sum
            Write-Host ''
            Write-Host ("==> Package ready: {0}" -f $stagePath)
            Write-Host ("    {0} files, {1:N1} MB" -f $files.Count, ($bytes / 1MB))
            if (-not $NoZip) {
                $zipPath = "$stagePath.zip"
                if (Test-Path -LiteralPath $zipPath -PathType Leaf) {
                    Write-Host ("    {0} ({1:N1} MB)" -f $zipPath, ((Get-Item -LiteralPath $zipPath).Length / 1MB))
                }
            }
        }
    }

    $script:overallTimer.Stop()
    Write-Host ''
    Write-Host ("==> Done in {0:N1}s." -f $script:overallTimer.Elapsed.TotalSeconds)
}
finally {
    Pop-Location
}
