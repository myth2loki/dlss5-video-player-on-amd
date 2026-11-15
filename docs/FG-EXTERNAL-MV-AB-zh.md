# 插帧外部运动场 A/B：预注册门禁（跑批前写死）

> 状态：**实验臂，默认关闭**。本文的阈值与判决规则写在任何一次运行之前；
> 看到数字后不许调阈值。代码：`src/FrameGenMotionPolicy.h`、
> `src/FsrFrameGenBackend.cpp`、`src/D3D12Renderer.cpp`；脚本：
> `tools/flow-ab/run_fg_mv_arm.sh`、`tools/flow-ab/compare_fg_mv.py`。

## 1. 要回答的问题

FidelityFX Frame Generation 的 `ffxDispatchDescFrameGenerationPrepareV2::motionVectors`
是一个**真句柄**，不是占位。播放器一直给它一张清成 `{NaN,NaN}` 的零场，
并开 `FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS`，
让运行时从颜色缓冲自己推运动。之前记录里「SDK 不导出」说的是**取不出**它自估的场，
不等于**喂不进**我们的场。

本 A/B 只问一件事：把播放器已有的 FFX 光流场（`m_motion` 同源，整像素、
8×8 块、current→previous）交给插帧 island，生成帧的鬼影会不会比自估更好。

**不是**要证明「统一接入」可行——可行性已经由接口存在给出；要证明的是**有收益**。

## 2. 三臂与开关

三臂共用同一个 exe、同一份 ini、同一段素材，只改环境变量：

| 臂 | `DLSS5_FG_EXTERNAL_MV` | 场 | create flag |
|---|---|---|---|
| `control` | 未设置（脚本会 `unset`） | NaN 零场（现行） | 保留 |
| `external` | `1` | 播放器 FFX 场 | **清除** |
| `display` | `display` | 播放器 FFX 场 | 保留 |

- `display` 是**对照臂，不是第三种候选行为**。`ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS`
  的确切语义只见于本地重建的 `src/FsrFrameGenAbi.h`，本仓库没有 SDK 的
  framegeneration 头可核。如果它其实只是「矢量在 display 分辨率」的声明而非
  「自己推运动」的开关，那么 `external` 与 `display` 会给出同样的结果 —— 这正是
  该臂存在的意义：把「flag 不是我们以为的开关」和「外部场没效果」区分开。
- 三臂都设 `DLSS_VIDEO_MOTION_FLOW=ffx`，保证估计器相同。
- `DLSS5_FG_MV_SCALE` 默认 `1.0`（把 decoded 像素换算成 island rung 像素的精确比值
  `fgW/sourceW`、`fgH/sourceH`）。负数用于翻方向、`0.5` 用于半程位移，都是
  **排查工具**，不进判决。
- 实验臂要求 `m_ffxOfActive == true` 且缩放非零；否则代码会拒绝并回落到自估，
  日志有明确一行（见 §6）。

## 3. 测量通道

`DLSS5_FG_DUMP=<目录>`（+ `DLSS5_FG_DUMP_FROM` / `_COUNT`）每个采样源帧写出三张
**BGRA8、island rung 尺寸**的原图：

- `gen_NNNNN.bgra8`：**island 的原始 midpoint，在 envelope clamp 之前**。
  clamp 把每个像素钳进两帧的 min/max，必然掩盖鬼影，所以绝不能 dump 钳后结果。
- `cur_NNNNN.bgra8` / `prev_NNNNN.bgra8`：clamp 用的同一对源帧。
- `manifest.txt` 13 列：`index w h rowPitch frameId intervalMs fgReset hasPrev arm
  decodedFrameNumber pts100ns historyGeneration srcReset`。

**必须按 `decodedFrameNumber` 配对，不按文件顺序**：丢帧或 island 单帧失败时，
按序号配会把不同帧比在一起。所有 decoded 号都是 0 时脚本才会退化到 index，
并在输出里明说。`historyGeneration` 在一次运行内变化 → 直接 `INCONCLUSIVE`，
不放宽门禁。

## 4. 预注册判决口径（写在看数之前）

