# 转换功能的可用性指标（Convert Video 验收口径）

_2026-10-27，第十节 2026-11-11 补入画质刻度。分三层：**P0 正确性**（不过就是坏的）、**P1 性能**（决定能不能当后台任务用）、**P2 体验**（进度、取消、报错）。
每一项都写了取法——能一条命令取到的就不写"看看效果如何"。_

## 0. 先说结论：现在能测 7 项，缺 2 项仪器

| 层 | 已可量化 | 只能人工 | 完全没有 |
|---|---|---|---|
| P0 正确性 | 时长/帧数/音轨位相同/流数量/解码完整性/尺寸/尾帧 | — | 色彩元数据透传、源文件未被改动（需补测） |
| P1 性能 | 端到端耗时与实时倍率、每帧成本、准备开销、首帧延迟 | 显存峰值、GPU 占空比（无内建采集） | 长片（>2 h）稳定性 |
| P2 体验 | 进度单调性、取消后无残留 | 取消响应延迟、ETA 偏差 | — |

`ConvertTests`（`tests/ConvertTests.cpp`，441 行）**只测 CLI 契约**——`planner_test` / `render_contract_test` / `front_end_test`，
从不碰产物。所以下表的每一项都得靠外部命令，或自己加。

---

## 1. P0 正确性：一条命令一项

基线样本：`cv_long.mp4`（60.000 s / 1440 帧 / 640×360 / h264 + AAC）→ `cv_long-dlss.mp4`（单 pass 神经渲染）。

| # | 指标 | 取法 | 门槛 | 本机实测 |
|---|---|---|---|---|
| 1 | 时长偏差 | `ffprobe -show_entries format=duration` 两边比 | ≤ 1 帧周期（24 fps → 42 ms） | 60.000 → 59.999（Δ **−1 ms**） |
| 2 | 帧数守恒 | `nb_frames`；插帧时 = 源 × 倍率 | 等于 `round(duration×fps)`±1 | 1440 = 1440 |
| 3 | 音轨逐字节相同 | 包 md5 与解码 md5 都比（只比一个会漏） | 完全一致 | 包 `b65a9f…` / 解码 `773f1b…` **两边全同** |
| 4 | 流数量与类型不变 | `-show_entries stream=index,codec_type` | 视频 1、音频 1（源有几个就要几个） | 2 流，未增减 |
| 5 | 全片零解码错误 | `ffmpeg -v error -i out -f null -` | 输出为空 | **空** |
| 6 | 尾帧存在 | `ffmpeg -sseof -0.2 -map 0:v -f framemd5 -` | 至少 1 帧 | 3 帧 |
| 7 | 尺寸符合请求 | `width/height` vs `--height`/源 | 与请求一致 | 640×360（未缩放） |
| 8 | **色彩元数据透传** | `-show_entries stream=color_primaries,color_transfer,color_space,color_range` | SDR `bt709`/`tv`；HDR 必须 `smpte2084`（或 `arib-std-b67`）+ `bt2020nc` | 2026-11-11 起：烧录重编码按渲染视频自己的标签写（四件套经 `setparams`），10 bit 跟随 `pix_fmt`；渲染无标签才兜底 `bt709/tv`。`PolicyTests` 两条断言钉住。**仍缺 HDR 素材实测** |
| 9 | 源文件未被改动 | 转换前后 `md5` + mtime | 完全一致 | mtime 未变（14:58 至今）；**md5 未做前后对比，属待补** |
| 10 | 起播时间戳 = 0 | `start_time` | 0.000 | 0.000 / 0.000 |

第 8 项曾经是最可能踩雷的一条：烧录分支只写 `-pix_fmt yuv420p`，一个色彩标签都不给，
10 bit HDR 源会被标成 bt709 播成灰片子。2026-11-11 补上了，改了三处：

- `ListMediaStreams` 多读 `pix_fmt` 与 `color_primaries/transfer/space/range`，`unknown`/`reserved`
  当"没声明"（与"声明了 bt709"是两件事，不能混）；
- `MuxStageExport` 把渲染视频自己的这些值填进 `StageExportMuxRequest`（`-c:v copy` 本来就带着标签走，
  只有烧字幕的重编码需要）；
- 烧录分支按这些值写 `setparams` 四件套，10 bit 渲染改用 `yuv420p10le` + `-profile:v high10`。

`setparams` 而不是输出选项，是因为这台 FFmpeg 的 `-color_primaries`/`-color_trc` 会被静默丢弃
（渲染路径早就量到这一点，它同样走 `setparams`）；实测 `ffprobe` 读回四件套齐全。

