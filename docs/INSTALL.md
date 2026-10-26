# Installing DLSS 5 Video Player on an AMD card

Windows 10 or 11, 64-bit, on an RDNA 4 Radeon (`gfx1200` / `gfx1201`, measured
on an RX 9070 XT). Nothing is installed in the usual sense: the player is a
folder of files, and it looks for the rest at startup.

Everything below ends up in **one folder**. Pick a path that is not inside
`Program Files` — the player writes its render cache next to itself:

```
D:\DLSSVideoPlayer\
```

---

## 1. The player

Download `DLSSVideoPlayer-v0.29.2-amd-win64.zip` (534 MiB) and unpack it so that
`DLSSVideoPlayer.exe` sits at the top of that folder. The archive has a single
`DLSSVideoPlayer-v0.29.2-amd-win64\` folder inside it, so you can either unpack
into `D:\` and rename the result, or unpack into `D:\DLSSVideoPlayer\` and move
the contents up one level.

It already contains, and you need nothing else for these:

| | |
| --- | --- |
| `DLSSVideoPlayer.exe` | the player |
| `neural-runtime\NeuralWorker.exe` | the process that runs the model |
| `amd_fidelityfx_loader_dx12.dll`, `…_upscaler_dx12.dll`, `…_framegeneration_dx12.dll` | FSR 4 / FSR 1 / CAS upscaling and frame generation |
| `ffmpeg.exe`, `ffprobe.exe` | decoding, probing, encoding. The player runs **only** these, never one from `PATH` — do not swap them |
| `amd-neural-runtime\` | the AMD neural runtime, 844 MiB unpacked — see below |

Check it against the release digest if you like:

```
certutil -hashfile DLSSVideoPlayer-v0.29.2-amd-win64.zip SHA256
```

```
CFCDC81CD9085E2F6BB662C99B07EB789D5514DC02DA300EB236D5EE040E1671
```

---

## 2. The AMD neural runtime — lmxxf's zip

**You can skip this section if you downloaded the player archive above**: it
already carries `amd-neural-runtime\`. Read on if you are repairing a runtime,
moving to a different one, or you got the player from somewhere that does not
carry it.

The runtime is `amd-neural-runtime/`: `LmxxfNrRuntime.dll`, 80 HSACO modules for
`gfx1200`/`gfx1201`, the tiled network weights, the runtime's own shader sources
and `DLSS5-AMD/custom-config.txt`. It is published as a separate asset on the
`lmxxf-<line>` release — **the tag carries the version, not the file name**, so
a pack for another line has the same name and a different tag.

```powershell
$tag    = 'lmxxf-0.44'
$zip    = 'amd-neural-runtime.zip'
$sha256 = '8965AC8919106EC59E24B21F44F31E534AC026C0895FE7FF4E51AADAC9131C8F'
Invoke-WebRequest "https://github.com/myth2loki/dlss5-video-player-on-amd/releases/download/$tag/$zip" -OutFile $zip
if ((Get-FileHash $zip -Algorithm SHA256).Hash -ne $sha256) { throw 'digest mismatch' }
tar -xf $zip -C 'D:\DLSSVideoPlayer'
```

- The asset is **341,283,097 bytes**, 743 entries, 844 MiB unpacked.
- **The zip carries the `amd-neural-runtime/` folder itself.** Unpack it *into*
  the player folder — do not unpack it *into* `amd-neural-runtime\`, which would
  give you `amd-neural-runtime\amd-neural-runtime\` and a player that finds no
  runtime at all.
- Use Windows' own `tar` (bsdtar, ships with Windows 10 1803+) or Explorer's
  "Extract All". **Git Bash's GNU tar cannot read zip files**, so `tar -xf` from
  a Git Bash prompt fails with "This does not look like a tar archive".
- The digest is published in upper case and `Get-FileHash` prints it that way
  too, so the comparison above is a plain `-ne` with no normalisation.

Afterwards it must look exactly like this — `LmxxfNrRuntime.dll` **inside**
`amd-neural-runtime\`, next to `DLSSVideoPlayer.exe`:

```
D:\DLSSVideoPlayer\
  DLSSVideoPlayer.exe
  amd-neural-runtime\
    LmxxfNrRuntime.dll
    lmxxf-modules\gfx1200\SHA256SUMS + 40 HSACOs
    lmxxf-modules\gfx1201\SHA256SUMS + 40 HSACOs
    native-game-tiled-assets\
    shaders\
    DLSS5-AMD\custom-config.txt