范围：**≥ 2/3 的配对帧**（`ceil`），reset 帧、几何不一致的帧、
`historyGeneration` 被打破的运行都不算。

- **EFFECT**：外部臂与 control 的 `gen_` 相差 > 6 个码值的像素占比 > 1%。
  不达标 = 这一臂什么都没改（场没喂进去，或 flag 不是那个开关）。
- **GAIN**：`overshoot`（生成像素落在 `[min(cur,prev)-6, max(cur,prev)+6]` 之外）
  外部臂比 control **低至少 0.05 个百分点**。这是 clamp 本来会藏起来的鬼影。
- **FLAG CONTROL**：`display` 臂与 `external` 的 `gen_` 差 <= 1% 的帧数，
  用来判断 flag 是否真的换了行为。

判决：

| EFFECT | GAIN | 结论 |
|---|---|---|
| 否 | — | `no measurable output-frame difference` |
| 是 | 否 | `changed the output but did not reduce ghosting` |
| 是 | 是 | `measurable gain`，**仅**作为实片验证候选，**不改默认值** |
| — | 无 `prev_` | `INCONCLUSIVE`，overshoot 无法度量 |

**噪声地板必须先量**：同一臂同配置复跑一次，两跑之间的 `diff%` / `overshoot` 差值
就是地板。若地板接近 0.05 pp 或 1% 门槛，判 `INCONCLUSIVE`，不许把噪声当信号。
（既有教训：冷启动 `encoder back pressure` 会让多一次 `history reset`，
哈希差异不是回归；同配置复跑能把它排除。）

「无显著差异」和「测不了」是**两个不同结论**，不许合并成「没效果」。

> 2026-11-15 补充一条**有效性前提**（不是判决阈值）：两源帧几乎相同的静止帧
> 在打分前排除 —— 静止画面上任何估计器都返回同一张图，该帧无法区分成功与失败。
> 判据与动机见 §8.1。`EFFECT` / `GAIN` 的数值和 2/3 门槛**一字未改**。

## 5. 跑法

```bash
cd /f/dlss5-video/dlss5-video-amd
# 素材放 tools/flow-ab/clips/<name>.mp4
for arm in control external display; do
  bash tools/flow-ab/run_fg_mv_arm.sh <name> "$arm"
done
PY=/c/Users/Administrator/AppData/Local/Python/bin/python   # 需带 numpy
"$PY" tools/flow-ab/compare_fg_mv.py \
  --control  tools/flow-ab/runs-fg-mv/<name>/control \
  --external tools/flow-ab/runs-fg-mv/<name>/external \
  --display  tools/flow-ab/runs-fg-mv/<name>/display \
  --json     tools/flow-ab/runs-fg-mv/<name>/verdict.json
```

脚本会备份 ini、只改 `[Playback]` 的 `FrameGenerationPlayback` 与 `SuperResolution`
两个键、EXIT trap 恢复（含 taskkill 之后）。这两个键**没有**命令行/env 覆盖，
改 ini 是唯一路径；玩家退出时会回写 ini，所以必须备份。

`--out` 类路径必须是 **Windows 风格**：本机原生 Windows Python 会把 `/c/Temp/...`
解析成 `F:\c\Temp\...`。脚本已用 `cygpath -w` 转换。

## 6. 自检：必须看到哪几行（不是「跑通即可」）

每臂的 `self-check.txt`（从 player.log 抽出）必须包含：

1. `Motion guide backend: FidelityFX optical flow ...` —— 估计器确实是 FFX。
2. `the island produced its first frame` / `cadence active` —— island 真在生成帧。
3. `Generated frame dump: wrote <COUNT> raw midpoints` —— 采样帧数达标，
   少一行都不算数。
4. **`external` / `display` 臂额外**必须有
   `Frame generation: external motion field is being drawn at ...`；
   缺这行说明场根本没画，`EFFECT` 再低也不能判「没效果」，只能判「没跑起来」。
5. `control` 臂**不得**出现第 4 行。

另外核 `manifest.txt`：13 列齐全、`arm` 列分别恒为 0/1/2、
`hasPrev=1`、`decoded` 号在两臂能对上。缺 `prev_` 时 overshoot 直接不可度量。

