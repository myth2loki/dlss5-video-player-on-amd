# 神经渲染速度优化方案（AMD / Mochizuki 路径）

> 2026-11-06 实测。所有数字来自本机 RX 9070 XT、`run-fg` 当前二进制、
> Mochizuki 1.14 系 DLL、RenoDX 路由开、`[RenoDX]` 2 遍、`networkModel=small`、
> `int4=0`、100% 处理缩放。**所有测速都在无其它播放器占用 GPU 的窗口内完成。**

## 0. 结论先行

- **之前那批数字被另一个播放器的 GPU 争用污染了**：1920x800 从 18.7–19.3 ms/帧
  降到 **15.36–15.67 ms/帧**（−18%），1080p 从 24.66 降到 **20.27 ms/帧**（−18%）。
  差值几乎全部落在 `network_ms`（12.78→8.35、10.0→6.4），CPU 侧不变，符合 GPU 被抢的特征。
- **当前播放并不卡**：稳态 15.54 ms/帧 @1920x800 ≈ 64 fps 渲染吞吐，对 23.976 fps 源是
  **2.7× 实时**；播放预算 41.7 ms/帧，余量 26 ms。`Playback health` 也是 presented≈24 fps / dropped 0。
  所以提速的目标应是**缩短导出时长、提高高分辨率余量、降功耗**，不是修播放卡顿。
- **唯一已实测、立即可用、且真的生效的提速旋钮是 `[RenoDX] RenoPasses`（2→1 = −20%）**，
  但它是画质换速度。其余候选里有两个是"能选但什么也不做"，一个是 AMD 明确不支持。
- **最大的未开发空间在 host 侧流水线**：runtime 自报 `network_ms` 只占每帧 41%，
  剩下 ~9 ms 里包含 RenoDX 两遍、颜色转换、上传/回读和两次全队列排空。
  **这部分到底有多少是纯同步空转、多少是真 GPU 工作，目前没有打点，先补度量再动代码。**

## 1. 干净基线

固定素材 `F:/dlss5-video/tmp/shot15.mp4`（1920x800 / 23.976 fps / 360 帧），
`--render --stages nr --neural-backend amd --encode high`。环境前后均确认无
`DLSSVideoPlayer.exe` / `NeuralWorker.exe` 之外的播放器，GPU 引擎无 >1% 占用。

| 运行 | 素材 | total | submit | network p50 | guide | loop |
|---|---|---:|---:|---:|---:|---:|
| clean_a | shot15 (360f) | 15.36 | 14.58 | 6.34 | 1.14 | 16.00 |
| clean_b | final_cur (365f) | 15.60 | 14.77 | 6.37 | 1.13 | 16.26 |
| clean_c | ab15_base (360f) | 15.67 | 14.92 | 6.48 | 1.18 | 16.35 |
| arm_base_repeat | shot15 (360f) | 15.54 | 14.81 | 6.44 | 1.17 | 16.19 |
| **中位** | | **15.54** | **14.77** | **6.42** | **1.16** | **16.20** |
| clean_1080 | shot15 放大 1920x1080 (360f) | 20.27 | 19.55 | 8.35 | 1.56 | 21.20 |

单位 ms/帧。spread 仅 **0.31 ms (2.0%)**；`arm_base_repeat` 与 `clean_a` 输出逐字节相同
（11,939,779 B）——链路确定性，所以**补跑不产生新信息**，方差只来自素材/镜头。

其它阶段恒定可忽略：`source 0.35`、`resolve wait 0.19`、`encoder back pressure 0.2`、
`write 0.05–0.4`、`loop unaccounted 0.15`。`resolve wait(copy not hidden)` 恒为 0，
说明回读已被异步 ring 隐藏，不是瓶颈。

### 与污染数据的对照

| 几何 | 争用时 | 干净 | 差 | 其中 network |
|---|---:|---:|---:|---:|
| 1920x800 / 360f（同素材，严格可比） | 18.71–19.34 | 15.36–15.67 | **−3.3~3.8 ms (−18~20%)** | 10.0 → 6.4 |
| 1920x1080 | 24.66（live 7193 帧长片） | 20.27（360 帧离线） | −4.39 ms (−18%) | 12.78 → 8.35 |

⚠️ 1080p 那组不是严格同素材同路径（污染值是边渲边播的 7193 帧长片），只作量级参考；
**800p 那组才是同素材同几何同帧数的严格对照**。

### 成本模型（用于定价，不外推到未测配置）

