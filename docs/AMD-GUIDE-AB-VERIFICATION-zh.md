# AMD 真引导 A/B 验证方案（无需人工介入）

状态：**待验证**。代码已改完（本机 macOS 无法编译），在 Win11 + RX 9070 XT 上按本文执行。
对应改动：`AmdLmxxfEvaluator.h/.cpp`（内存引导 + 运动场跨距修复）、`OfflineNeuralRenderer.cpp`（AMD 路径撤三条拒绝、接 CPU 估计器）、`tools/benchmark/guidefiles.py`（`zero` 子命令）、`tools/benchmark/amd-guides.profile.json`（四个臂）。

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

| profile | guides | 说明 |
|---|---|---|
| `amd-cpu` | `mv=cpu,depth=1` + `--guide-dump` | **C 臂**：真运动 + 深度代理，lease 生命周期，dump 供 Z 臂与往返证明用 |
| `amd-cpu-nzmt` | C 臂 + `--zero-motion-test 1` | **C' 臂**：唯一差异是静止帧零化——量化块匹配在 held 帧上的噪声危害 |
| `amd-zmv` | `mv=file:zero/mv,depth=file:cpu/depth` | **Z 臂**：零运动场 + C 臂同款深度 → 与 C 臂唯一差异是运动场 |
| `amd-cpu-replay` | `mv=file:cpu/mv,depth=file:cpu/depth` | **往返证明**：C 臂 dump 回喂，必须与 C 臂逐位一致（验证 stride 修复） |

全部走 `--neural-backend amd-lmxxf`；runtime 目录用环境变量 `DLSS5_AMD_RUNTIME_DIR` 指定。**执行顺序：amd-cpu →（其余任意，但 amd-zmv / amd-cpu-replay 依赖 amd-cpu 的 dump）。**

## 4. 执行步骤（Win11 + 9070 XT）

```bat
:: 0) 构建 Release（与平时相同），确认 NeuralWorker.exe 存在
:: 1) 语料（已有 corpus 跳过；否则按 tools/benchmark/README.md 构建）
python tools\benchmark\corpus.py
:: 2) 零运动场（帧数 = 目标 clip 的帧数；深度不写——Z 臂用 C 臂 dump 的深度）
python tools\benchmark\guidefiles.py zero --frames 300 --out %WORK%\guides\still-hold\zero
:: 3) 设 runtime
set DLSS5_AMD_RUNTIME_DIR=<amd-neural-runtime 目录>
:: 4) 跑四个臂 × 三个 clip（still-hold / depth-pan / real-game-motion）
python tools\benchmark\run.py --profile-file tools\benchmark\amd-guides.profile.json ^
    --clips still-hold depth-pan real-game-motion --profiles amd-cpu amd-cpu-nzmt amd-zmv amd-cpu-replay
:: （run.py 的参数名以 --help 为准；核心是 profile 文件 + clip 列表）
:: 5) 往返证明判定
python tools\benchmark\guidefiles.py prove --clips depth-pan real-game-motion --profile-file tools\benchmark\amd-guides.profile.json
```

每臂自动产出 `runs/<clip>__<profile>__1/`：`result.json`（含 temporal 指标）、`output.mkv`、`frames.md5`。

## 5. 自检（每臂必查，缺一即该臂无效）

1. worker 日志一行：`guides=motion:estimator(cpu block matcher)` / `depth:…` / `zero-motion-test=on|off` / `temporal=on`。
2. `GuideFrames() == 帧数`：引导真的每帧都进了（不是常量路径）。
3. `history=` 的帧占比：`active` 为正常；大面积 `off` 说明 runtime 门禁没过，先查日志里的格式/互指检查，不要继续比。
4. 往返臂 `frames.md5` 与 `amd-cpu` **逐位一致**（`guidefiles.py compare` 或 prove 命令）。

## 6. 判据（预登记，跑完照表读数）

| 问题 | 对比 | 指标 | 结论规则 |
|---|---|---|---|
| held 帧噪声值多少 | C vs C'（still-hold） | per-pixel temporal sigma、PSNR 对源 | C' 显著更好 → 零化测试值得保留（与 NVIDIA mv-gate 同向） |
| 真运动在静止段 | C vs Z（still-hold） | 同上 | 差异 ≈ 0 → 静止段零场即最优 |
| 真运动在运动段 | C vs Z（depth-pan / real-game-motion） | flickerAdded、warp 误差、PSNR（±0.1 dB 内算平） | C 更好且 >0.1 dB → 真引导有正收益；打平或更差 → 维持零运动架构 |
| 往返一致性 | C vs C-replay | framemd5 | 不一致 → stride 修复或 dump 通路有回归，先修再比 |

**总判定**：只有当"真运动在运动段可量地更好、且静止段不劣"同时成立，才考虑把真引导带入产品路径；否则维持现状（零场 + 宿主侧 P2.5 累积），下一步的画质希望放在 **FidelityFX Optical Flow**（`dlss5-neural-amd` 的 `optical_flow.inc`，需集成 FFX SDK，见 `.workbuddy/memory/2026-10-19.md` 的调研结论），本方案不包含它。

## 7. 已知风险

- **叠加层拖影**：真运动场下字幕/水印会被 runtime 历史当成背景重投影。C 臂的 output.mkv 重点看字幕边缘与转场后 3–5 帧。
- **CPU 估计器成本**：guide 生成在 `PrepareGuides`，不计入 `LastSubmitMs`，看 wall time；1080p30 预期每帧几毫秒量级。
- **切镜**：估计器的场景切会置 `guide.id.reset`，随帧传给 runtime 重置历史。若 Z/C 两臂切镜行为不同，先确认日志里 reset 帧号一致。
- **首轮 lease 6 帧预热**是 FFX OF 的行为，与本次无关；AMD runtime 的 priming 帧见 `HistoryStatus()`。