**脏臂检出**：`ffmpeg: Error reinitializing filters!` 冷启动偶发、
`reset` 跳变、帧号不连续 → 补跑，不放宽门禁。写满 COUNT 帧不等于 COUNT 帧可用。

## 7. 已知边界（别把结论外推）

- **只改运动这一半**：FG 还吃 depth，播放器只有亮度代理、没有相机近远平面，
  继续喂常数 1.0（远平面）。所以就算赢，也只是「统一了运动」，不是「统一了引导」。
- **场是整像素 8×8**：FFX OF 的输出精度低于 FG 自估（后者自带遮挡/亚像素处理）。
  既有 A/B 里 1 px/帧是 −2.94 dB、2~8 px/帧 +2.7~3.4 dB，慢镜头更可能输。
- **只量 raw midpoint**：最终呈现还过 envelope clamp；若外部场只在被钳掉的
  区域有差，用户看不到。赢了之后必须补一次「开 clamp 的真机画面」验证。
- **一次 GPU drain / 帧**：这是测量配置，不是播放配置；`guide=` 耗时、
  cadence、帧时间不能从这一臂读。
- **`DLSS5_OUTPUT_DUMP` 与 `DLSS5_FG_DUMP` 同时开会每帧 drain 两次**，别叠着用。
- `DLSS5_FG_EXTERNAL_MV` 只在实验里有效；默认 unset 时没有第三张共享纹理、
  没有额外 draw、create flag 一字不变、插帧仍走自估。**任何默认行为的改变
  都要另开一次讨论，不能由本 A/B 顺手合入。**

## 8. 首次运行记录（2026-11-15，事后记录；判决阈值未改）

**先说结论**：`external field changed the output but did not reduce ghosting` ——
**不默认开启**，也不建议为「统一」继续推这条支路。规则与阈值一字未调。

### 8.1 联调中修掉的三个真问题（都不是调阈值）

1. **决策时机错**：`CreateFrameGenResources()` 比 `CreateVideoResources()` 早，
   而 FFX 光流是在后者里初始化的。原代码在前者里查 `m_ffxOfActive`，
   结果**任何机器上每次都被拒绝**。首次冒烟日志明确写了 `was refused`，
   但 dump 仍正常出帧 —— 若不按 §6 核「必须看到哪一行」，会把一个
   「场根本没喂进去」的运行读成「场没效果」。修法：把最终判定移到
   `EngageFrameGenIsland()`（FFX 已就绪之后），并把拒绝时的 manifest `arm`
   列归零，保证 manifest 记录的是**实际运行的臂**。
2. **alpha 通道不是颜色**：BGRA8 的 island 输出 alpha=0、两张源帧 alpha=255。
   初版脚本把 4 个通道一起做 overshoot，静止片段上得到 **100% 越界**的假值。
   修法：只取 B/G/R 三通道。这属于「指标不能区分成功与失败」，不是阈值问题。
3. **静止窗口无信息量**：`FG_DUMP_FROM=60/120` 落在素材的静止段
   （`mean|cur-prev| = 0.017`），任何插帧都返回同一张图，三臂逐字节相同。
   修法：加 `STATIC_MEAN_ABS = 1.0` 的**有效性前提**（见脚本注释），
   静止帧在打分前排除；并让 `FG_RUN_SECONDS` 可配，以便采到素材后段的
   运动窗口。`EFFECT` / `GAIN` 阈值**未改**。

另外补一处资源安全：`WaitGPU` 超时后不再复用同一命令分配器（诊断路径
不允许把设备带下水），该会话停止 dump。

### 8.2 有效数据

素材 `tools/benchmark/fixtures/demo-capture-20260912.mp4`（1920×1080 /
30 fps / 678 帧），窗口 `FG_DUMP_FROM=440`、`COUNT=12`、`FG_RUN_SECONDS=21`
（≈14.7 s，实测该段 `srcMot` 6.8–9.3 码值，全部为运动帧）。
三臂 manifest 均 13 列、几何 2560×1440、`hasPrev=1`、reset 全 0；
control/external 的 `historyGeneration` 恒为 9。

