# AMD 真引导 A/B 验证方案（无需人工介入）

状态：**待验证**。代码已改完（本机 macOS 无法编译），在 Win11 + RX 9070 XT 上按本文执行。
对应改动：`AmdLmxxfEvaluator.h/.cpp`（内存引导 + 运动场跨距修复）、`OfflineNeuralRenderer.cpp`（AMD 路径撤三条拒绝、接 CPU 估计器）、`tools/benchmark/guidefiles.py`（`zero` 子命令）、`tools/benchmark/amd-guides.profile.json`（四个臂）、`tools/benchmark/amdguides.py`（驱动脚本）。

**执行 = 构建 + 一条命令**：

```bat
build-fsr4.bat
set DLSS_BENCHMARK_BUILD=F:\dlss5-video\dlss5-video-amd\build-ninja
python tools\benchmark\amdguides.py
```

零场生成、臂的依赖顺序、四项自检、往返证明、指标读数与总判定全部在脚本内完成，
不需要人工设环境变量、数帧数、按顺序调 run.py、查日志或比对 md5。退出码 0 表示
"四臂都渲染成功 + 自检全过 + 往返逐位一致"；A/B 结论无论如何都会打印。

## 0. 背景与待回答的问题

AMD 路径（amd-lmxxf runtime）的时域模型目前只吃过两种引导：常量（零运动 + 深度 1.0），或 `mv=file:` 的离线文件。本方案把 **CPU 块匹配估计器**（`TemporalGuideGenerator`，NVIDIA 路径一直在用）接到 AMD 路径上，让 runtime 的历史第一次吃到真运动场，然后用三个受控臂回答一个问题：

> **在视频上，真运动场比零运动场好多少？值不值得为它承担重投影错误的风险？**

已有证据（先读再跑）：
- `docs/measurements/neural-mv-gate-20260924/REPORT.md` —— NVIDIA 侧：**零运动反而更好**（still-hold sigma 0.439→0.366、PSNR 36.34→36.50）。
- `docs/AMD-TEMPORAL-ACCUMULATION-zh.md` —— runtime 时域的融合权重只有 0.081，收益上限小。
- `tools/benchmark/roundtrip.profile.json` + `guidefiles.py` 的注释 —— 往返证明的机制。

## 1. ⚠️ 先决条件：本次修复了一个会让"真运动臂"失效的 bug

`AmdLmxxfEvaluator::UploadGuides` 的文件读取把 3 通道运动 PFM 重采样到全帧后，用 **2 通道跨距**写进 R32G32 纹理——每个 y 落到下一个像素的 x 上，且 1/3 的纹素读到填充 0。**修复之前所有经 `mv=file:` 喂真运动场的 AMD 测量，其运动场都是错排的**（零场不受影响——零怎么排都是零，所以 run 6 的零场臂结论仍有效）。本次 A/B 必须用包含此修复的构建；历史"真场"数据不再可比。

## 2. 本次代码改动（已提交，待编译）

| 文件 | 改动 |
|---|---|
| `AmdLmxxfEvaluator.h/.cpp` | 新增 `FrameGuides`/`SetFrameGuides`：每帧内存引导，优先于引导目录；`UploadGuides` 修复 3→2 通道跨距错误 |
| `OfflineNeuralRenderer.cpp` | AMD 适配器接 `TemporalGuideGenerator`（fps/布局进复用判断、场景切、reset 传播）；`RunAmdJob` 撤三条拒绝，改为：`--guide-dump` 与 `--zero-motion-test 1` 仅在 `mv=cpu` 时可用；`temporalHistory = 文件引导 ‖ CPU 估计器` |
| `guidefiles.py` | 新增 `zero` 子命令：生成零运动场（零场尺寸不变性，任意分辨率等价） |
| `amd-guides.profile.json` | 四臂 profile（下表） |

zero-motion test 的 AMD 语义：**上一帧与本帧逐字节相同 → 运动场置零**（在文件叠加与 dump 之前做，所以 dump 记录的就是渲染所用场）。这是 `NvofResolveShader.h` 测试的宿主侧镜像。

## 3. 四个臂