判据：产物 `ffprobe` 的四个字段与渲染视频一致；渲染没声明时才允许出现 `bt709/tv`。
**仍然没有 HDR 素材**，所以"HDR 源端到端"是未验证项，只能保证"标签不再被改写"。

现成命令（复制即用）：

```bash
FF=F:/dlss5-video/dlss5-video-amd/run-fg/ffmpeg.exe
FP=F:/dlss5-video/dlss5-video-amd/run-fg/ffprobe.exe

$FP -v error -show_entries format=duration -show_entries stream=index,codec_type,width,height,
r_frame_rate,nb_frames,start_time,color_primaries,color_transfer,color_range -of default=nw=1 OUT
$FF -v error -i OUT -f null -                                   # 5：有输出就是错误
$FF -v error -i SRC -map 0:a -c copy -f md5 -                   # 3：与产物的值比
$FF -v error -i OUT -map 0:a -c copy -f md5 -
$FF -sseof -0.2 -i OUT -map 0:v -f framemd5 -                   # 6
```

## 2. P1 性能：日志里两行时间差就是实时倍率

转换开始与结束各有一行日志（`run-fg/DLSSVideoPlayer.log`）：

```
Convert video starting: output=… upscale=0 neural=1 framegen=1 container=mp4 passes=2 size=640x360 fps=48 …
Stage export wrote …
```

| 指标 | 定义 | 门槛 | 本机实测（RX 9070 XT，60 s / 640×360） |
|---|---|---|---|
| **实时倍率** | 片长 ÷ 墙钟耗时 | **≥1.0× 才能当后台任务**；≥2× 体验良好 | 单 pass **4.03×**（14.91 s）、两 pass **3.19×**（18.84 s） |
| 每帧成本 | pass 耗时 ÷ 帧数 | 与分辨率线性相关则有意义 | neural **9.5 ms/帧**、插帧 **2.6 ms/帧** |
| pass 分解 | 各 pass 占墙钟比例 | 单 pass 应占 >90% | neural 73% / 插帧 20% / 封装 3% |
| 准备开销 | 点确定到第一帧之间 | <3 s（否则要放进进度文案） | runtime 摘要 **2.55 s 冷 / 74 ms 热**（707 个文件） |
| 首帧延迟 | `Neural cold start:` 的 `request=` | <0.5 s | **2.234 s**（range-selection 路径） |
| 显存峰值 | 外部工具 | < 卡容量 80% | 无采集 |
| 长片稳定性 | 2 h 片全程 | 不掉速、不 OOM | 未测 |

`GPU 占空比` 不要用播放时的曲线衡量转换：播放是分块渲染等低水位，占空比恒为 `1/r`（r≈1.8），
转换是连续跑满，两者不可比。

**同一份参数重跑，倍率应当稳定在 ±5%**；漂出去说明命中了缓存或换了引擎（本机曾出现 2.55 s 与 74 ms 的摘要耗时，
不是 bug，是 runtime 指纹缓存）。

## 3. P2 体验：进度、取消、报错

| 指标 | 取法 | 门槛 | 现状 |
|---|---|---|---|
| 进度单调性 | 采样进度条填充宽度（**按"连续 ≥6 行等宽实心横带"识别，不能数青色像素**——ClearType 会在字形边缘留彩边） | 倒退次数 **0** | 39 次采样，最大回退 **0 px**；pass 接缝从 78.7% 连续通过 |
| 分母口径 | 读 `StageExportUpdate::overallCompleted/overallTotal` | 整片，不是单 pass | 已改（commit `bec2b33`） |
| 封装相位不消失 | `writing` 标记 | 面板保留、条满格、时钟继续 | 已改 |
| 取消可用 | 模态面板上的 Cancel | 点下即停 | 已验证 |
| 取消后无残留 | 目录里不应出现 `<stem>-dlss.*` | 0 个残留 | 已验证（状态栏 `Conversion cancelled.`） |
| 取消响应延迟 | 点击到进程退出 | <2 s | **未量化** |
| 转换时暂停、结束恢复 | 播放状态 | 开始时停、结束/取消后恢复 | 已验证 |
| ETA 偏差 | 面板 ETA vs 实际剩余 | <20% | 未量化（`elapsed/done×(total−done)`，done>4 且 elapsed>2 s 才显示） |
| 失败可读 | 缺 runtime / 磁盘满 / 目录只读 | 一句话说清怎么修，不留半成品 | 部分：缺 runtime 有一次 notice |
| 绝不覆盖 | 同名再转一次 | 生成 `-1`/`-2` | 已验证（`cv_long-dlss-1.mp4`） |

