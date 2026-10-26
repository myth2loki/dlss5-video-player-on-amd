[CmdletBinding()]
param(
    # Where the assets land. dist\ is ignored by git; these go up to a release
    # with `gh release upload <tag> <folder>\*` or by hand.
    [string]$OutputDirectory,
    # GitHub refuses a release asset above 2 GiB, and the translation model is
    # 2.32 GiB, so it travels as volumes of this size. Everything else fits
    # whole - the largest of them is the 548 MB whisper model.
    [int]$VolumeSizeMB = 1200,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# What a release does not carry: the two engines and the two models. The
# engines are staged with the same whitelists tools\package_amd_release.ps1
# uses for -IncludeSpeechEngine and -IncludeSubtitleEngine, so that unpacking
# these over a player folder leaves the same tree those switches would have
# produced - no CLIs, no benchmarks, no chess demo.
$repositoryRoot = Split-Path -Parent $PSScriptRoot
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $repositoryRoot 'dist\subtitle-engine'
}

$speechRoot = Join-Path $repositoryRoot 'asr'
$translationRoot = Join-Path $repositoryRoot 'translation'

function Assert-SpeechRuntime {
    param([string]$Root, [string]$Label)
    foreach ($required in @('whisper-server.exe', 'whisper.dll', 'ggml.dll', 'ggml-base.dll')) {
        if (-not (Test-Path -LiteralPath (Join-Path $Root $required) -PathType Leaf)) {
            throw "The $Label whisper runtime is missing $required`: $Root"
        }
    }
    $variants = @(Get-ChildItem -LiteralPath $Root -File -Filter 'ggml-cpu-*.dll')
    if ($variants.Count -eq 0) { throw "The $Label whisper runtime carries no ggml-cpu-*.dll: $Root" }
    # The GPU build is the one that has to prove it is one, because the whole
    # point of the second directory is this file.
    if ($Label -eq 'graphics card' -and
        -not (Test-Path -LiteralPath (Join-Path $Root 'ggml-vulkan.dll') -PathType Leaf)) {
        throw "The graphics card whisper runtime has no ggml-vulkan.dll: $Root (run tools\install_speech_gpu_runtime.ps1)."
    }
    Write-Host ("Speech runtime ({0}): {1} - {2} ggml-cpu variants" -f $Label, $Root, $variants.Count)
}

function Get-SpeechRuntimeFiles {
    param([string]$Root)
    # The server and every DLL beside it; the rest of the upstream archive is
    # a CLI the player never starts.
    return @(Get-ChildItem -LiteralPath $Root -File | Where-Object {
        $_.Extension -eq '.dll' -or $_.Name -eq 'whisper-server.exe'
    })
}

function Get-TranslationRuntimeFiles {
    param([string]$Root)
    # -impl.dll belongs to a CLI front end the package does not carry;
    # llama-server's own is the exception, it is the server.
    return @(Get-ChildItem -LiteralPath $Root -File | Where-Object {
        ($_.Extension -eq '.dll' -and ($_.Name -eq 'llama-server-impl.dll' -or $_.Name -notlike '*-impl.dll')) -or
        $_.Name -eq 'llama-server.exe' -or
        $_.Name -eq 'LICENSE-LLVM-OpenMP'
    })
}

Assert-SpeechRuntime -Root (Join-Path $speechRoot 'runtime') -Label 'processor'
Assert-SpeechRuntime -Root (Join-Path $speechRoot 'runtime-gpu') -Label 'graphics card'
if (-not (Test-Path -LiteralPath (Join-Path $translationRoot 'runtime\llama-server.exe') -PathType Leaf)) {
    throw "The translation runtime is missing llama-server.exe: $translationRoot\runtime"
}

# The whisper model is the one thing here that is MIT and redistributable. The
# translation model is Gemma: it may travel with Gemma's terms attached, and
# the notice inside README.txt says where they are.
$speechModel = @(Get-ChildItem -LiteralPath (Join-Path $speechRoot 'models') -File -Filter '*.bin' -ErrorAction SilentlyContinue)
$translationModel = @(Get-ChildItem -LiteralPath (Join-Path $translationRoot 'models') -File -Filter '*.gguf' -ErrorAction SilentlyContinue)
if ($speechModel.Count -eq 0) { Write-Warning 'No asr\models\*.bin: the transcription archive will be skipped.' }
if ($translationModel.Count -eq 0) { Write-Warning 'No translation\models\*.gguf: the translation model will be skipped.' }