| profile | guides | dump | 说明 |
|---|---|---|---|
| `amd-cpu` | `mv=cpu,depth=1` | `{clip}/cpu/` | **C 臂**：真运动 + 深度代理，lease 生命周期，dump 供 Z 臂与往返证明用 |
| `amd-cpu-nzmt` | C 臂 + `--zero-motion-test 1` | `{clip}/cpu-nzmt/` | **C' 臂**：唯一差异是静止帧零化——量化块匹配在 held 帧上的噪声危害 |
| `amd-zmv` | `mv=file:{clip}/zero/mv,depth=file:{clip}/cpu/depth` | — | **Z 臂**：零运动场 + C 臂同款深度 → 与 C 臂唯一差异是运动场 |
| `amd-cpu-replay` | `mv=file:{clip}/cpu/mv,depth=file:{clip}/cpu/depth` | — | **往返证明**：C 臂 dump 回喂，必须与 C 臂逐位一致（验证 stride 修复） |

零运动场由脚本写到 `{clip}/zero/mv/`（`guidefiles.py zero` 的布局），C 臂的 dump 写到
`{clip}/cpu/`，C' 臂独立写到 `{clip}/cpu-nzmt/`——**两个臂不能共用一个 dump 目录**，否则
谁后跑谁决定另一臂的输入，Z 臂与往返证明读到的就不是 C 臂的画。

全部走 `--neural-backend amd-lmxxf`。**执行顺序由脚本固定：amd-cpu 必须最先**
（Z 臂与往返证明读它的 dump），脚本会拒绝违反这个顺序的 `--arms`。

## 4. 执行步骤（Win11 + 9070 XT）

### 4.1 构建

```bat
build-fsr4.bat
```

产出 `build-ninja\Release\neural-runtime\NeuralWorker.exe`（`add_dependencies(DLSSVideoPlayer NeuralWorker)`，
构建播放器会连带重建 worker）。两处路径问题脚本会在渲染前拦下：

- harness 默认在 `build-upscaling/` 找 worker，而本工程构建到 `build-ninja/` → 设一次
  `set DLSS_BENCHMARK_BUILD=F:\dlss5-video\dlss5-video-amd\build-ninja`
- `benchmark-work/runtime-snapshot/` 若存在旧快照，`run.py` 会**优先用它**，新编译的 worker
  根本不参与——那样往返证明测的是没有 stride 修复的旧二进制。脚本会报错并要求删除该目录。

### 4.2 跑（一条命令，其余全自动）

```bat
python tools\benchmark\amdguides.py
```

首次运行会自动构建语料。**不需要**手工设 `DLSS5_AMD_RUNTIME_DIR`（脚本按
`AmdLmxxfRuntime.cpp` 的解析顺序自己找：显式参数 → 环境变量 → 模块目录及父/祖父）、
**不需要**手工数帧数生成零场（按 manifest 里每个 clip 自己的帧数与分析网格生成）、
**不需要**手工按顺序调 run.py（Z 臂与往返臂依赖 C 臂的 dump，脚本按依赖顺序跑）、
**不需要**手工查日志做自检（脚本解析 worker 日志，见第 5 节）、
**不需要**手工比对 md5（脚本自己比）。

常用变体：

```bat
python tools\benchmark\amdguides.py --amd-runtime F:\amd-neural-runtime
python tools\benchmark\amdguides.py --clips still-hold --skip-corpus
python tools\benchmark\amdguides.py --skip-analyze     :: 只渲染，不跑指标
python tools\benchmark\amdguides.py --fresh            :: 重渲染已存在的臂（改了 profile 或 dump 目录后用）
```

**`--fresh` 什么时候必须加**：`run.py` 见到 `runs/<clip>__<arm>__1/result.json` 就跳过该臂。
改了 profile（例如 dump 目录改名）、换了构建、或怀疑上次跑到一半，都要带 `--fresh`，
否则你看到的是上一轮的画面，而这一轮的 dump 目录可能压根没被写。

退出码 0 = 四臂全部渲染成功 + 自检全过 + 往返逐位一致。
A/B 结论无论正负都会打印出来——它是测量结果，不是通过/失败的门。

脚本还会写一份机器可读的自检记录：
`build-upscaling/benchmark-work/amd-guides-ab/selfcheck.json`。

### 4.3 单独调某一步（调试用，日常不需要）