| 指标 | 首跑 | 同配置复跑 |
|---|---|---|
| 配对有效帧 | 12/12 | 11/11 |
| EFFECT（>1% 像素变化） | **12/12**（2.5%–15.1%） | **11/11** |
| GAIN（overshoot 改善） | **0/12**（control 4.9–7.5%，external 6.9–11.1%，**恶化约 2–3 pp**） | **0/11** |
| FLAG CONTROL（display≈external） | 11/12 | — |

**噪声地板**：同配置各复跑一次，`control↔control` **0/12** 帧超 1%、
`external↔external` **0/11** 帧超 1% → 地板远低于 EFFECT 门槛，指标灵敏；
两跑的 overshoot 也未达 0.05 pp 差。所以上面的变化不是运行抖动。

**顺序上的诚实交代**：首跑先出了数，噪声地板是随后补量的，没做到 §4 写的
「先量地板」。但判决不依赖地板假设 —— 它是负面结论，且由第二次独立运行
完整复现；补量地板是为了排除「变化其实来自噪声」这一种解释。

### 8.3 读出来的两件事

- **外部场确实喂进去了，而且改了输出**（EFFECT 满票），但**鬼影更差**，
  不是「无效果」。整像素 8×8 的场替换掉运行时自带的遮挡/亚像素估计，
  与既有 1 px 平移 −2.94 dB 的方向一致。按 §4 的表格，判「改了但没减鬼影」，
  按第 4 步**不谈默认开启**。
- **`display` 与 `external` 11/12 帧相同**：清不清
  `ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS` 几乎没有区别，说明它**不是**
  本地 ABI 头注释里那个「自估 vs 读外部场」的开关（更可能只是分辨率声明，
  或运行时两路都读了 `prepare.motionVectors`）。这一条仍是**待查**，
  本 A/B 只能说「对照臂没能区分」，不能给 flag 下语义结论。首次小窗口
  复跑里 display 臂还出现过一次丢帧 / 帧时间膨胀（66.7 / 100 ms），
  加大预热后未复现 —— 单次观察，不判因果。

### 8.4 不外推

单素材、单窗口、单次判决（含一次复现）。**不能**据此说「实片上一定更差」，
也不能说「所有片段都没收益」。要推翻这个负面结论，需要换不同运动幅度的
多段素材、按同一套预注册规则重跑，并先量各自的噪声地板。

**代码状态**：`DLSS5_FG_EXTERNAL_MV` 保持实验开关、默认 unset；
shipped 路径仍是 NaN 零场 + 运行时自估，本次没有改任何默认行为。

## 9. 与既有结论的关系

- `docs/MOTION-GUIDE-AB-zh.md`：输出帧三臂判 `no measurable output-frame difference`；
  神经渲染换稠密场 +0.21/−0.02/+0.08 dB、1/3 过门禁 → 不值得接。
- `docs/ARCHITECTURE.md` 的 *One estimator or two*：光流场只服务 FSR 4 #1 的时域
  重投影；CPU 网格场服务神经渲染与深度；插帧自估是第三条互不相知的路径。
- 本次是**第三条路径**的第一次对照实验，不改变上面任何一条结论。

## 10. 实片多段复验（2026-11-15，跑批前预注册）

> 本节写于**任何一段 control/external A/B 运行之前**。此前只量过素材的
> 帧间运动量与切镜位置（有效性前提），不含任何臂间比较数字。
> 动机：§8.4 明确写了单素材单窗口不能外推，要推翻或确认负面结论必须换
> 不同运动幅度的多段素材、按同一套规则重跑，并先量各自的噪声地板。

### 10.1 素材与窗口（已量，非判决）

源：`M:\电影\阳光电影www.ygdy8.com.头号玩家.BD.720p.国英双语双字.mkv`
（1280×720 / 24000/1001 / h264 / 8398 s）。整片按 1 fps 抽帧粗扫，再在
**原始 24 fps 相邻帧**尺度上量 `mean|d|`（320×180 gray）。片尾 7771–7891 s
近乎全黑、3991/4921 s 等 97–100% 帧差 < 1 的段落，按 §4 有效性前提排除。