```
2 遍时： total(MP)  ≈ 2.1 + 8.8 × MP          network(MP) ≈ 1.0 + 3.5 × MP
                                              非 network   ≈ 1.1 + 5.3 × MP
1920x800 (1.536 MP)、P 遍： total ≈ 9.5 + 3.0 × P   ms/帧
```
像素每 +1 MP 约 +8.8 ms；RenoDX 每 +1 遍约 +2.9 ms（其中 network 约 +1.5 ms）。

## 2. 提速杠杆实测（同素材同几何，全部真渲染）

| 臂 | total | Δ vs 基线 | network p50 | 输出字节 | 判定 |
|---|---:|---:|---:|---:|---|
| 基线（RenoPasses=2） | 15.54 | — | 6.44 | 11,939,779 | — |
| **RenoPasses=1** | **12.38** | **−3.16 (−20%)** | 3.01 | 9,444,271 | ✅ 真生效 |
| RenoPasses=4 | 21.05 | +5.51 (+35%) | 12.10 | 13,508,181 | ✅ 线性 ≈2.9 ms/遍 |
| `[AmdNeural] MochizukiInt4=1` | 15.54 | 0 | 6.44 | 11,939,779 | ❌ **被 runtime 静默拒绝**（非接线问题，见 2.1） |
| `--processing-scale 50` | — | — | — | — | ❌ **AMD 硬拒绝** |
| `--passes 2`（神经面板） | 15.54 | 0 | 6.46 | 11,939,779 | ❌ **只改 capacity** |

证据：
- INT4 臂的 runtime 日志仍是 `submitted network #1 model=small int4=0 renodx=1 passes=2`
  （07:48:10.706），图仍按 `dlssnr-small.bin` 建（`graph build 1.29s`），输出逐字节相同。
  ⚠️ **2026-11-06 追加查证推翻了最初"离线装配没送进 runtime"的判断**：接线是全通的，
  真正原因是 `[AmdNeural] MochizukiNetwork=1`（small）与 INT4 互斥。详见 2.1。
- `--processing-scale 50` 直接 exit=4：`The AMD backend runs the model at the source size;
  a processing scale below 100 is not implemented for it.`
  （`src/OfflineNeuralRenderer.cpp:3976`、`src/main.cpp:3224`）。
  `src/UpscalingPolicy.h:196-198` 那组 14.8/17.3/22.0 fps 是 **RTX 4080 SUPER** 的数字，不能套到 AMD。
- `--passes 2` 的 runtime 行变成 `requested ... passes=2 capacity=2`（基线 `passes=1 capacity=1`），
  但实际提交仍是 `submitted ... passes=2 styles=[0,0]`、耗时与画面不变。
  ⇒ RenoDX 路由下**实际遍数的唯一 owner 是 `reno_passes`**；神经面板 `Passes` 只改网络容量。
  这正是 11-05 记下的"两个 owner 未统一"，现在有了实测证据。

### 2.1 INT4 为什么"设了等于没设"（2026-11-06 追加查证）

**结论：不是接线 bug，是配置组合不兼容 + runtime 静默降级 + host 无回显。**

整条接线是通的，逐段可查：

| 段 | 位置 |
|---|---|
| ini → `MochizukiOptions::int4Mixed` | `src/main.cpp:2585`（`ReadSavedMochizukiOptions`）、离线装配 `src/main.cpp:17129` |
| → worker payload 串 `int4=%d` | `src/NeuralRenderTypes.h:529`（`FormatMochizukiOptions`）、解析 `:670`、回填 `src/NeuralWorker.cpp:1862` |
| → runtime ABI `int4_mixed` | `src/AmdLmxxfEvaluator.cpp:1610`，经 `SetControls` |
| → `SessionControls::int4` | runtime `MochizukiNrRuntime.cpp:747`（`Sanitise`）、`:6958` 读取 |

**断点在最后一步**，runtime 源码
`OptiScaler/.../mochizuki_runtime/MochizukiNrRuntime.cpp:7016`：

```cpp
key.int4 = int4 && networkModel == 0; // INT4 records/weights describe the original model only.
```

`dlssnr-int4.bin` 只对应原版 `dlssnr.bin`，small（`dlssnr-small.bin`）没有 INT4 记录/权重，
所以请求被**有意**清零。当前 ini 是 `MochizukiNetwork=1`，于是必然 `int4=0`。

**三臂对照实验**（同一 360 帧素材、同几何、干净环境、`--render --stages nr --encode high`；
每次都真渲染，runtime 日志有新的 `graph build` / `submitted network`）：

| 臂 | ini | runtime 实际 | total | network p50 | 输出字节 |
|---|---|---|---:|---:|---:|
| A | `Int4=1, Network=1` | `model=small int4=0` | 16.53 | 6.42 | 11,939,779 |
| B | `Int4=1, Network=0` | `model=original int4=1` | 19.02 | 9.08 | 12,129,101 |
| C | `Int4=0, Network=0` | `model=original int4=0` | 19.32 | 10.26 | 12,797,151 |