```

The player looks for that folder, in this order: the environment variable
`DLSS5_AMD_RUNTIME_DIR`, then `amd-neural-runtime` beside the exe, then one and
two levels above the exe. So a copy in the parent of the player folder works
too — but one folder is simpler.

**Verify it before launching.** The runtime checks itself against its own
`SHA256SUMS` at startup and refuses a tree that does not match; the log
(`DLSSVideoPlayer.log`, beside the exe) says either
`AMD neural runtime identified: digest … over 707 files` or names the file that
failed. A truncated download is the usual cause.

---

## 3. Optional: subtitles from the film's own audio

The player can transcribe a film that has no subtitle track, translate the
transcript, and draw both lines over the render. The engines and models are
**not** in the player archive — they are separate assets, because the models
are 3 GiB and most people never turn this on.

| Download | Unpack into |
| --- | --- |
| `dlss5-subtitle-speech-runtime-cpu.zip` | `D:\DLSSVideoPlayer\asr\runtime\` |
| `dlss5-subtitle-speech-runtime-gpu.zip` | `D:\DLSSVideoPlayer\asr\runtime-gpu\` |
| `dlss5-subtitle-speech-model-ggml-large-v3-turbo-q5_0.zip` | `D:\DLSSVideoPlayer\asr\models\` |
| `dlss5-subtitle-translation-runtime.zip` | `D:\DLSSVideoPlayer\translation\runtime\` |
| `…translategemma-4b-it-Q4_K_M.part1of5` … `part5of5` | join first, then `D:\DLSSVideoPlayer\translation\models\` |

The `asr\` and `translation\` folders do not exist until you create them; the
archives hold the *contents* of those directories, so unpack them into the
folders named above.

The translation model is 2.32 GiB, above the 2 GiB a release asset may be, so
it arrives as five 512 MB volumes. Join them in the order in the file names:

```
copy /b dlss5-subtitle-translation-model-translategemma-4b-it-Q4_K_M.part1of5+dlss5-subtitle-translation-model-translategemma-4b-it-Q4_K_M.part2of5+dlss5-subtitle-translation-model-translategemma-4b-it-Q4_K_M.part3of5+dlss5-subtitle-translation-model-translategemma-4b-it-Q4_K_M.part4of5+dlss5-subtitle-translation-model-translategemma-4b-it-Q4_K_M.part5of5 translategemma-4b-it-Q4_K_M.gguf
```

Then check it — a bad join still produces a file of the right size:

```
certutil -hashfile translategemma-4b-it-Q4_K_M.gguf SHA256
```

```
7f7357c14abd9da4eb200b38b05da502cd6e10d7e1d403fbc9f78c19f3209b72
```

The speech engine exists as two builds and the Subtitles menu picks between
them: `asr\runtime` computes on the processor, `asr\runtime-gpu` is the same
build with `ggml-vulkan.dll` beside it and computes on the graphics card. The
difference is not small — 60 s of audio costs 298 s of CPU time on 12 cores
against 2.63 s on a 9070 XT, same transcript. Install `runtime-gpu` and the
graphics card is used by default; `Subtitles > Transcribe on the graphics card /
on the processor` switches, and `[Speech] Gpu=` in the ini is the same choice.

---

## 4. First run

1. Start `DLSSVideoPlayer.exe`. It is not code-signed, so Windows may show
   "Windows protected your PC": **More info > Run anyway**. If you did not
   build it yourself, know where the binary came from.
2. **File > Open** (`Ctrl+O`), a public YouTube link (`Ctrl+L`), or
   **File > Game trailers** for something to test with.
3. Press **`D`**. It buffers for a few seconds, then plays the render, filling
   the rest of the film in behind the playhead.
4. The bar under the picture switches between the neural view, the original,
   Split, Wipe, Difference, Side by side and 2 × 2. **`C`** steps through them,
   **`X`** swaps sides.
5. **DLSS > Convert & export** writes the rendered video to a file.

Settings persist in `DLSSVideoPlayer.ini` beside the exe.

---

## When something is wrong

| Symptom | Cause |
| --- | --- |
| "FFmpeg is unavailable", nothing opens | `ffmpeg.exe` / `ffprobe.exe` missing from the player folder. The player never looks in `PATH`, by design: what it writes goes into your cache and your exports |
| Neural rendering greyed out, no `D` | `amd-neural-runtime\LmxxfNrRuntime.dll` not where the player looks (§2), or the tree failed its own digest check — see the log |
| "Neural Rendering (experimental)" then a fallback to the original | the runtime loaded but the GPU was rejected: the card is not `gfx1200`/`gfx1201` |
| Upscaling greyed out | one of the three `amd_fidelityfx_*.dll` is missing beside the exe |
| **Subtitles from speech** greyed out | `asr\runtime\whisper-server.exe` **or** a `*.bin` model in `asr\models\` is missing — both are required, and neither starts until both are there |
| Subtitles appear but stay in English | the translation model (`translation\models\*.gguf`) or `translation\runtime\llama-server.exe` is missing |
| Recognising is slow and the fans spin up | `asr\runtime-gpu` is not installed, so it is using the processor. The Subtitles menu says which is in force |

The log beside the exe (`DLSSVideoPlayer.log`) names every one of these at
startup, including which runtime directory and which speech build it resolved.

---

## Where these files come from

| | |
| --- | --- |
| Player, FFX DLLs, ffmpeg | this fork's release archive |
| `amd-neural-runtime/` | the `lmxxf-<line>` release, built from [lmxxf's porting project](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting) |
| whisper.cpp runtime | upstream whisper.cpp, MIT; the Vulkan backend is the one from the llama.cpp runtime beside it |
| llama.cpp runtime | upstream llama.cpp, MIT |
| `ggml-large-v3-turbo-q5_0.bin` | OpenAI Whisper weights, MIT |
| `translategemma-4b-it-Q4_K_M.gguf` | a Gemma model, under the Gemma Terms of Use — see `GEMMA_TERMS.txt` |
