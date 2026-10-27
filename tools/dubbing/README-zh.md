# tools/dubbing — 把一部片子配成另一种语言

六个阶段，每个阶段是一个独立进程，只通过磁盘上的文件交接。任何一段出问题都能
用 `--from` 从断点重跑，`--keep` 留下中间产物。

```
python dub.py movie.mov                       # 全流程
python dub.py movie.mov --from mix --keep     # 只重跑混音 + 封装
```

| 阶段 | 脚本 | 产出 |
|---|---|---|
| prepare | `step0_prepare.py` | 44.1k 音轨、`transcript.json`、`stems_out/vocals.wav` + `no_vocals.wav` |
| align | `step1_align.py` | `lines.json`：每句的起止 + 谁在说（ECAPA 嵌入 + 基频单边否决） |
| refs | `step11_refs_auto.py` | 每个说话人一段参考音频（Qwen3-TTS 的上限 15 s） |
| translate | `step2t_translate.py` | `zh_final.json` 草稿译文（过术语表提示） |
| fit | `step12_fit_auto.py` | `fit_auto.json` + 逐句合成 `dub_auto/zhNN.wav` + `subtitles.ass` |
| mix | `step13_mix_auto.py` | `dubbed_full_zh2.wav/mp3`、`ab_lines_zh2.*`、`mix_auto.json` |
| mux | `dub.py` 自己 | 视频 + 配音 + 字幕轨（`mov_text` + `language=chi`） |

`dub.py` 只负责编排和封装；真峰限制、电平归一这些「声音好不好听」的判断在
`step13` 里，字幕的文本在 `step12` 里由同一批行生成（`subtitles.py`），
所以「听到的」和「看到的」结构上不可能分叉。

## 依赖

仓库自带的：`run-fg/ffmpeg.exe`、`run-fg/ffprobe.exe`、`asr/runtime-gpu/whisper-cli.exe`
与 `asr/models/ggml-large-v3-turbo-q5_0.bin`（见 `tools/install_speech_gpu_runtime.ps1`）、
`translation/models/translategemma-4b-it-Q4_K_M.gguf`（见 `tools/package_subtitle_engine.ps1`）。

必须在 `DUB_ENGINE` 指定的引擎目录里的（权重不入库）：

- `stems/stems-split.exe` + `stems/models/htdemucs-42M-v1.0-F32.gguf` — 人声/伴奏分离
- `bin/**/llama-tts.exe` + `models/Qwen3-TTS-12Hz-1.7B-Base-Q4_K_M.gguf` + `mmproj-*.gguf` — 合成
- `models/ecapa_voxceleb.onnx` — 说话人嵌入
- `pace_model.json` — 该音色的语速模型（是**引擎**的属性，不是片子的，所以随引擎走）

Python：`numpy`、`soundfile`、`onnxruntime`、`parselmouth`；`probe_metrics.py` 另外要
`librosa`/`torch`/`speechbrain`（只做离线评估，不在流水线里）。用装好这些的 venv 跑，
系统 python 一般缺 `soundfile`。

## 环境变量

| 变量 | 默认 | 说明 |
|---|---|---|
| `DUB_ROOT` | 作业目录 | 所有相对路径的根 |
| `DUB_INPUT` / `DUB_OUTPUT` | — | 源片 / 成品 |
| `DUB_ENGINE` | 本目录 | 引擎目录（权重、pace 模型、termer） |
| `DUB_GLOSSARY` | `$DUB_ENGINE/glossary.json` | 翻译与 fit 共用的术语表 |
| `DUB_FFMPEG` / `DUB_FFPROBE` | `run-fg/*` | |
| `DUB_WHISPER` / `DUB_WHISPER_MODEL` | `asr/runtime-gpu/*` | |
| `DUB_TTS_BIN` / `DUB_TTS_MODEL` / `DUB_TTS_MMPROJ` | 引擎目录 | |
| `DUB_TR_MODEL` | `translation/models/*` | 翻译用 gguf |
| `DUB_LANG` | `en` | 源语言 |

## 不在这里的东西

- **模型权重**（≥3 GB）和 `stems/`、`bin/`：按上面的清单在引擎目录里自备。
- **早期的实验脚本**：`step2*`（Granite 直译的各种试法）、`step3_tts`/`step3b_fix`、
  `step4_mix`、`step5_hybrid`、`step6_refs`/`step7_ab_refs`/`step8_xtts_basis`/`step9_ab_compare`
  （XTTS 选型与 A/B）、`step10_calibrate`/`step10b_fit`、`step14_stems_parity`。
  它们被 `step12`/`step13` 取代，留在作业目录里只作历史，不要接进流程。

## 验收

`tools/dub-metrics/` 下的脚本量成品，不量日志：

```
python tools/dub-metrics/audit_subtitles.py          # 字幕轨（阅读速度、超宽、间隔）
python tools/dub-metrics/audit_translation.py        # 数字/否定/疑问/专名四道闸
python tools/dub-metrics/measure_delivered_levels.py # 电平离散、原声残留
python tools/dub-metrics/measure_timing_headroom.py  # 时间余量
```

门槛与基线写在 `docs/SUBTITLE-AUDIO-METRICS-zh.md`；拟合规则写在
`docs/DUBBING-FIT-RULES-zh.md`。