臂 B 出现了 `requested.model=original(int4 requested)` → `[nr] int4_mixed network (switch)`
→ `submitted ... int4=1`，证明请求一路送到了网络构建；臂 A 输出与基线逐字节相同，
证明配置确实没变。（A 的 total 16.53 比基线 15.54 高 1 ms 是当次运行波动，
同设置重跑的 spread 见第 1 节。）

**是 bug 吗？分三层看：**

1. runtime 那行是**设计约束**，不是缺陷——权重资产决定它只能这么写。
2. **host 侧是真 bug**：ini / 面板允许 `Int4=1` 与 `Network=1` 同时存在，没有互斥、
   没有禁用、没有提示；runtime 的 `MochizukiNrInfo` 明明导出了 `int4_requested` 与
   `int4_effective`（`MochizukiNrRuntime.cpp:7617/7625`），**host 里一处都没读**
   （`grep -rn "int4_effective\|int4_requested" src/` 为空）。所以"请求了但没生效"
   在日志、面板、自检里全都看不见。这是"能选但什么也不做"的又一例，但根因是
   **下游拒绝、上游不回显**，和前几例（字段没进白名单 / 装配 early-return）不同。
3. 附带副作用：`FormatMochizukiOptions` 把 `int4=1` 写进缓存 key，于是 small 模型下
   开/关 INT4 会生成**两份内容逐字节相同**的缓存条目。

**修复建议（正确性，不是提速）**：① 最小改动——`ReadSavedMochizukiOptions` 在
`networkModel != 0` 时把 `int4Mixed` 归一为 false 并记一行日志，面板同步禁用该项；
② 根治——evaluator 读 `MochizukiNrGetInfo`，`int4_requested != int4_effective`
时打醒目日志并在 UI 标注"已降级"；③ 缓存 key 用归一后的值，避免双份条目。

⚠️ **别把 INT4 当提速项**：它只在 original 模型生效，而 original 比当前 small
慢约 3.8 ms/帧（19.32 vs 15.54），INT4 只省 1.18 ms，净亏 ~2.6 ms/帧。
历史 `mochizuki_nr.1.log` 里 09:21 那批 `int4=1` 生效记录全部是 `renodx=0` +
original 模型，与当前 RenoDX + small 配置不是一回事。

### 2.2 host 侧修复（2026-11-06 已实施，runtime 不动）

按 2.1 的三条建议全部落地。`MochizukiNrRuntime.cpp:7016` 那行是资产约束，保持原样。

| 文件 | 改动 |
|---|---|
| `src/NeuralRenderTypes.h` | `MochizukiOptions::NormaliseInt4()` 作为唯一归一入口（`networkModel!=0 ⇒ int4Mixed=false`，返回是否丢弃了请求）；`FormatMochizukiOptions` 改**按值传参**并在序列化前归一，让 wire 与缓存 key 同源；`ParseMochizukiOptions` 入口归一，兼容旧 host 的 payload |
| `src/main.cpp` | `ReadSavedMochizukiOptions` 归一并写播放器 LOG；新增 `SyncMochizukiInt4()`，small 下禁用并取消勾选 INT4；`ReadNeuralSettingControls` 读完两个控件后归一并刷新；WM_COMMAND 对 disabled 复选框不再翻转 |
| `src/OfflineNeuralRenderer.cpp` | `RunAmdJob` 装配 config 时最后一道归一 + worker/player LOG。⚠️ 离线路径把 evaluator 的 log sink 清空（`:4048 config.log={}`），所以**回显必须放在这里和 main.cpp**，只加在 evaluator 里等于没加 |
| `src/AmdLmxxfEvaluator.cpp` | `ApplyMochizukiControls` 按值取 config 并归一；`WaitForMochizukiNetwork` 读 `MochizukiNrGetInfo` 的 `int4_requested/effective`、`network_requested/effective`、`renodx_effective`，requested≠effective 时打 `<-- REQUEST DROPPED BY RUNTIME`（只覆盖有 log sink 的直连/smoke 路径） |
| `src/Localization.h` | INT4 tooltip 补「仅 310.8.0 原版网络有 INT4 权重，small 下不可用」 |
| `tests/PolicyTests.cpp` | 新增 `mochizuki_int4_normalise_test`：合法组合往返、非法组合归一、Format 归一、旧 payload 入口归一、幂等；原往返测试不再用不可渲染的组合 |

