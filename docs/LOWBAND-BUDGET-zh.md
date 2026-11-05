# 低频误差的构成与可消除性

结论先行：**PSNR 26–28 dB 里 97% 的误差不是"重建错了"，是"画面被压暗了"。
它能消除，但消除的比例就是 relight 被削弱的比例——两者是同一个数，没有中间地带。**

测量条件：`real-film-cuts` / `real-game-cuts` / `real-game-motion`，AMD LMXXF 后端，
参照臂 `ab-cpu`（内置块匹配 + 深度代理），单 pass。`vme_ab/lofreq.py` 复现。

## 1. 误差预算（老结论的延伸）

| 片段 | low 残差 rms | mid+high 残差 rms | low 占比 | 当前 PSNR | 低频完全匹配后的上限 |
|---|---|---|---|---|---|
| real-film-cuts | 9.73 | 1.29 | 98.3% | 27.29 | 45.93 |
| real-game-cuts | 9.38 | 1.70 | 96.8% | 28.38 | 43.54 |
| real-game-motion | 12.07 | 1.65 | 98.2% | 26.40 | 43.81 |

`mid` 与 `high` 合起来只有 1.3–1.7 个灰阶，换算 43.5–45.9 dB。**纹理层面这条 pass 已经做完了**，
所有可测差异都在低频。

## 2. 低频误差不是结构错误

对 low 带（σ>4 px）逐帧比较输出与源：

| 片段 | `corr(low_o, low_s)` | 能量比 | 块残差 vs 源亮度的相关 |
|---|---|---|---|
| real-film-cuts | 0.9891 | 0.937 | −0.44 |
| real-game-cuts | 0.9819 | 0.951 | −0.29 |
| real-game-motion | 0.9873 | 0.898 | −0.71 |

**结构相关性 0.98–0.99，能量比 0.90–0.95。** 低频的"形状"保持得近乎完好，只是幅度比源低 5–10%。
残差 9–12 个灰阶之所以显得大，是因为源的低频本身有约 54 个灰阶的幅度：54 × √(1−0.985²) ≈ 9.3。

能量比小于 1、且块残差与源亮度负相关（亮处压得更多），合起来是**局部对比度压缩**。
1D 色调曲线也确认了同一件事：暗部（0–95）偏移约 0，从 96 起单调压暗，亮部压掉 10–22 个灰阶，
三个片段曲线形状一致。

## 3. 能被简单模型吃掉多少

| 模型 | film | cuts | motion |
|---|---|---|---|
| 全局偏移（1 个数/帧） | 9% | 8% | 11% |
| 平面 a+bY+cX | 20% | 19% | 28% |
| 二次曲面（6 个数/帧） | 24% | 28% | 33% |
| 1D 色调曲线（64 段） | 34% | 19% | 34% |
| 最优增益 k\*（low 带） | +0.51 dB | +0.11 dB | +1.19 dB |

**只有 1/4 到 1/3 是"几个数就能写完"的**，剩下 2/3 是真正随画面内容变化的空间亮度重分配。
增益校正几乎救不回来——这也排除了"只是对比度没对上"这个解释。

## 4. 代价曲线：一条直线，没有便宜区间

把低频误差按比例 λ 混合回源，`MSE(λ) = (1−λ)²·MSE_low + MSE_rest`：

| λ（误差消除比例） | film-cuts | game-cuts | game-motion |
|---|---|---|---|
| 0%（现状） | 28.30 | 28.54 | 26.42 |
| 25% | 30.74 | 30.94 | 28.86 |
| 50% | 34.10 | 34.17 | 32.21 |
| 75% | 39.34 | 38.90 | 37.41 |
| 90% | 43.97 | 42.38 | 41.94 |
| 100% | 45.93 | 43.54 | 43.81 |

**每消除一半低频误差 ≈ +5.8 dB**（`−20·log10(1−λ)`）。三个片段斜率一致。

所以"97% 的误差能消除吗"的答案是：能，全部消除换取 +16 ~ 18 dB，但那正是关掉 relight。
不存在"改一点点就能拿几个 dB"的区间——想拿 3 dB，就得放弃 30% 的 relight 强度。

## 5. 因此指标要怎么写

- 对这条 pass，head-line PSNR **度量的是 relight 强度**，不是重建水平。
  它的 mid/high 部分（44 dB）才是"重建得多准"，而那一项几乎不动。
- 要判"改得对不对"，只能用分带 + `detail_ratio_high` + `k*`（见 `BENCHMARK-METRIC-VALIDITY-zh.md`）。
- 要判"relight 强度合不合适"，PSNR 帮不上忙——那是审美决定，只能看图。

## 6. 顺带查出的三个工具链缺陷

这三条影响了之前若干轮实验对"实验条件"的描述，需要修正。

1. **benchmark 的 `overrides` 在 AMD 后端全程空转。**
   `run.py --profile-file` 把 overrides 写进 runtime 的 `ReShade.ini` 的 `[RenoDX.DLSS5]` 段，
   但 AMD 后端从不加载该文件（`NeuralWorker.cpp:1288` 的注释明说 "asked a helper to configure a
   ReShade.ini that backend never loads"），worker 命令行也没有 override 入口。
   证据：`NRIntensity=0` 与 `NRIntensity=1` 渲染出的逐帧哈希完全相同（`cc2dfa30…`），
   worker 日志仍是 `Overall Intensity: requested=1`。
   **受影响**：`ABLATION` 里的 `tone-0` / `structure-0` / `intensity-0` / `preset-*` / `style-*`
   从未真正跑过；所有 `ab-*` 臂的 overrides 同样无效。
   **不受影响**：以 `mv=` / `mv=file:` 区分的那批臂——它们的差异来自引导场，哈希确实不同。

2. **`mv=cpu,depth=0` 不生效。** `ParseSpec` 正确解析出 `controls.depth=false`，但产物与
   `depth=1` 逐帧相同。待查（另注：`baseline` / `depth-off` 两个内置 profile 默认走 NVIDIA 后端，
   在 AMD 机器上直接失败于 "Feature 18 was not created"）。

3. **`ParseGuideControls` 与 `ParseSpec` 两套解析并存。** 前者只认 `mv=0|1,depth=0|1`，
   后者还认 `mv=cpu` / `file:`。benchmark 走后者，player 走前者，两边的"合法写法"不同。

要动 relight 强度做实验，得先修第 1 条（把 overrides 接到 AMD 的通道上），否则一切都是空跑。
