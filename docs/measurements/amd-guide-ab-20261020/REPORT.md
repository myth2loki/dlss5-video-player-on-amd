# AMD 真引导 A/B：真运动场 vs 零场 — 2026 年 10 月 20 日，RX 9070 XT

**结论：不采纳真运动场，维持零场架构。** AMD 路径的 runtime 历史第一次吃到真
运动场（CPU 块匹配器，每帧内存递交），在三个 clip × 四个受控臂上比零场：静止段
两者逐位相同，运动段一胜一负且净负，不满足预登记的采纳条件。下一步的画质杠杆是
FidelityFX Optical Flow，不是这个场。

复现只需一条命令（脚本内部完成零场生成、依赖顺序、四项自检、往返证明与判据读数）：

```bat
set DLSS_BENCHMARK_BUILD=F:\dlss5-video\dlss5-video-amd\build-ninja
python tools\benchmark\amdguides.py --amd-runtime F:\dlss5-video\dlss5-video-amd\amd-neural-runtime
```

方法、臂的设计与判据见 [AMD 真引导 A/B 验证方案](../../AMD-GUIDE-AB-VERIFICATION-zh.md)，
结论已并入[集成计划 §5.3 末段](../../AMD-LMXXF-INTEGRATION-PLAN-REV2-zh.md)。

## 要回答的问题

> 在视频上，真运动场比零运动场好多少？值不值得为它承担重投影错误的风险？

在此之前 AMD 路径只吃过两种引导：常量（零运动 + 深度 1.0），或 `mv=file:` 的离线
文件。已有证据都指向"收益很小"：NVIDIA 侧零运动反而更好
（[neural-mv-gate-20260924](../neural-mv-gate-20260924/REPORT.md)），runtime 时域的
融合权重只有 0.081（[集成计划 §5.3](../../AMD-LMXXF-INTEGRATION-PLAN-REV2-zh.md)），
按此算出的收益上界是噪声标准差下降 7.8%，而那个上界还落在最有利的静止内容上。

这次补上的是缺的那一半：**真实运动场在真实内容上的读数**。

## 一个会让"真运动臂"整体失效的前提

`AmdLmxxfEvaluator::UploadGuides` 的文件读取把 3 通道运动 PFM 重采样到全帧后，用
**2 通道跨距**写进 R32G32 纹理——每个 y 落到下一个像素的 x 上，且 1/3 的纹素读到
填充 0。修复之前所有经 `mv=file:` 喂真运动场的 AMD 测量，其运动场都是错排的。

零场不受影响（零怎么排都是零），所以历史零场结论仍然成立；但**历史"真场"数据不
再可比**。本次测量用的构建包含此修复，并由下方的往返证明直接验证。

## 四个臂

| profile | guides | dump | 作用 |
|---|---|---|---|
| `amd-cpu` | `mv=cpu,depth=1` | `{clip}/cpu/` | **C 臂**：真运动 + 深度代理，lease 生命周期，dump 供 Z 臂与往返用 |
| `amd-cpu-nzmt` | C 臂 + `--zero-motion-test 1` | `{clip}/cpu-nzmt/` | **C′ 臂**：唯一差异是静止帧零化 |
| `amd-zmv` | `mv=file:{clip}/zero/mv,depth=file:{clip}/cpu/depth` | — | **Z 臂**：零运动场 + C 臂同款深度，与 C 臂唯一差异是运动场 |
| `amd-cpu-replay` | `mv=file:{clip}/cpu/mv,depth=file:{clip}/cpu/depth` | — | **往返证明**：C 臂 dump 回喂，须与 C 臂逐位一致 |

全部走 `--neural-backend amd-lmxxf`，执行顺序由脚本固定（`amd-cpu` 必须最先）。

## 结果

预登记阈值：PSNR 相差 0.1 dB 以内算平（再低编码器自身的量化就能推动这个数）。