六段用 ffmpeg 精确 seek + 重编码（CRF 18 / 固定 GOP 24 / 无 B 帧 / 无音轨），
统一 1280×720 / 24000/1001 / 528 帧。窗口选择规则：连续 12 帧（0.5 s）内
及前 1 s 均无切镜尖峰、且 ≥90% 帧有运动。切片后复核值：

| 段 | 源起点(s) | `FG_DUMP_FROM` | 窗口 mean\|d\| | 窗口内 ≥1 的帧 | 窗口内切镜 |
|---|---|---|---|---|---|
| `rp01` | 5127 | 156 | 2.44 | 100% | 0 |
| `rp02` | 1531 | 132 | 4.97 | 100% | 0 |
| `rp03` | 2787 | 120 | 8.82 | 100% | 0 |
| `rp04` | 897 | 144 | 12.55 | 100% | 0 |
| `rp05` | 720 | 156 | 16.87 | 100% | 0 |
| `rp06` | 2881 | 240 | 23.06 | 100% | 1（快摇段，允许） |

覆盖 2.4 → 23.1 码值，含低运动 / 中低 / 中 / 中高 / 高 / 快摇。
`COUNT=12` 不变；`FG_RUN_SECONDS` 按 `FROM/fps + 7~8 s` 逐段给，
只影响采样能不能到达，**不影响任何判决数值**。

### 10.2 一字不改的东西

- §4 全部阈值：`TOL=6`、`EFFECT_PCT=0.01`、`GAIN_FRACTION=0.0005`、
  `STATIC_MEAN_ABS=1.0`（有效性前提）、`MIN_SHARE=2/3`。
- §6 的自检行、§2 的三臂定义、`compare_fg_mv.py` 的配对与脏臂检查
  （按 decoded 配对、排除 `fgReset/srcReset`、几何一致、
  `historyGeneration` 单次运行内恒定、只取 B/G/R）。
- shipped 默认：NaN 零场 + 运行时自估，本次不改任何默认行为。

### 10.3 每段跑什么

| 组 | 臂 | 用途 |
|---|---|---|
| A | `control` 首跑 | 主判决基准 |
| B | `external` 首跑 | 主判决实验臂 |
| C | `control` 同配置第二次 | **噪声地板** |
| D | `external` 同配置第二次 | **噪声地板** + 独立复现 |
| E | `display`（仅 `rp03` / `rp06`） | flag control，沿用 §2 |

C/D 与 A/B 分放在两个独立根目录，互不覆盖。
**输出顺序写死**：先逐段算并打印地板（A↔C、B↔D 各跑一次 compare），
地板数字全部列出之后，才允许输出主判决（A↔B）与独立复现（C↔D）。
这次不重蹈 §8.3「先出数、后补地板」的顺序问题。

### 10.4 段级判决与多段汇总口径（跑批前定死）

每段按 §4 表格给出**一个**段级 verdict。定义 `N_valid` = 段级判决非
`INCONCLUSIVE` 的段数。

- `N_valid < 4` → 整体 `INCONCLUSIVE`，**不许**用 2/3 去凑。
- **确认负面**：≥ `ceil(2/3 × N_valid)` 段判
  `changed the output but did not reduce ghosting` 或
  `no measurable output-frame difference`，且 `measurable gain` 段数 **= 0**
  → 维持默认关闭，并把 §8.4 的「单素材」限制升级为「6 段实片覆盖
  2.4–23.1 码值仍无收益」。
- **推翻负面**：≥ `ceil(2/3 × N_valid)` 段判 `measurable gain`，且这些段的
  地板都远低于门槛 → 才进入「开 clamp 的真机画面」候选，**仍不改默认**。
- 其余（混合、地板接近门槛、INCONCLUSIVE 偏多）→ `MIXED / INCONCLUSIVE`，
  保持默认关闭，逐段列出哪些段有增益，供后续定向复验；
  **不许**为凑结论调阈值、删段、改窗口或改 `FROM`。
- 段级 `INCONCLUSIVE`（`historyGeneration` 变化、无 `prev_`、全静止、
  `gen_frames < 12`、缺 §6 自检行）不计入 `N_valid`，只补跑，不放宽门禁。
