[CmdletBinding()]
param(
    # Where `runtime` and `models` live: the ASR tree beside the player, one
    # level up in the build tree, or wherever the viewer keeps it.
    [string]$SpeechRoot = '',
    # The Vulkan backend to install beside the copied tools. Left empty it is
    # taken from the subtitle translation engine, which ships exactly this file:
    # both are ggml, and whisper loads whichever backend DLLs sit next to it.
    [string]$VulkanBackend = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repositoryRoot = Split-Path -Parent $PSScriptRoot
if (-not $SpeechRoot) {
    # HelperRoot() tries beside the player first, then one level up.
    $candidates = @(
        (Join-Path $repositoryRoot 'run-fg\asr'),
        (Join-Path $repositoryRoot 'asr')
    )
    foreach ($candidate in $candidates) {
        if (Test-Path (Join-Path $candidate 'runtime\whisper-server.exe')) { $SpeechRoot = $candidate; break }
    }
}
if (-not $SpeechRoot -or -not (Test-Path (Join-Path $SpeechRoot 'runtime\whisper-server.exe'))) {
    Write-Error "No CPU whisper runtime found (looked for runtime\whisper-server.exe). Pass -SpeechRoot."
}

if (-not $VulkanBackend) {
    foreach ($candidate in @(
        (Join-Path $repositoryRoot 'translation\runtime\ggml-vulkan.dll'),
        (Join-Path $repositoryRoot 'run-fg\translation\runtime\ggml-vulkan.dll')
    )) {
        if (Test-Path $candidate) { $VulkanBackend = $candidate; break }
    }
}
if (-not $VulkanBackend -or -not (Test-Path $VulkanBackend)) {
    Write-Error "No Vulkan backend found. Download whisper.cpp's Vulkan Windows build and pass -VulkanBackend <path>\ggml-vulkan.dll."
}

$source = Join-Path $SpeechRoot 'runtime'
$target = Join-Path $SpeechRoot 'runtime-gpu'
Write-Host ("Speech root : {0}" -f $SpeechRoot)
Write-Host ("CPU build   : {0}" -f $source)
Write-Host ("GPU build   : {0}" -f $target)

New-Item -ItemType Directory -Path $target -Force | Out-Null
Get-ChildItem -LiteralPath $source -File | Copy-Item -Destination $target -Force
Copy-Item -LiteralPath $VulkanBackend -Destination (Join-Path $target 'ggml-vulkan.dll') -Force

$copied = Get-ChildItem -LiteralPath $target -File
Write-Host ("Installed {0} files, {1:N1} MB." -f $copied.Count, (($copied | Measure-Object -Property Length -Sum).Sum / 1MB))
Write-Host "Whichever the menu says now, both builds are installed and either can be chosen."
Write-Host "Check it: .\runtime-gpu\whisper-cli.exe -m ..\models\<model>.bin -f <60s.wav> -t 8"
Write-Host "expects 'load_backend: loaded Vulkan backend' and 'using Vulkan0 backend'."