**验证**（run-fg 成套换新二进制，脚本 `/f/tmp/speed/int4_verify.sh`、`int4_recheck.sh`）：

- BUILD EXIT=0；PolicyTests **425 cases 全过**（+1）；UI 回归 66 cases / 3160 assertions / **22 failures**，与 `build-ci`(10-15)、`build-mz`(10-30) 旧二进制的 22 failures / 3 cases **完全一致**，无新增失败。
- `Int4=1, Network=1`：`DLSSVideoPlayer.log` 打出 `Mochizuki settings: INT4 was requested with the small network...`；runtime `submitted network #1 model=small int4=0`；输出与 plain small **像素哈希一致**（`7105dced...`）。
- `Int4=1, Network=0`：runtime 仍 `model=original int4=1` + `[nr] int4_mixed network (switch)`，输出与修复前逐字节相同（12,129,101 B）——合法组合没被误伤。
- 缓存目录里含 `int4=1` 的 `neural-settings.ini` 数量渲染前后都是 2（均为旧实验遗留），**没有新增双份 key**。

⚠️ **一次假警报，值得记住**：修复后首轮 `Int4=1, Network=1` 输出 12,018,234 B，与 plain small 的 11,939,779 B 不同，345 帧从第 16 帧起全不同。runtime 日志显示那次 `encoder back pressure 1.46 ms`（正常 0.02–0.22），并在 **frame=16 多了一次 `history reset (timeGap=1)`**，temporal 历史从此分叉；同配置复跑两次都回到 `7105...`，reset 序列 1/172/300 与基线一致。⇒ 不是配置差异，是新二进制冷启动的时序抖动。**「链路确定性」有这个例外**：判 A/B 时先看 `encoder back pressure` 与 `history reset` 帧号，别把首次运行的哈希差异直接当回归。

## 3. 优化项（按性价比排序）

### P0 · 先补打点，别急着改同步（零画质代价，必须先做）

`network_ms` **不是 GPU 总时间**，它只覆盖网络 kernel。RenoDX 的两遍 stage、颜色转换、
上传/回读 copy 都藏在 `submit` 的等待里。当前每帧链路是彻底串行：

- `src/AmdLmxxfEvaluator.cpp:3614` 和 `:3638`（lease 路径）两次 `WaitForGpu()`，
  默认路径 `:3784` / `:3847` 也是两次；`WaitForGpu()`（`:2030`）是 `Signal + WaitForSingleObject`
  的**整个队列排空**，不是等单帧 fence。
- runtime 侧日志 `split enqueue ... in_flight=0 physical=0` 也说明没有帧间重叠。
- 但 `src/OfflineNeuralRenderer.cpp:1426` 的 `ResolveOldest` 已经是异步回读 ring。

⇒ **要做的**：在 submit 内加分段 fence / GPU timestamp，把 14.77 ms 拆成
upload / EnqueueHip 前 / network / RenoDX stage / copy / readback / 纯 CPU。
只有知道那 ~7–9 ms 里 GPU 与 CPU 各占多少，才能判断"去掉排空"值不值得做。

**刻度**：`RenoPasses=1` −3.16 ms、`=4` +5.51 ms 就是现成的尺子；任何优化必须能在这把尺子上
被分辨（>0.3 ms）。上机前先跑这两个已知好/已知坏对照，分不开就说明打点没用。

### P1 · 零画质代价，代码级

1. **帧间流水**（收益待 P0 定价）：把"每帧两次全队列排空"改成"每帧一个 fence 值 +
   只等最老一帧"，保留 2 帧在飞。风险点必须逐个守住：
   - ABI 契约：`EnqueueHip` 必须紧跟 producer 提交，中间不能有 fence wait / Flush
     （`AmdLmxxfEvaluator.cpp:3600` 附近注释）；
   - `EndRecording` 的证书要 host 自己的 tail Signal（`:3620-3635`）；
   - history chain 按帧顺序，`reset` 语义不能乱；
   - 回读 ring 的身份匹配 `OfflineNeuralRenderer.cpp:1426` 的 `SameSource` 断言。
   - 若 P0 显示那 7–9 ms 主要是 RenoDX 的 GPU 工作，则此项收益接近 0，直接放弃，不要硬改。
2. **去掉每帧无谓查询**（预计 0.1–0.5 ms，零风险）：`SampleLocalVram()` 每帧一次
   `IDXGIAdapter3::QueryVideoMemoryInfo`（`:3963`）、`RefreshHistoryStatus()` 每帧一次
   `runtime_.Status()`（`:3390`）。改成每 30 帧采样一次即可，回执里的峰值/驻留采样
   （`NeuralRenderTypes.h:845` 的 `NeuralRenderTiming`）语义不变。