- **噪声地板**：每段 A↔C、B↔D 各自跑一次 compare。若任一出现 ≥1 帧超
  EFFECT 门槛（> 1% 像素差 > 6 码值），该段标 `DIRTY`，补跑该臂一次；
  补跑后仍 DIRTY → 该段 `INCONCLUSIVE`。地板数字必须逐段列出，
  不许只写「已量」。地板本身**不改变** EFFECT/GAIN 阈值。
- **判决按段，不按整片均值**：repeat 零信息量、方差在镜头之间（既有铁律：
  门槛 ≥ 2/3 镜头同号且每段独立过线，整片平均不算数）。

### 10.5 自检（沿用 §6，另加）

每段每臂必须：`gen_frames=12`；`external`/`display` 臂出现
`external motion field is being drawn`，`control` 臂不得出现；
manifest 13 列、`arm` 恒 0/1/2、`hasPrev=1`、单次运行内
`historyGeneration` 恒定、`fgReset/srcReset` 全 0。
缺任一项 → 该臂作废补跑，**不算成「没效果」**。

### 10.6 已知边界（仍不外推）

- 仍只量 **raw midpoint**（clamp 之前），最终呈现还过 envelope clamp；
  赢了也必须另做开 clamp 的真机画面验证。
- 仍只统一**运动**这一半，FG 的 depth 继续喂常数 1.0。
- 源为 720p、SR 到 2560×1440，缩放比与上次 1080p 素材的 1.3333 相同；
  但内容、编码、运动类型不同，正是本轮要覆盖的变量。
- 重编码 CRF 18 引入编码噪声，三臂读同一文件，配对不受影响。
- 6 段仍只是一类素材（同一部电影），不能代表所有内容；
  结论限定在「这 6 段 + 这套测量通道」。

## 11. 实片多段复验结果（2026-11-15，按 §10 预注册口径执行）

**先说结论**：`NEGATIVE CONFIRMED` —— 5 个有效段全部判
「改了输出但没减鬼影」，0 段有增益，噪声地板全 0；**默认保持关闭**。
§10 的阈值、窗口、`FROM`、判决规则一字未改。

### 11.1 运行与自检

`main`（6 段 × control/external）、`repeat`（同 6 段 × 同 2 臂）、
`display`（rp03/rp06）、rp06 补跑 2 臂、加一次链路冒烟，共 29 臂。
每臂 12 帧、2560×1440、manifest 13 列。

- `rp01`–`rp05` 共 20 臂 + 1 个 display 臂**全 OK**：
  `frames=12`、`wrote=1`、control `drawn=0` / external `drawn≥1`、
  island / cadence / FFX backend 日志齐全、`rows=12 bad=0 hg=1`。
- **`rp06`（23.06 码值快摇段）判 `INCONCLUSIVE`，不计入分母**：
  4 次运行 + 2 次补跑全部在 decoded 242、251 两帧出现 `fgReset=1`，
  `historyGeneration` 20→21→22。补跑逐帧复现同一模式 → 是素材切镜，
  不是运行抖动。按 §10.4 **只补跑，没有改窗口、改 `FROM` 或放宽门禁**；
  这一段的判决留空，不假装测过。

### 11.2 噪声地板（按 §10.3 先于任何臂间判决打印）

| 段 | control A↔C | external B↔D | 结论 |
|---|---|---|---|
| rp01 | EFFECT 0/12 | 0/12 | clean |
| rp02 | 0/12 | 0/12 | clean |
| rp03 | 0/12 | 0/12 | clean |
| rp04 | 0/11 | 0/11 | clean |
| rp05 | 0/12 | 0/11 | clean |
| rp06 | INCONCLUSIVE | INCONCLUSIVE | DIRTY → 段级 INCONCLUSIVE |

`paired=11` 的段是两侧 decoded 号交集少一帧（丢帧），按帧号配对后取交集，
不是脏臂。地板 0/N 远低于 1% 门槛，指标能区分成功与失败。

### 11.3 逐段主判决

`main` 与独立 `repeat` 两组数字几乎逐位相同。括号内为 overshoot
（生成像素落在两源包络外的比例）control → external。