## 4. 兼容性矩阵（每格都要跑，不能抽查）

| 维度 | 取值 | 现状 |
|---|---|---|
| 分辨率 | 640×360 / 720p / 1080p / 4K / 竖屏 / **奇数宽高** | 只验过 640×360、1080p 输入上限 |
| 帧率 | 23.976 / 24 / 25 / 30 / 60 / **VFR** | 只验过 24（插帧后 48） |
| 容器 | mp4 / mkv / mov / avi | 只验过 mp4 入 mp4 |
| 音频 | AAC / AC3 / DTS / **无音轨** | 只验过 AAC（位相同已证） |
| 位深 | 8 bit / 10 bit | 10 bit 编码参数存在，未测 |
| 动态范围 | SDR / HDR10 / HLG | **未测**，且 color tag 不写（见 P0-8） |
| 多轨 | 多音轨 / 字幕轨 / 章节 | 未测（`-map` 策略决定是否会丢） |
| 时长 | 5 s / 60 s / 12 min / 2 h | 验过 5 s、60 s |
| 路径 | 中文 / 空格 / 网络路径 | 中文路径验过（配音成品） |

## 5. 建议补的仪器（按性价比排序）

1. **产出校验命令**：`DLSSVideoPlayer.exe --verify-output a.mp4 [-s source]`，把 §1 的 10 项跑一遍并退出非零码。
   现在这些判断散在临时 Python 脚本里（`F:/tmp/verify_*.py`），换个人就重写一遍。
2. **每 pass 耗时进 `StageExportUpdate`**：想在发布版里长期跟踪性能，就不能靠在驱动线程里临时打日志
   （上一轮调试用的 `Stage export progress:` 已经删了，那批 9.5 / 2.6 ms 的数字现在无法复现）。
3. **显存与 GPU 采集**：转换期间峰值显存是判断"4K 会不会炸"的唯一手段，目前靠第三方工具肉眼。
4. ~~**色彩元数据透传**~~：已完成（2026-11-11，见 §1 第 8 项），仍欠 HDR 素材实测。

## 6. 基线快照（2026-10-27，用于回归对比）

| 项目 | 单 pass（neural） | 两 pass（neural + 插帧） |
|---|---|---|
| 源 | 60.000 s / 1440 帧 / 640×360 / 24 fps | 同左 |
| 墙钟 | 14.91 s | 18.84 s |
| 实时倍率 | 4.03× | 3.19× |
| 输出 | HEVC 640×360 24 fps，2.26 Mbps | 日志记 `fps=48 passes=2`；该产物在 15:40 被单 pass 覆盖，未复测 |
| 体积 | 17.7 MB（源 3.5 MB，367 kbps → 2.26 Mbps） | — |
| 音轨 | 位相同（96960 bps 未重编） | 位相同 |
| 时长偏差 | −1 ms | 未复测（产物已覆盖） |

体积涨 5 倍不是缺陷：源是 366 kbps 的糊片，神经渲染重编在 `--encode` 默认档，码率由质量决定，不由源码率决定。
**要判断"有没有变糊"，得看分带 PSNR，不是看码率**——2026-11-11 起有尺子了，见 §8；
当时"这一项没有基准"是该文档最大的缺口，现在补上，但只覆盖 1080p 的 SDR 素材。

## 7. 字幕轨与烧录（2026-10-28 接入，验收口径）

Convert Video 的 Subtitles 行三态现在都是真的：None 照抄源；A track（软轨）把转写+翻译的
ASS 作为自己的流挂进容器（MKV 保样式、MP4 转 mov_text，`language=chi`——mov 封装丢 T 码）；
Burned into the picture 在 mux 里用 `ass` 滤镜把字画进每一帧并重编码
（`libx264 -preset medium -crf 18`，与缓存导出的软件重编码同一配方）。配音（TTS）仍未接。

**仍被拒绝的组合**（弹窗，不静默跳过）：范围+生成轨/烧录（文本是全片时间轴）；
烧录但源无文本轨且未开翻译（没字可烧）；翻译但引擎未装。

验收取法：