3. **guide 1.16 ms**：CPU 运动估计。`vme_ab` 已证这个网络对 MV 极不敏感
   （`mv=0` 只掉 0.02–0.12 dB），但那是在另一条 benchmark 通道，且项目已决定 VME 不接。
   要省这一毫秒必须按镜头验画面，别直接关；列可选，不列默认。

### P2 · 画质换速度（已实测，立即可用）

- **`[RenoDX] RenoPasses` 2→1**：−3.16 ms/帧（−20%），1080p 预计 ~−4 ms。
  代价是实打实的：第 2 遍的 natural 风格、拖尾抑制、跨遍稳定（当前
  `detail_stability=Always`）全部消失；11-05 实测"跟随 vs 不跟随第 1 遍"PSNR 30.8 dB，
  说明这一遍真的在改画面。
- **建议做成预设而非改默认值**：「快速预览 / 批量导出」= 1 遍，「成片」= 2 遍（当前 kStart）。
  默认仍保持 2 遍，别动用户已经调好的最优配置。
- 反向 `RenoPasses=4` 每帧 +5.5 ms，画质收益未证，**不要默认开**；面板 5..30 的档位在
  runtime 里还要靠 `reno_unlock_passes` 才生效，别被数字迷惑。

### P3 · 冷启动 / 交互（不是稳态吞吐）

每次 Apply 重建网络 `graph build 1.24–1.30 s`：`plan 0.58 + weights-unpack 0.28 +
noise-field 0.18 + adapters 0.08`，`pipelines 0.01`（5.9 MB pipeline cache 已生效，
冷启动那次是 4.93 s）。加首帧预热就是用户看到的"2–3 秒回原视频"。
可优化：只改 per-pass look、不改 `reno_passes` / 几何 / scale 时不要重建 network 容量。
pipeline cache 别动。

### P4 · 明确不要做

- ❌ `--processing-scale 75/50`：AMD 不支持，别写进方案。
- ❌ 把 `MochizukiInt4` 当提速项：只对 original 模型生效，而 original 比 small 慢 ~3.8 ms/帧，
  INT4 只省 1.18 ms，净亏。修互斥/回显属正确性问题，不是性能方案，见 2.1。
- ❌ 神经面板 `Passes`：RenoDX 路由下不改实际遍数，改了没效果。
- ❌ 靠 repeat 补跑降方差：链路确定性，三次得到逐字节相同文件。方差在镜头之间。
- ❌ VME / 3×3 补洞：`vme_ab` 四轮已结论"不接"，净 +1~2 ms 且 flicker 变差。
- ❌ 用 head-line PSNR 判画质：低频 relight 占残差 97%，`RenoPasses=1` 的"更贴源"
  只是 relight 变淡。要判用 `detail_ratio_high` + 按镜头同号（≥2/3 且每段 >0.05 dB）+ 闪烁。
- ⚠️ benchmark 的 `overrides` 在 AMD 后端曾全程空转（写进 `ReShade.ini`，worker 不读），
  做 A/B 前先确认该通道真的送达；player 侧 `[RenoDX]` → `MochizukiOptions.overrides`
  这条是通的，但要用 runtime 日志 `submitted network ... passes=N` 核实。

## 4. 验收方法

- 固定素材 + 固定几何 + 固定设置；**A/B 必须成套换二进制**（evaluator 在 `NeuralWorker.exe` 里）。
- 读三处日志：`run-fg/neural-runtime/NeuralWorker.log` 的
  `Neural export stage cost per frame` / `Render stage ...`；
  `amd-neural-runtime/mochizuki_nr.log` 的 `session end: network` / `submitted network`；
  `run-fg/DLSSVideoPlayer.log` 的 `Measured neural render pace` / `Playback health`。
- 判据：total 变化 **>0.3 ms 且 ≥3 次同号**（当前噪声 2%，0.3 ms 约 2σ）。
  只用 p50/mean，不用整片 wall time（会被 1.3 s 建网和首帧预热污染）。
- 画质改动另按镜头切窗判，整片均值不算数。
- 自检报告写"必须看到哪一行"，不写"跑通即可"。

## 5. 复现

```bash
bash /f/tmp/speed_clean.sh     # 干净基线：4 次，含 1080p 几何
bash /f/tmp/speed_arms.sh      # 6 个杠杆臂 + 退化刻度
```
产物 `F:/tmp/speed/*.mkv`、`*.stdout.log`；两个脚本都会**备份并还原**
`run-fg/DLSSVideoPlayer.ini`（sha 前后一致 `0544e6e0e14c63e2…`）。