if (Test-Path -LiteralPath $OutputDirectory) {
    if (-not $Force) { throw "Output directory already exists: $OutputDirectory (pass -Force to rebuild)." }
    Remove-Item -LiteralPath $OutputDirectory -Recurse -Force
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path

$assets = New-Object Collections.Generic.List[object]

function New-ZipAsset {
    param(
        [string]$Name,
        [object[]]$Files,
        [string]$Level,
        [string]$Contents
    )
    $zipPath = Join-Path $OutputDirectory $Name
    # -Path with the file list rather than the directory: the archive root has
    # to be the files themselves, because it is unpacked straight into an
    # existing asr\runtime or translation\runtime.
    $paths = @($Files | ForEach-Object { $_.FullName })
    Compress-Archive -LiteralPath $paths -DestinationPath $zipPath -CompressionLevel $Level -Force
    $info = Get-Item -LiteralPath $zipPath
    $raw = ($Files | Measure-Object -Property Length -Sum).Sum
    Write-Host ("{0}: {1} files, {2:N0} -> {3:N0} bytes ({4:P0})" -f $Name, $Files.Count, $raw, $info.Length, ($info.Length / [math]::Max(1, $raw)))
    $assets.Add([pscustomobject]@{ Name = $Name; Bytes = $info.Length; Contents = $Contents })
}

function New-SplitAsset {
    param(
        [string]$BaseName,
        [System.IO.FileInfo]$File,
        [int]$VolumeBytes,
        [string]$Contents
    )
    $volumes = [int][math]::Ceiling($File.Length / [double]$VolumeBytes)
    $buffer = New-Object byte[] (4MB)
    $written = 0
    $index = 0
    $in = [IO.File]::OpenRead($File.FullName)
    try {
        while ($true) {
            $index++
            $name = '{0}.part{1}of{2}' -f $BaseName, $index, $volumes
            $outPath = Join-Path $OutputDirectory $name
            $remaining = $VolumeBytes
            $out = [IO.File]::Create($outPath)
            try {
                while ($remaining -gt 0) {
                    $want = [int][math]::Min($buffer.Length, $remaining)
                    $read = $in.Read($buffer, 0, $want)
                    if ($read -le 0) { break }
                    $out.Write($buffer, 0, $read)
                    $remaining -= $read
                }
            }
            finally { $out.Dispose() }
            $partBytes = (Get-Item -LiteralPath $outPath).Length
            if ($partBytes -eq 0) { Remove-Item -LiteralPath $outPath -Force; break }
            Write-Host ("{0}: {1:N0} bytes" -f $name, $partBytes)
            $assets.Add([pscustomobject]@{ Name = $name; Bytes = $partBytes; Contents = $Contents })
            $written += $partBytes
            if ($written -ge $File.Length) { break }
        }
    }
    finally { $in.Dispose() }
    if ($written -ne $File.Length) {
        throw ("Split of {0} lost bytes: wrote {1} of {2}." -f $File.Name, $written, $File.Length)
    }
    return $volumes
}

# --- The two engines -------------------------------------------------------
$zipLevel = 'Optimal'
New-ZipAsset -Name 'dlss5-subtitle-speech-runtime-cpu.zip' `
    -Files (Get-SpeechRuntimeFiles -Root (Join-Path $speechRoot 'runtime')) `
    -Level $zipLevel `
    -Contents 'asr\runtime\ (speech recognition on the processor)'

New-ZipAsset -Name 'dlss5-subtitle-speech-runtime-gpu.zip' `
    -Files (Get-SpeechRuntimeFiles -Root (Join-Path $speechRoot 'runtime-gpu')) `
    -Level $zipLevel `
    -Contents 'asr\runtime-gpu\ (speech recognition on the graphics card)'

New-ZipAsset -Name 'dlss5-subtitle-translation-runtime.zip' `
    -Files (Get-TranslationRuntimeFiles -Root (Join-Path $translationRoot 'runtime')) `
    -Level $zipLevel `
    -Contents 'translation\runtime\ (subtitle translation)'

# --- The whisper model -----------------------------------------------------
# Weights, already entropy-coded: compressing them again costs minutes and buys
# a few per cent, so this one is stored.
foreach ($model in $speechModel) {
    $name = 'dlss5-subtitle-speech-model-{0}.zip' -f [IO.Path]::GetFileNameWithoutExtension($model.Name)
    New-ZipAsset -Name $name -Files @($model) `
        -Level 'NoCompression' -Contents ('asr\models\' + $model.Name)
}

# --- The translation model, in volumes -------------------------------------
$volumeBytes = $VolumeSizeMB * 1MB
$splitNames = New-Object Collections.Generic.List[string]
foreach ($model in $translationModel) {
    $base = 'dlss5-subtitle-translation-model-{0}' -f [IO.Path]::GetFileNameWithoutExtension($model.Name)
    if ($model.Length -le 2GB -and $model.Length -le $volumeBytes) {
        # Under both limits: one asset, no volumes to join.
        $name = $base + '.zip'
        New-ZipAsset -Name $name -Files @($model) `
            -Level 'NoCompression' -Contents ('translation\models\' + $model.Name)
        continue
    }
    $volumes = New-SplitAsset -BaseName $base -File $model -VolumeBytes $volumeBytes `
        -Contents ('translation\models\' + $model.Name)
    for ($i = 1; $i -le $volumes; $i++) { $splitNames.Add(('{0}.part{1}of{2}' -f $base, $i, $volumes)) }
}

# --- The Gemma terms, as a pointer -----------------------------------------
# A release archive travels further than the README in it does, and the
# translation model is Gemma: passing the weights on means passing the terms
# on with them. A copy of the terms would be stale the moment Google edits
# them, so this is a pointer with the restrictions summarised - which is what
# the Gemma Terms ask a distributor to convey.
$gemmaTerms = @'
Gemma Terms of Use - pointer
=============================

translategemma-4b-it-Q4_K_M.gguf is a Gemma model. Using it, copying it and
passing it on are governed by the Gemma Terms of Use:

    https://ai.google.dev/gemma/terms

This file is a pointer and not a copy of those terms: Google updates them, and
a stale copy inside an archive would be worse than none. Read the current text
before you pass the model on, and pass it on with the model.

In short - and this is not legal advice, the terms are:
  - you may use, copy, modify, distribute and commercialise the model;
  - if you distribute it, you pass these terms and the use restrictions on
    with it, and you do not attribute your outputs to Google;
  - the Prohibited Use Policy applies, and Google gives no warranty;
  - Google may update the terms, and the current version is the one that binds.

The recogniser is not Gemma and is not under those terms: whisper.cpp and its
ggml weights are MIT, libopenblas.dll is BSD 3-Clause, SDL2.dll is zlib, and
the llama.cpp server and its LLVM OpenMP runtime travel under MIT and the
LLVM licence respectively.
'@
$gemmaPath = Join-Path $OutputDirectory 'GEMMA_TERMS.txt'
[IO.File]::WriteAllLines($gemmaPath, ($gemmaTerms -split "`r?`n"), (New-Object Text.UTF8Encoding $false))
$assets.Add([pscustomobject]@{
    Name = 'GEMMA_TERMS.txt'
    Bytes = (Get-Item -LiteralPath $gemmaPath).Length
    Contents = '(the translation model''s licence, as a pointer)'
})

# --- What to do with them --------------------------------------------------
$readme = New-Object Collections.Generic.List[string]
$readme.Add('DLSS 5 Video Player - subtitle engine add-ons')
$readme.Add('=============================================')
$readme.Add('')
$readme.Add('What a release does not carry: the speech recognition engine, the subtitle')
$readme.Add('translation engine, and the models they need. Unpack every archive with the')
$readme.Add('player shut down, into the folder that holds DLSSVideoPlayer.exe, so that the')
$readme.Add('tree ends up looking like this:')
$readme.Add('')
$readme.Add('  DLSSVideoPlayer.exe')
$readme.Add('  asr\')
$readme.Add('    runtime\        whisper-server.exe and the DLLs beside it (processor)')
$readme.Add('    runtime-gpu\    the same, plus ggml-vulkan.dll (graphics card)')
$readme.Add('    models\         ggml-large-v3-turbo-q5_0.bin')
$readme.Add('  translation\')
$readme.Add('    runtime\        llama-server.exe and the DLLs it loads')
$readme.Add('    models\         translategemma-4b-it-Q4_K_M.gguf')
$readme.Add('')
$readme.Add('Each archive holds the contents of one directory, not the directory itself.')
$readme.Add('The Subtitles menu greys its entries out until the engine AND the model are')
$readme.Add('both in place; nothing is started before that.')
$readme.Add('')
$readme.Add('Contents')
$readme.Add('--------')
foreach ($asset in $assets) {
    $readme.Add(('  {0,-70} {1,12:N0}  ->  {2}' -f $asset.Name, $asset.Bytes, $asset.Contents))
}
$readme.Add('')
if ($splitNames.Count -gt 0) {
    $readme.Add('The translation model is larger than the 2 GiB a release asset may be, so it')
    $readme.Add('travels as volumes. Join them first, in order, then move the result into')
    $readme.Add('translation\models\:')
    $readme.Add('')
    $joinedName = $translationModel[0].Name
    $readme.Add('  Windows:  copy /b ' + (($splitNames | ForEach-Object { $_ }) -join '+') + ' ' + $joinedName)
    $readme.Add('  Linux:    cat ' + (($splitNames | ForEach-Object { $_ }) -join ' ') + ' > ' + $joinedName)
    $readme.Add('')
    $readme.Add('Check the result against the SHA-256 of the whole file in SHA256SUMS.txt:')
    $readme.Add('a join that went wrong still produces a file of the right size.')
    $readme.Add('')
}
$readme.Add('Where the hardware is chosen')
$readme.Add('----------------------------')
$readme.Add('Subtitles > Transcribe on the graphics card / on the processor, or [Speech]')
$readme.Add('Gpu= in DLSSVideoPlayer.ini (1 = the graphics card, the default). Without')
$readme.Add('asr\runtime-gpu the player recognises on the processor and says so.')
$readme.Add('')
$readme.Add('Licences')
$readme.Add('--------')
$readme.Add('whisper.cpp, llama.cpp and ggml are MIT, and the whisper model is MIT too.')
$readme.Add('Two DLLs beside whisper-server are not: libopenblas.dll is OpenBLAS (BSD')
$readme.Add('3-Clause) and SDL2.dll is zlib. The llama.cpp server links the LLVM OpenMP')
$readme.Add('runtime, whose licence travels with it as LICENSE-LLVM-OpenMP.')
$readme.Add('')
$readme.Add('The translation model is a Gemma model and is distributed under the Gemma')
$readme.Add('Terms of Use. GEMMA_TERMS.txt beside this file points at the current text and')
$readme.Add('summarises what they ask of you; passing the model on means passing them on')
$readme.Add('with it.')
$readme.Add('')
$readmePath = Join-Path $OutputDirectory 'README.txt'
[IO.File]::WriteAllLines($readmePath, $readme, (New-Object Text.UTF8Encoding $false))

# --- Checksums -------------------------------------------------------------
$sums = New-Object Collections.Generic.List[string]
foreach ($asset in $assets) {
    $hash = (Get-FileHash -LiteralPath (Join-Path $OutputDirectory $asset.Name) -Algorithm SHA256).Hash.ToLowerInvariant()
    $sums.Add(('{0}  {1}' -f $hash, $asset.Name))
}
# The whole, joined file too, so a bad join is caught before it reaches the player.
foreach ($model in $translationModel) {
    $hash = (Get-FileHash -LiteralPath $model.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    $sums.Add(('{0}  {1} (after joining the volumes)' -f $hash, $model.Name))
}
$sumsPath = Join-Path $OutputDirectory 'SHA256SUMS.txt'
[IO.File]::WriteAllLines($sumsPath, $sums, (New-Object Text.UTF8Encoding $false))

$total = (Get-ChildItem -LiteralPath $OutputDirectory -File | Measure-Object -Property Length -Sum)
Write-Host ''
Write-Host ("{0} assets, {1:N0} bytes in {2}" -f $total.Count, $total.Sum, $OutputDirectory)
Write-Host 'Upload with: gh release upload <tag> "<folder>\*"'