| # | 指标 | 取法 | 门槛 |
|---|---|---|---|
| 1 | 软轨存在与语言 | `ffprobe -show_streams` 数 `codec_type=subtitle`，读 `language` | 源自带轨 +1，tag 非空 |
| 2 | 烧录无残留流 | 同上，burn 产物 | 不新增字幕流 |
| 3 | 字真的在画面里 | 抽台词区间中点帧人查 | 文字可读、无方框（字体 fallback 失败会出 □） |
| 4 | 命令行契约 | `PolicyTests::stage_export_mux_generated_subtitle_is_an_addition_test` | 全过（含空轨逐字节不变断言） |

已知坑：冒烟测试**不能**用 `-ss` 截源再烧——输入时间轴被重置到 0，ASS 的 cue 按
全片时间轴排，片段里永远等不到字。产品里范围导出走 `-itsoffset` 保留时间轴（且
范围+文本直接拒绝），不受此影响；手工验证要抽帧就用 `-t` 限长，别用输入 `-ss`。

## 8. 画质：判"有没有变糊"的尺子（2026-11-11 补）

工具 `tools/convert-metrics/measure_quality.py`（numpy + scipy），取法：

```bash
python tools/convert-metrics/measure_quality.py --source SRC --output OUT \
       [--crop 1920:800:0:140] [--window 20] [--json r.json]
python tools/convert-metrics/measure_quality.py --calibrate CLIP --window 8 --work build-convert-metrics
```

**为什么不能只看一个 PSNR**：整片误差能量几乎全在最低频带，relight 与"变糊"能印出同一个
`psnr_mean`。所以按 σ=1 / σ=4 两级高斯把 luma 拆成 low / mid / high 三带，各带单独算 PSNR，
并报每带的相关系数——**相关说"变没变"，能量比说"少了多少"**：

| 读数 | 含义 |
|---|---|
| `high e-ratio` ≈ 1.0 且 high PSNR 高 | 细节量保住了 |
| `high e-ratio` < 0.9，high corr 仍 > 0.9 | 变软，但同一张画 |
| `high e-ratio` ≈ 1.0 但 corr < 0.85 | 源本身有损时的二次编码代价（实测 crf10 源过 crf18 只到 0.79），不是细节丢了 |
| `high e-ratio` < 0.9 **且** corr < 0.8 | 糊到边缘都对不上了 |
| low 带低、high 带正常、e-ratio ≈ 1 | 只改了色调（relight），细节未动 |
| 全带都掉、e-ratio ≈ 1 | **帧没对齐**，先看偏移探测，别当画质问题 |

最后一行是最容易误判的：一帧错位会让全带掉到 32 dB，看着像灾难。`run()` 会先探 −2..+2
的偏移并用最好的那个，同一对文件在 −1 偏移下读 50.7 dB。**报 PSNR 之前必须先跑这个探测。**

本机刻度（1920×1080 @ 23.976，8 s，来自真实影片，每条臂只改一件事）：

| 臂 | all | low | mid | high | hi-corr | e-ratio | SSIM |
|---|---|---|---|---|---|---|---|
| crf18 近无损 | 47.05 | 63.39 | 55.33 | 49.71 | 0.978 | 0.994 | 0.9881 |
| crf28 可见损失 | 41.95 | 54.03 | 48.31 | 46.50 | 0.952 | 0.965 | 0.9705 |
| blur σ0.5 | 44.78 | 67.07 | 53.95 | 46.76 | 0.973 | 0.809 | 0.9930 |
| blur σ1.0 | 37.62 | 60.10 | 44.51 | 40.74 | 0.901 | 0.560 | 0.9841 |
| blur σ2.0 | 32.03 | 50.74 | 36.73 | 37.53 | 0.735 | 0.308 | 0.9608 |
| 半分辨率 | 36.84 | 59.38 | 48.60 | 38.34 | 0.709 | 0.584 | 0.9841 |
| 亮度 +5% | 25.36 | 25.37 | 59.54 | 52.26 | 0.988 | 1.007 | 0.6883 |
| 提前一帧 | 32.32 | 37.06 | 39.05 | 44.61 | 0.924 | 1.001 | 0.9050 |

**门槛**（据此判"这次转换能不能出"）：
- 可发布：`high e-ratio ≥ 0.81` 且 `high PSNR ≥ 46 dB`（等价于"不比 blur σ0.5 更糊"）；
- crf28 的 0.965 是白送的，重编码本身不是代价；
- `e-ratio < 0.56` 是人眼能看出来的软，不能出。

未覆盖：VMAF（缺 libvmaf 与模型）、时域闪烁（在 `tools/benchmark/analyze.py` 里，那边有配好的帧对）、
4K 与 HDR 素材（本机没有 HDR 样本，P0-8 的色彩标签问题仍在）。