```bat
python tools\benchmark\run.py --profile-file tools\benchmark\amd-guides.profile.json ^
    --clips still-hold --profiles amd-cpu
python tools\benchmark\analyze.py --no-ocr --no-faces
```

注意单独跑 `run.py` 时**不会**替你生成零运动场，也不会检查 `runtime-snapshot` 是否在顶替
你的构建——这两件事只在 `amdguides.py` 里。

每臂产出 `runs/<clip>__<profile>__1/`：`result.json`（含 temporal 指标）、`output.mkv`、
`frames.md5`、`metrics.json`。

## 5. 自检（每臂必查，已自动化）

这四项由 `amdguides.py` 解析 worker 日志自动判定，任一不满足即在输出里打
`SELF-CHECK:` 并让退出码非 0——**不需要人工看日志**：

| # | 检查项 | 日志依据 | 不满足的含义 |
|---|---|---|---|
| 1 | 跑的是哪个臂 | `Neural backend:` 行的 `guides=` / `temporal=` / `zero-motion-test=` | 该臂渲染的不是它名字所说的配置，测的不是这个变量 |
| 2 | 引导每帧都进了 | 帧尾行的 `guide-frames=` 是否等于该 clip 帧数 | 部分帧退回常量，臂内不一致 |
| 3 | runtime 历史是否真的跑了 | 帧尾行的 `history=` | `off`/`absent`/`unreadable` 说明门禁没过，runtime 退回空域渲染，**不报错**——此时数字不能用来比较 |
| 4 | 往返一致性 | `amd-cpu` 与 `amd-cpu-replay` 的 `frames.md5` 序列摘要 | 不一致说明 stride 修复或 dump 通路有回归，先修再比 |

原始日志留在每个 run 目录的 `pass1-stdout.txt`，自检结论汇总在
`benchmark-work/amd-guides-ab/selfcheck.json`。

## 6. 判据（预登记，脚本自动读数）

下表由 `amdguides.py` 从 `analyze.py` 的 `metrics.json` 自动读出并打印，不需要人工查表。
阈值写在脚本里（`PSNR_TIE_DB = 0.1`：0.1 dB 以内算平，因为再低编码器自身的量化就能推动这个数）。

| 问题 | 对比 | 指标 | 结论规则 |
|---|---|---|---|
| held 帧噪声值多少 | C vs C'（still-hold） | per-pixel temporal sigma、PSNR 对源 | C' 显著更好 → 零化测试值得保留（与 NVIDIA mv-gate 同向） |
| 真运动在静止段 | C vs Z（still-hold） | 同上 | 差异 ≈ 0 → 静止段零场即最优 |
| 真运动在运动段 | C vs Z（depth-pan / real-game-motion） | flickerAdded、warp 误差、PSNR | C 更好且 >0.1 dB → 真引导有正收益；打平或更差 → 维持零运动架构 |
| 往返一致性 | C vs C-replay | framemd5 | 不一致 → stride 修复或 dump 通路有回归，先修再比 |

脚本最后打印总判定，规则与本文一致：**只有当"真运动在运动段可量地更好、且静止段不劣"
同时成立**，才考虑把真引导带入产品路径；否则维持现状（零场 + 宿主侧 P2.5 累积），
下一步的画质希望放在 **FidelityFX Optical Flow**（`dlss5-neural-amd` 的 `optical_flow.inc`，
需集成 FFX SDK，见 `.workbuddy/memory/2026-10-19.md` 的调研结论），本方案不包含它。

## 7. 已知风险

- **叠加层拖影**：真运动场下字幕/水印会被 runtime 历史当成背景重投影。C 臂的 output.mkv 重点看字幕边缘与转场后 3–5 帧。
- **CPU 估计器成本**：guide 生成在 `PrepareGuides`，不计入 `LastSubmitMs`，看 wall time；1080p30 预期每帧几毫秒量级。
- **切镜**：估计器的场景切会置 `guide.id.reset`，随帧传给 runtime 重置历史。若 Z/C 两臂切镜行为不同，先确认日志里 reset 帧号一致。
- **首轮 lease 6 帧预热**是 FFX OF 的行为，与本次无关；AMD runtime 的 priming 帧见 `HistoryStatus()`。