| 段 | 运动量 | EFFECT | GAIN | overshoot 变化（main / repeat） | 段级判决 |
|---|---|---|---|---|---|
| rp01 | 2.44 | 11/12 | **0/12** | 0.02%→0.10% (+0.09 pp) / 同 | 改了但没减鬼影 |
| rp02 | 4.97 | 12/12 | **0/12** | 1.44%→2.58% (+1.14 pp) / 同 | 改了但没减鬼影 |
| rp03 | 8.82 | 12/12 | **0/12** | 1.43%→3.23% (+1.80 pp) / 同 | 改了但没减鬼影 |
| rp04 | 12.55 | 11/11 | **0/11** | 3.69%→6.46% (+2.77 pp) / 同 | 改了但没减鬼影 |
| rp05 | 16.87 | 11/11 | **0/11** | 7.37%→12.12% (+4.75 pp) / +4.78 pp | 改了但没减鬼影 |
| rp06 | 23.06 | — | — | 不可度量 | INCONCLUSIVE |

`display` 对照臂（rp03）与 `external` **12/12 帧相同** —— 再次说明
`ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS` 不是本地 ABI 头注释里那个
「自估 vs 读外部场」的开关；语义仍**待查**，本实验不给它下结论。

### 11.4 多段汇总（§10.4）

`N_valid = 5`（≥4 门槛）、`need = 4`、`negative = 5`、`gain = 0`
→ `NEGATIVE CONFIRMED: >=2/3 segments show no ghosting gain and no segment
shows measurable gain - keep the default off`。

§8.4 的「单素材、单窗口、不能外推」由此升级为：**6 段实片覆盖
2.4–16.9 码值的相邻帧运动量，仍无一段有鬼影收益**。

### 11.5 新读出的一条：代价随运动幅度单调增长

把上表按运动量排序，外部场带来的 overshoot 恶化是
**+0.09 → +1.14 → +1.80 → +2.77 → +4.75 pp**，单调递增，且两次独立
运行几乎逐位复现。整像素 8×8 场替换掉运行时自带的亚像素/遮挡估计后，
位移越大误差越大 —— 与 §7 记的「1 px/帧 −2.94 dB」方向一致，
但**两者不是同一个指标**（那是 FSR 4 输出帧 PSNR，这是插帧 raw
midpoint 的 overshoot），不要混为一谈。

另一处要分清：最低运动段 rp01 仍有 11/12 帧过 EFFECT（1.45% 像素变化），
但鬼影只多 0.09 pp。**「场确实喂进去了」与「有收益」是两件事**；
低运动不是「没生效」，只是代价小到接近地板。

### 11.6 仍然不外推

- 6 段来自同一部电影、同为 720p→2560×1440，每段只有 12 帧；
  最快的一段（23.06 码值）**没测成**，不能说「快摇也一定更差」。
- 仍只量 clamp 之前的 raw midpoint；depth 继续喂常数 1.0。
- flag 语义未查清，不影响本次负面判决。
- `DLSS5_FG_EXTERNAL_MV` 仍是实验开关、默认 unset；
  **本次代码零改动，shipped 路径一行默认行为都没变**。

### 11.7 复现

```bash
cd /f/dlss5-video/dlss5-video-amd
PY=/c/Users/Administrator/AppData/Local/Python/bin/python   # 需 numpy
MOVIE_PHASE=main    bash tools/flow-ab/run_movie_batch.sh
MOVIE_PHASE=repeat  bash tools/flow-ab/run_movie_batch.sh
MOVIE_PHASE=display bash tools/flow-ab/run_movie_batch.sh
"$PY" tools/flow-ab/summarize_movie_ab.py --phase floor     # 先地板
"$PY" tools/flow-ab/summarize_movie_ab.py --phase verdict   # 后判决
```

切片表与 `FG_DUMP_FROM` 在 `run_movie_batch.sh` 的 `SEGMENTS`；
素材在 `tools/flow-ab/clips/`（gitignore）。判决数据在
`tools/flow-ab/runs-movie/movie-{floor,verdict}.json`。