| clip | 帧数 | PSNR C（真场） | PSNR Z（零场） | Δ PSNR | sigma C | sigma Z | 读数 |
|---|---|---|---|---|---|---|---|
| still-hold | 60 | 29.30 | 29.30 | 0.000 | 0.7678 | 0.7678 | 逐位相同：静止帧上估计器产出的本就是零场 |
| depth-pan | 90 | 21.23 | 22.12 | **−0.892** | 62.7350 | 63.9112 | 零场胜 |
| real-game-motion | 76 | 32.12 | 31.95 | +0.166 | 20.9550 | 21.1211 | 真场胜，刚过平带 |

C′（静止帧零化）与 C 在三个 clip 上**全部逐位相同**——估计器在 held 帧上没有发明
噪声，零化测试在这里无事可做。这与 NVIDIA 侧的 mv-gate 结论方向相反但不相矛盾：
那边的噪声来自硬件光流对两帧相同的输入仍报出非零场，这里的 CPU 块匹配器对相同
输入本来就报零。

**总判定**：采纳条件是"运动段可量地更好**且**静止段不劣"同时成立。实际运动段一胜
一负（−0.892 与 +0.166，净 −0.726），因此不采纳，维持零场（纯空间）架构。

## 往返证明（同时验证了 stride 修复）

`amd-cpu` 与 `amd-cpu-replay` 的帧摘要逐位一致，三个 clip 全部如此：

| clip | 摘要（C 与 replay 相同） |
|---|---|
| still-hold | `484a60842df49593` |
| depth-pan | `758948fd5b5db3a4` |
| real-game-motion | `8a8bd6d002c87a8e` |

这同时证明两件事：dump 通路无损，以及 `UploadGuides` 的 3→2 通道跨距修复成立——
修复前这条必然失败。

**一处交叉印证**：still-hold 上 C / C′ / Z / replay **四个臂的摘要全部相同**
（`484a6084…`），而运动段 Z 与 C 明显不同（depth-pan `f84c37a9…`、
real-game-motion `41007d6b…`）。这是"静止帧上真场即零场"的直接证据，判据表里
still-hold 的 Δ=0 不是测不出差异，而是差异确实为零。

## 测量条件与可复现性

- GPU：AMD Radeon RX 9070 XT，驱动 32.0.31041.1004；runtime 包
  `F:\dlss5-video\dlss5-video-amd\amd-neural-runtime`（`LmxxfNrRuntime.dll` 1,653,536 B）
- 每臂 wall 10–15 s，gpu p50 ≈ 9.6 ms，峰值本地显存 1502 MiB
- 分析网格 160x90（30 fps 1080p 源）
- **两轮独立运行逐位一致**：换 runtime 路径（`F:\amd-neural-runtime` 与仓库内
  `amd-neural-runtime`）重跑，三 clip 的摘要与全部指标相同，退出码均为 0
- 自检：12 臂全部 `history=active`、guide-frames 与各自 clip 帧数一致、`failures=[]`

## 仍未验证

- 位移向量的**符号**约定仍是照 `GuideFiles.h` / `TemporalGuides` 两个头的声明，
  本次没有独立验证 runtime 重投影的符号。
- 真运动场只测了 CPU 块匹配器这一个来源。**FidelityFX Optical Flow 未测**——它需
  要先集成 FFX SDK，是本方案的下一步而不是一部分。
- depth-pan 的 PSNR 绝对值偏低（21–22 dB）是该合成 clip 强 relight 的预期结果；
  比较用的是同 clip 内的 Δ，不受影响。

## 对产品路径的影响

无。播放器 CLI 不暴露 `--guides`，产品路径取不到 `mv=cpu`，默认
`MotionSource::Estimator` 在 AMD 上落到常量，仍是零场纯空间渲染。这条现在是
**评估后的决定**而非缺件，见
[集成计划 阶段 C「仍然缺的」](../../AMD-LMXXF-INTEGRATION-PLAN-REV2-zh.md)。
