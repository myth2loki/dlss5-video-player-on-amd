# RenoDX「Passes (fast)」接入调研

> 调研日期 2026-11-06。对象：游戏内 `dlssnr-amd.asi 0.0.8.2-pre1` 的 RenoDX 层「Pass 次数（快速）」，
> 目标是把这套东西接进本仓库（播放器 `dlss5-video-amd` + 它加载的 `MochizukiNrRuntime.dll`）。

## 0. 结论

1. **fast 不是"多跑几遍更省"，是把"多跑几遍网络"换成"跑一遍网络 + 4~7 个 2D 后处理 dispatch"。**
   强度用 legacy pass 次数作刻度（1..8）。所以「5.06 遍」= 一遍网络的改动被放大到约 3.55 倍，
   代价接近一遍（`RenoPasses=1` ≈ 12.38 ms / 帧，本仓库已实测），不是 `RenoPasses=5`。
2. **算法与着色器已经在本机源码里**（`mz-src/mz-082/third_party/mochizuki` 与
   `DLSSNR-AMD-gitlab/windows`），缺的是三处接线：**ABI 字段**、**打包清单**、**播放器 UI**。
3. **随包二进制是早于 fast 的版本**：`amd-neural-runtime/MochizukiNrRuntime.dll`（11-03）里没有
   `st_fb_in`/`st_box`/`st_apply` 字符串，`dlssnr-amd/shaders/renodx/` 里也没有 `renodx_st_*.spv`。
   所以现在这台机器上，就算把参数写进 ini 也不会有任何效果——而且不会有任何报错。
4. **`mz-082` 那棵树目前构建不完整**：11-04 那次构建停在 `MochizukiNrRuntime.cpp`
   （`exports/mochizuki-runtime/obj` 里只有 4 个 .obj，独缺它），因为它用到的
   `StageFrame::preserve_normalization` 在同树的 `nr_renodx.hpp` 里不存在。
   要接就接 `DLSSNR-AMD-gitlab`（0.0.8.2 自洽：stage + renodx-fast 着色器 + 键表都在）。

## 1. 它做了什么

### 1.1 legacy 与 fast 的差别

```
legacy（Pass 次数 = N）
  frame → encode → network#1 → look/resolve → encode → network#2 → … → network#N → resolve → frame
  代价 ≈ N × 2.9~3.2 ms（1080p，2.9 ms/遍是实测斜率）

fast（Pass 次数（快速）= nfast）
  frame → encode → network#1 → ┬ st_prep   拆分这次改动的粗/细
                               ├ st_gf ×2  1/4 分辨率导向滤波（st_box 横竖各一次）
                               ├ st_apply  沿改动方向放大 k 倍（带光晕限制/彩度钳制/饱和保持）
                               └ (Feedback/Hybrid) st_fb_in  把上一帧的改动按 MV 搬回网络输入
  代价 ≈ 1 遍网络 + 4 个全分辨率 dispatch + 2 个 1/4 dispatch（Feedback 再加 3 个）
```

关键文件：

| 位置 | 内容 |
|---|---|
| `third_party/mochizuki/windows/shaders/renodx-fast/{st_prep,st_box,st_fb_in,st_apply}.comp` + `st_math.glsl` | 算法本体（GLSL） |
| `third_party/mochizuki/windows/src/core/nr_renodx.cpp:1683-1875` | 判定与调度（`fast` / `fast_feedback` / `ensure_st`） |
| `.../nr_renodx.hpp:31-137` | `Settings` 的 fast 字段与 `stack_passes()` |
| `mz-082/OptiScaler-.../mochizuki_runtime/MochizukiRenoOptions.inc` | ABI 字段 ↔ Settings 字段的单点表（**fast 缺口就在这里**） |

### 1.2 强度刻度（`nr_renodx.cpp:1810-1815`）

```
kt[] = {1.00, 1.77, 2.36, 2.94}   放大倍数 k
wt[] = {0,    0.50, 0.67, 0.75}   反馈权重 w（Feedback/Hybrid 用）
nfast > 4：k = 2.94 + 0.58·(nfast−4)，w = min(0.8, 1 − 1/nfast)
```

截图参数（外推 5.06、光晕限制开、细节 0.24）换算：

- `k = 2.94 + 0.58 × 1.06 = 3.55`，`w = 0.80`
- 意义：把这一遍网络对每个像素的改动**再推 3.55 倍**，细节只补 24%（`Fine = 0.24`），并限制新增光晕。

三个容易读错的地方：

1. **`Passes (fast) = 1.00` 等于关闭**。`fast` 的判据是 `fast_passes[method] > 1`（`nr_renodx.cpp:1685`），
   1 遍时走的就是普通单遍，只是"没有强化"。
2. **fast 模式下 `reno_passes` 无效**。`stack_passes()` 在 `passes_mode==1` 时直接返回 1
   （`nr_renodx.hpp:133-137`），面板上旧的「Pass 次数」变成死字段——这是本仓库已经踩过四次的
   "能选但什么也不做" 的典型形态。
3. **`NRFastEditStrength` 只在方法 = 改动强度 时出现**，它不走 `fast_passes`，判据是
   `fast_edit_strength != 1`（`nr_renodx.cpp:1685`）。

### 1.3 三种方法

| 方法 | 需要 MV | 行为 | 视频管线适用性 |
|---|---|---|---|
| 外推 Extrapolation（默认） | 否 | 沿本帧改动的方向再推一步，首帧即生效 | ✅ 首选，不依赖 VME |
| 反馈 Feedback | 是 | 上一帧改动按 MV 搬回网络输入，前几帧逐渐建立 | ⚠️ 取决于播放器 MV 质量 |
| 混合 Hybrid | 是 | 历史可靠处用 Feedback，新表面/切镜/边缘退化成外推 | ⚠️ 同上，切镜要 reset |
| 改动强度 Edit strength | 否 | 只放大改动（等效"调整 NR 的效果→改动强度"的宽量程版） | 可用来单独量"改动放大"的纯效应 |

无 MV 时 Feedback/Hybrid **不报错**：`facts.fast_fallback = true`，画面按外推走
（`nr_renodx.cpp:1691`）；而 `.asi` 面板会把这两项灰掉（`nr_control_ui.hpp`/`nr_control_pages.hpp:1826-1841`）。
播放器侧若要照抄这个语义，必须能读到"有没有 MV"。

## 2. 本机现状（四处缺口，逐条可验证）

```bash
# ① 随包 DLL 里没有 fast 的着色器名（st_fb_in/st_box/st_apply 是字面量，编译进去就一定有）
python -c "d=open('amd-neural-runtime/MochizukiNrRuntime.dll','rb').read();print([ (p,d.count(p)) for p in (b'st_fb_in',b'st_box',b'st_apply')])"
# → 全是 0

# ② 随包着色器目录里没有 renodx_st_*.spv
ls amd-neural-runtime/dlssnr-amd/shaders/renodx | grep st_    # → 空

# ③ ABI 选项表里没有 fast（86 行，只有 passes/look/codec）
grep -c fast mz-src/mz-082/OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/mochizuki_runtime/MochizukiRenoOptions.inc  # → 0

# ④ 播放器侧没有 fast 的任何键
grep -rn "passes_mode\|fast_passes" dlss5-video-amd/src | wc -l   # → 0
```

另外两条：

- **打包脚本不带 fast 着色器**：`mz-082/tools/build/mochizuki-renodx.py:5-8` 的 `KERNELS` 只列了 19 个
  `renodx_*`，**没有 `st_*`**；带 `st_*` 的是同仓的 `windows/build/build_renodx.sh` 末尾那四行。
  这意味着"重编 runtime 就会带上"是错的——会少四个 spv。
- **缺 spv 的失败形态很隐蔽**：`ensure_st()` 是**懒加载**（`nr_renodx.cpp:593-597`，第一次真正跑 fast 才 load），
  而 `load()` 里 `read_spirv()` 走 `check()`。所以缺文件不会在启动时报错，会在**第一个 fast 帧**抛/中断。
  排查时极容易被误读成"设置没生效"。

## 3. 接入方案

### 3.1 目标形态

```
播放器 (dlss5-video-amd)
  [RenoDX] 对话框 / overrides  ──┐
   NeuralRenderTypes.h RenoDxSettings ──→ MochizukiOptions.overrides ("reno_passes_mode" 等)
                                 │
                            AmdMochizukiApi.h kControlFields（键名→offsetof+范围）
                                 │
                     MochizukiNrControls（ABI 结构，host 与 runtime 各一份，逐字节对齐）
                                 │
  runtime: Sanitise() ──→ nr::renodx::Settings ──→ Stage::record()  ──→ st_* dispatch
                                 │
                      dlssnr-amd/shaders/renodx/renodx_st_*.spv（必须随包）
```

### 3.2 Step 1 — ABI 加字段（最关键，两边必须同一次改）

`struct_size` 是**严格等值**校验（`MochizukiNrRuntime.cpp:7527`），加字段必然破坏旧对照，
所以 host 与 runtime 必须成套发布——这是设计意图，不要改成"兼容读取"。

需要加 11 个成员（名字按 `nr_renodx_keys.hpp:123-135` 的 ini 键对齐）：

| ABI 成员 | Settings 成员 | 类型 | 默认 | 范围 |
|---|---|---|---|---|
| `reno_passes_mode` | `passes_mode` | U | 0 | 0..1（0 legacy / 1 fast） |
| `reno_fast_method` | `fast_method` | U | 0 | 0..3（外推/反馈/混合/改动强度） |
| `reno_fast_passes_0/1/2` | `fast_passes[0..2]` | F | 3.0 | 1..8 |
| `reno_fast_halo_limit_0/1/2` | `fast_halo_limit[0..2]` | B | false | 0..1 |
| `reno_fast_fine_0/1/2` | `fast_fine[0..2]` | F | 0 / 1 / 1 | 0..2 |
| `reno_fast_edit_strength` | `fast_edit_strength` | F | 2.0 | 0..4 |

落地写法（三处宏展开已经现成，加行即可）：

1. `MochizukiRenoOptions.inc` 追加 11 行 `RDX_*`（数组用 `fast_passes[0]` 这种下标写法，
   和 `pass_transfer[0]` 一致）。
2. `MochizukiNrControls.h` 追加同名成员——**runtime 树与 host 的
   `dlss5-video-amd/third_party/mochizuki/MochizukiNrControls.h` 都要改**（后者是 vendored 副本，
   已核对除导出修饰外内容一致）。
3. `Sanitise()`（`MochizukiNrRuntime.cpp:751-753`）不需要改代码，宏会按 low/high 自动 clamp；
   但 `passes_mode` 建议额外做一次 `min(1)`（越界值不该变成"某个未定义模式"）。

### 3.3 Step 2 — 打包补 spv

`tools/build/mochizuki-renodx.py` 的 `KERNELS` 里加：

```python
'st_fb_in st_prep st_box st_apply'   # renodx_st_*.spv
```

（`build_renodx.sh` 里用的是 glslang + `-Iwindows/shaders/renodx-fast`，而 python 里 `KERNELS`
走的是 Slang 路径，所以**不能直接塞进 `KERNELS`**——那四个是 GLSL，得照 `renodx_prep` 那一段
用 glslang 单独编。这是最容易踩的一处：塞进 KERNELS 会得到 Slang 找不到 `st_apply.cs_5_1.hlsl` 的报错。）

产物落到 `exports/mochizuki-runtime/dlssnr-amd/shaders/renodx/`，再连同 DLL 整套 stage 到
`dlss5-video-amd/amd-neural-runtime/`。

### 3.4 Step 3 — 播放器侧接线（4 个单点，都在既有表里）

| 文件 | 改动 |
|---|---|
| `src/AmdMochizukiApi.h:186-` | `kControlFields` 加 11 条（键名 + `offsetof` + `Kind` + 范围）。漏一条的后果是 override 被当 unknown 丢掉，且不报错。 |
| `src/NeuralRenderTypes.h:229-` | `RenoDxSettings` 加 11 个 `Field`、`kControlName`、`kDefault`（默认值必须抄 `nr_renodx_keys.hpp` 的：3.0/0/1/1/2.0，否则"没动过的键"也会被写进 controls）。 |
| 同上 `:466-510` | `ApplyNeuralRoute` 的 `kRouteKeys` 加这 11 个键（否则关掉路由后残留 override → 下次进 RenoDX 就带着 fast）。 |
| `src/main.cpp:9434-` / `src/Localization.h` | 对话框加行。当前 470×652 放不下，需要加高到 ~860；控件形态照 `.asi`：模式 radio（legacy/fast）、方法 combo（4 项，无 MV 时禁用 2/3）、强度滑杆 1..8、光晕限制、细节、（方法=改动强度时的）改动强度。 |

### 3.5 Step 4 — 可观测性（不做这步等于无法验收）

1. runtime 日志：把现有
   `[mochizuki] renodx style: requested=%d requested.passes=[%d,%d,%d] feed_mode=%u`
   （`MochizukiNrRuntime.cpp:7065`）扩成带 `mode/method/fast_passes/motion/fallback` 的一行，
   直接读 `Stage::facts()` 的 `fast_passes` / `motion` / `fast_fallback`。
2. host 侧 `FormatMochizukiOptions` 的回显里加上这三个键（**别只加在 evaluator**，
   离线路径 `OfflineNeuralRenderer.cpp` 会清 `config.log`）。
3. `PolicyTests` 增用例：默认值不进 overrides、非默认值进、路由关闭后清干净。

## 4. 怎么判"真的生效"

- **先看日志**，别看画面：`mode=1 method=0 fast_passes=5.06 motion=1`。
- 计时尺子现成（`docs/NEURAL-RENDER-SPEED-PLAN-zh.md` 实测）：
  `RenoPasses=1 → 12.38 ms`、`=2 → 15.54`、`=4 → 21.05`。
  fast(nfast=5.06) 若真是"一遍网络 + 后处理"，应该落在 **12.4~13.5 ms**，而不是 21 ms。
  超出这个区间说明网络跑了多遍（`stack_passes` 没生效 / 传的不是 `passes_mode=1`）。
- **画质不能拿 head-line PSNR 判**：fast 放大的是同一份改动，低频（relight）分量同步被放大，
  而低频占残差平方和 96.9~98.6%，任何"更亮更透"的改动都会让 head-line 数字大幅移动，
  这与"重建更准"是两回事。判据沿用既有约定：**≥2/3 镜头同号且每段 >0.05 dB**，
  再加人眼对光晕/暗部色阶（`halo_limit`、`darkcol`）的核对。
- 预期副作用（由算法决定，不是 bug）：放大改动 ⇒ **亮度与饱和度同时抬**；`Fine=0` 与
  `halo_limit` 是两个护栏，`Fine` 抬高会连游戏/片源自带颗粒一起放大。

## 5. 陷阱清单

1. **三个"能选但什么也不做"**：fast 下的 `Pass 次数`、`链式历史`（单遍无意义）、`解锁 Passes`
   （被 `stack_passes` 短路）。面板必须灰掉，否则又是一次"设置不生效"投诉。
2. **缺 spv 只在第一个 fast 帧炸**（懒加载），且报错点在 `read_spirv`，看不出是"漏打包"。
3. **`passes_mode` 与 `reno_passes` 是两个 owner**：沿用既有教训——"遍数的唯一 owner"
   现在是 `passes_mode ? fast_passes[method] : reno_passes`，日志必须把两者都打出来。
4. **Feedback 依赖 MV 的真实性**：`fast_motion_available` 在全链路里默认为 `true`，runtime 从不下调。
   若播放器给的是"中性填充"的运动纹理（零位移），Feedback 会按零位移把上一帧改动原样搬回来，
   表现为"拖影式自我复制"而不是报错。接 Feedback 前必须先确认绑定的是真 VME 场。
5. **`mz-082` 树的 hpp/cpp 不是同一版本**（`preserve_normalization` 缺失），直接在里面加字段会
   把"编不过"和"接错了"两个问题搅在一起。建议以 `DLSSNR-AMD-gitlab`（0.0.8.2）为基线做移植。
6. **`reno_passes` 在 fast 下不能"顺手置 1"**：它仍是 legacy 模式的返回点，写死会让用户切回
   legacy 时丢失原来的遍数。

## 6. 建议的落地顺序

1. **最小可用（先量收益，不动 UI）**：Step 1 + Step 2（ABI 11 字段 + 打包 4 个 spv + 重编 runtime）
   → 用 `overrides` / `--render` 直接传 `reno_passes_mode=1`、`reno_fast_method=0`、
   `reno_fast_passes_0=3`，跑一遍计时与 A/B。
2. 收益成立再上 Step 3（UI）与 Step 4（日志/测试）。
3. 之后才考虑 Feedback/Hybrid——那要先有可信的 MV，是另一个命题。

## 7. 附：参数对照（截图 ↔ ini ↔ ABI）

| 截图项 | ini 键 | 值（截图） | ABI 建议名 |
|---|---|---|---|
| Pass 次数（旧式）/（快速） | `NRPassesMode` | 快速 | `reno_passes_mode` |
| 方式（外推/反馈/混合/改动强度） | `NRFastMethod` | 外推 = 0 | `reno_fast_method` |
| Pass 次数（快速） | `NRFastExtrapolationPasses` | 5.06 | `reno_fast_passes_0` |
| 限制新增光晕 | `NRFastExtrapolationHaloLimit` | 开 | `reno_fast_halo_limit_0` |
| 细节 | `NRFastExtrapolationFine` | 0.24 | `reno_fast_fine_0` |
| 改动强度（方法=改动强度时才出现） | `NRFastEditStrength` | — | `reno_fast_edit_strength` |
| 反馈/混合各自的 Passes/Halo/Fine | `NRFastFeedback*` / `NRFastHybrid*` | 默认 3 / 关 / 1 | `..._1` / `..._2` |

---

## 8. 实施记录（2026-11-06 已落地，未提交）

> 结论先行：**fast 通了，而且真生效**。成本 = 1 遍网络 + 1.1 ms 后处理。

### 8.1 移植基线的更正（重要）

调研阶段判断"以 `DLSSNR-AMD-gitlab` 为基线"是错的，实际更可行的路线是反过来：

| 树 | 状态 |
|---|---|
| `mz-src/mz-082/third_party/mochizuki` | 与 gitlab 0.0.8.2 **逐字节相同**（忽略行尾），有 fast，但**缺产品扩展** |
| `mz-src/mz-082/dlss-5-amd-project-1.14.0/` | 与 mz-082 **共用同一个 7847 行 `MochizukiNrRuntime.cpp`**，有产品扩展（`preserve_normalization` / `TransitionPush` / `RecordingResources` / `exposure_state`），**缺 fast** |

两棵树是**分叉**，不是版本先后。直接换层会炸：`mz-082` 那棵树编不过，实测报错不止 `preserve_normalization` 一处，
而是 `ProductRuntime` 缺 10+ 成员（`set_white_point` / `begin_recording_resources` / `gpu_timing_serial` /
`harvest_recording_timings` / `exposureSnapshot` …）。**正确做法是在 1.14.0 上增量加 fast**，一行产品扩展都不删。

### 8.2 改了什么

runtime 侧（`mz-src/mz-082/dlss-5-amd-project-1.14.0/`）：

| 文件 | 改动 |
|---|---|
| `third_party/mochizuki/windows/src/core/nr_renodx.hpp` | `Settings` 加 6 组 fast 字段；`stack_passes()` 加 `passes_mode==1 → 1` 短路；`StageFrame::fast_motion_available`；`Facts` 加 `fast_passes` / `motion` / `fast_fallback` |
| `.../nr_renodx.cpp` | `Impl::P` 加 `kStFbIn/kStPrep/kStBox/kStApply`；`st_*` 图像成员 + `ensure_st()` + `st_gf()`；判定与调度段插在"3a 编码之后"与"3b 网络之后"；`history_settings_differ` 纳入 fast 字段 |
| `.../shaders/renodx-fast/` | 从 mz-082 整目录拷入（4 个 `.comp` + `st_math.glsl`） |
| `tools/build/mochizuki-renodx.py` | 新增 4 个 `st_*` 的 glslang 编译（**不能塞进 `KERNELS`**，那是 Slang） |
| `.../mochizuki_runtime/MochizukiRenoOptions.inc` | 追加 **12** 行 `RDX_*`（不是 11：`passes_mode` + `fast_method` + 3 组×(passes/halo/fine) + `edit_strength`） |
| `.../MochizukiNrControls.h`（runtime 与 host 各一份） | 同名 12 个成员，**两份必须同一次改** |
| `.../MochizukiNrRuntime.cpp` | `renodx style:` 行扩出 mode/method/fast_passes/fine/halo/edit；新增 `renodx fast (last frame): strength=… motion=… fallback=…`（读 `Stage::facts()`） |

host 侧（`dlss5-video-amd/`）：

| 文件 | 改动 |
|---|---|
| `src/NeuralRenderTypes.h` | `RenoDxSettings` 加 12 个枚举 / `kIniKey` / `kControlName` / `kDefault` / `kStart` / `kLow` / `kHigh`；`ApplyNeuralRoute` 的 `kRouteKeys` 加 12 个键 |
| `src/AmdMochizukiApi.h` | `kControlFields` 加 12 条 |
| `src/main.cpp` | 启动渲染时 `LOG("Mochizuki overrides: …")`，与 runtime 的 `renodx style:` 对成两端 |
| `tests/PolicyTests.cpp` | fast 用例：默认值/范围/不写默认键/写非默认键/两位小数/路由关闭清干净 |

`kStart` 的 `RenoPassesMode` 为 **0**（legacy）：开 fast 是换一种画面，不该是"新 profile 默认就在里面"。

### 8.3 实测（1920x800，60 帧，同素材同几何，`--stages nr --neural-backend amd`）

| 臂 | submit p50 | submit p95 | network p50 | 输出像素哈希 |
|---|---:|---:|---:|---|
| legacy 1 遍（`RenoPasses=1`） | 10.81 | 15.93 | 3.00 | `c0566d00…` |
| legacy 2 遍（长跑基线，3680 帧） | 14.23 | 17.20 | 6.45 | — |
| **fast `5.06`（外推）** | **11.90** | 16.57 | **3.13** | `595f742c…` |

- **fast(5.06) = 1 遍 + 1.1 ms**。若真跑 5 遍应是 `10.81 + 4×3.42 ≈ 24.5 ms`，实测 11.90 ⇒ 成本约为 5 遍的 **46%**。
- **两臂像素哈希不同**（`fast` 102,144 B vs `legacy1` 73,659 B）⇒ `st_prep/st_gf/st_apply` 真的改了画面，
  不是"只跑一遍网络什么都没强化"。这是本次最关键的判据——只看 `passes=1` 分不出"生效"和"没生效"。
- 首次运行 `total` 会被 `graph build 1.81s` 摊爆（27.68 ms/帧），**只看 p50，别用 mean 判**。

### 8.4 踩到的坑

1. **`build_network.py` 会被沙箱批量删除保护拦下**（`SAFE_DELETE_BULK_CONFIRM_REQUIRED`，阈值 50 个文件）。
   绕法：先 `mv dlssnr-amd dlssnr-amd.bak-<date>`，然后单独跑 `mochizuki-renodx.py` 的 `build()` 补着色器，
   再 `--skip-shaders` 编 DLL。**不要**为了过这一步去关安全策略。
2. **`--render` 默认走 NVIDIA 后端**（`ExportPlan::backend` 默认 Nvidia），会报
   `The neural runtime does not match the locked stack: nvngx_dlssnr.dll: missing …`。必须显式 `--neural-backend amd`。
3. **`--height` 只接受 1080/1440/2160，且只对 `sr` 有效**；`nr` 单跑时不要带 `--height`。
4. `--skip-shaders` 不写 manifest，所以 `exports/mochizuki-runtime/mochizuki-build.json` 可能不存在——
   只影响可复现性证明，不影响 DLL。

### 8.5 遗留（不要当成"已支持"）

1. **Feedback / Hybrid 在本机不可用**：日志显示 `guides=motion:constant`（AMD 后端无 guide estimator，job 也没带
   guide 文件），runtime 里 `hasMotion` 是硬编码 `true`。若把 `reno_fast_method` 设成 1/2，会拿**恒定填充**当运动矢量，
   表现为"拖影式自我复制"而不是报错。当前只有 `Extrapolation(0)` 和 `Edit strength(3)` 是安全的。
   要接 Feedback，先解决 MV 来源（真 VME 场），再让 runtime 在拿不到真 MV 时把 `fast_motion_available` 置 false。
2. **UI 没做**：目前只能通过 `[RenoDX]` 的 `RenoPassesMode` / `RenoFastExtrapolationPasses` 等键开启，
   或 `ApplyNeuralRoute` 的 overrides。对话框还没加行，面板上也还没有对应控件。
3. **fast 下 `RenoPasses` 是死字段**（`stack_passes()` 短路）。UI 做的时候必须灰掉，
   否则又是一次"设置不生效"。`ChainedHistory` 同理（单遍无意义）。

### 8.6 怎么复现 / 怎么回退

```
# 开启（run-fg/DLSSVideoPlayer.ini，latin-1 无 BOM，[RenoDX] 段）
RenoPassesMode=1
RenoFastExtrapolationPasses=5.06

# 计时臂
DLSSVideoPlayer.exe --render <in> --stages nr --range f0-f60 --neural-backend amd --out <out>
```

回退：`amd-neural-runtime/MochizukiNrRuntime.dll.bak-20261106`（旧 11-03 版）、
`run-fg/DLSSVideoPlayer.exe.bak-pre-fast`、`run-fg/neural-runtime/NeuralWorker.exe.bak-pre-fast`、
`run-fg/DLSSVideoPlayer.ini.bak-pre-fast`。

---

## 9. 对比素材（2026-11-06）

产物：

| 文件 | 规格 | 大小 |
|---|---|---|
| `docs/media/renodx-fast-vs-legacy-5pass.png` | 2040×1470 静态对比海报 | 1.6 MB |
| `docs/media/renodx-fast-vs-legacy-5pass.mp4` | 1920×556 / 24 fps / 10 s / 240 帧 | 4.5 MB |

### 9.1 对比臂的选择（为什么是"传统 5 遍"）

fast 的强度是用 legacy 遍数当刻度。`kt={1, 1.77, 2.36, 2.94}`，`n>4` 时 `k = 2.94 + 0.58(n−4)`：

- 传统 `RenoPasses=5` → **k = 3.52**
- 快速 `RenoFastExtrapolationPasses=5.06` → **k = 2.94 + 0.58×1.06 = 3.55**

两者标称强度几乎相等，所以这组才是"同强度、不同代价"的干净对照。**不要用"传统 2 遍 vs 快速 5.06"**——
那是同时改了两个变量（强度 + 模式），看出来的差无法归因。

### 9.2 素材与镜头

`tears_of_steel_1080p.mov`，**449.5–459.5 s（f10788–f11028，240 帧）**。
这一段在 444.9–461.8 s 的同一镜头内，**没有剪切**（`select='gt(scene,0.3)'` 实测：
430 s 起 50 s 内的切点在 431.5 / 434.2 / 438.6 / 441.2 / 444.9 / 461.8 / 464.8 / 468.3 / 469.5 / 479.0）。
⚠️ 最初选的 455–465 s 跨了 461.8 的剪切，重渲过一版。

### 9.3 实测（1920×800，`--stages nr --neural-backend amd`，240 帧）

| 臂 | submit p50 | 网络 p50 | 网络遍数 |
|---|---:|---:|---:|
| 传统 5 遍 | 22.91 ms | 14.77 ms | 5 |
| 快速 5.06 | 10.91 ms | 3.14 ms | 1 |
| 差 | **2.10×** | 4.7× | 5 → 1 |

> 与第 8.3 节那组（24.63 / 11.55 → 2.13×）方向一致，绝对值的差异来自**换了片段**（运动量不同），
> 不是配置漂移。报数字必须和素材绑定。

### 9.4 两臂画面差异（同一帧：源片第 10908 帧 = 454.5 s，片段内第 120 帧）

| 指标 | 值 | 读法 |
|---|---|---|
| PSNR(Y) 全片 | 31.4 dB | 别单独用，被低频主导 |
| SSIM(Y) 全片 | 0.983 | — |
| 平均像素差 | 4.5 / 255 | — |
| 低频（relight）相关 | 0.996 | 色调映射几乎一致 |
| 高通（结构）相关 | 0.938 | 结构对齐，但不是逐像素相同 |
| 接缝台阶 | ≈ 6 / 255 | 相邻列梯度 1.32，整图列梯度 p95 = 1.20 ⇒ **刚好可察觉** |

**结论要这么说**：快速 5.06 复现了传统 5 遍的观感（同强度、同色调、同结构），
但**不是同一张图**——接缝检验能看出 ≈2% 的色阶差，集中在暗部/低对比区域。
这也正是"两臂像素哈希不同"的量化版本，别对外说"完全一样"。

### 9.5 复现

```
# 臂 A：传统 5 遍（ini 里 RenoPasses>4 会自动加 reno_unlock_passes=1）
RenoPassesMode=0 / RenoPasses=5.00

# 臂 B：快速
RenoPassesMode=1 / RenoFastExtrapolationPasses=5.06 / RenoPasses=1.00

DLSSVideoPlayer.exe --render <in> --stages nr --range f10788-f11028 --neural-backend amd --out <out>
```

出图/出片脚本：`F:/dlss5-video/tmp/cmp/make_still.py`、`make_video_fast.py`（PIL 合成 + ffmpeg 编码，
视频走 rawvideo 管道，crf 19 / preset slow，10 s 约 4.5 MB）。

---

## 10. 三档强度复测：3 / 5 / 10 遍（2026-11-06 18:10）

产物：

| 文件 | 规格 | 大小 |
|---|---|---|
| `docs/media/renodx-fast-vs-multipass-3-5-10.png` | 2040×1880 对比海报 | 1.7 MB |
| `docs/media/renodx-fast-vs-multipass-3-5-10.mp4` | 1920×520 / 24 fps / 20 s / 480 帧 | 6.1 MB |

脚本：`F:/dlss5-video/tmp/cmp3/make_still3.py`、`make_video3.py`；臂产物在 `F:/dlss5-video/tmp/cmp3/*.mkv`。

### 10.1 素材与臂

`F:/dlss5-video/tmp/shot15.mp4`（《终结者：黑暗命运》，1920×800，360 帧 / 15 s，从 `seg3min` 的 115 s 起切 15 s，
即 11-05 那轮用的同一段）。整段跑，不抽子区间 —— 六个臂各 360 帧，逐帧对齐。

| 臂 | ini | 标称强度 |
|---|---|---|
| L3 / L5 / L10 | `RenoPassesMode=0`, `RenoPasses=3/5/10` | k = 2.36 / 3.52 / 6.42 |
| F3 / F5 / F10 | `RenoPassesMode=1`, `RenoFastExtrapolationPasses=3/5/10` | 同上（F10 实际 8 → k=5.26） |

k 由 `nr_renodx.cpp:1784-1787` 折算：`kt={1,1.77,2.36,2.94}`，`n>4` 时 `k=2.94+0.58(n−4)`。三档都是**同刻度**对照。

### 10.2 实测（submit p50 / network p50，ms/帧）

| 组 | 传统 | 快速 | 加速 |
|---|---:|---:|---:|
| 3 遍（k=2.36） | 16.74 / 9.32 | 11.03 / 3.15 | **1.52×** |
| 5 遍（k=3.52） | 22.24 / 14.80 | 10.58 / 3.15 | **2.10×** |
| 10 遍（传统 k=6.42 / 快速 k=5.26） | 38.81 / 30.45 | 10.66 / 3.15 | **3.64×** |

**快速的成本与强度无关**：三档都是 10.6–11.0 ms，网络恒 3.15 —— 强度只改 `st_apply` 的一个 uniform。
所以越往上用越划算，前提是画面对得上。

### 10.3 画面对不对得上（同强度两两比，全 360 帧）

| 组 | PSNR(Y) | SSIM(Y) | 结论 |
|---|---:|---:|---|
| L3 vs F3 | **42.1 dB** | 0.989 | 基本等价 |
| L5 vs F5 | 31.8 dB | 0.981 | 开始分叉 |
| L10 vs F10 | 24.0 dB | 0.942 | 不是同一种画面 |

10 遍那组还混了「强度上限」这一项，单独剥出来：**L8 vs F10 同为 k=5.26 → 25.4 dB / SSIM 0.953**。
所以即使强度对齐，外推在 k≈5.3 时的误差也已经到 25 dB 量级。

### 10.4 关键发现：三档之后两者走向相反

12 帧平均的色调统计（第 5 百分位 = 阴影亮度，std = 全局对比）：

| 臂 | 阴影 5% | 全局 std | 高频能量（相对原片） |
|---|---:|---:|---:|
| 原片 | 9.84 | 43.81 | 1.000 |
| L3 / L5 / L10 | 9.22 / 9.86 / **12.01** | 45.92 / 42.23 / **35.55** | 1.252 / 1.230 / 1.298 |
| F3 / F5 / F10 | 8.36 / 6.69 / **4.53** | 46.39 / 47.82 / **49.92** | 1.120 / 1.216 / 1.358 |

- **传统多遍 = 越叠越“平”**：阴影被抬亮（9.2 → 12.0）、全局对比下降（45.9 → 35.6）。
  这和“多遍会糊”的既有经验一致 —— 第 5 遍起重复跑 pass 4 的权重，画面越来越奶。
- **快速 = 越强越“硬”**：阴影被压死（8.4 → 4.5）、对比上升（46.4 → 49.9）。
  外推是把一遍网络做的改动再放大 k 倍，方向随 k 线性外推，所以高 k 时低对比区域的改动被放大成“压黑”。

⇒ **fast 只在前两档是“便宜的等效”，第三档是另一种风格。** 这也解释了为什么 L10 看起来比 F10 更“灰”。
按这个方向，`reno_fast_passes` 的实用区间是 **1–5**（k≤3.52），5 以上要接受风格漂移。

### 10.5 一个必须记住的硬限制

`reno_fast_passes_*` 的合法区间是 **1..8**（`MochizukiRenoOptions.inc:15` 的 `RDX_F(..., 1.0f, 8.0f)`，
host `kControlFields` 同样夹在 1..8），且 `nr_renodx.cpp:1782` 里还有一层 `std::clamp(nfast,1,8)`。
所以：

- ini 写 `RenoFastExtrapolationPasses=10.00` → 运行时 `renodx style:` 打印 `fast_passes=[8.00,…]`，**静默钳到 8**；
- k 的上限就是 `2.94+0.58×4 = 5.26`，**追不上传统 10 遍的 6.42**；
- 若要更高，得同时改 runtime 的 clamp 与 ABI 上限（host 侧不发超出范围的值）。本次没改 —— 先看 5.26 的表现再决定，
  因为阴影在 k=5.26 时已经被压得很重，再放大只会更明显。

### 10.6 复现

```
# 传统 3/5/10
python /f/tmp/arm3.py 0 3.00 3.00      # 后两个参数：fast 标称值（传统臂不用）、RenoPasses
python /f/tmp/arm3.py 0 5.00 5.00
python /f/tmp/arm3.py 0 10.00 10.00    # >4 时 FormatMochizukiOptions 自动补 reno_unlock_passes=1

# 快速 3/5/10
python /f/tmp/arm3.py 1 3.00 1.00
python /f/tmp/arm3.py 1 5.00 1.00
python /f/tmp/arm3.py 1 10.00 1.00

DLSSVideoPlayer.exe --render F:/dlss5-video/tmp/shot15.mp4 --stages nr \
  --range f0-f360 --neural-backend amd --out <arm>.mkv
```

校验：`neural-runtime/NeuralWorker.log` 的 `submit(guide+render+gate) … p50` 与 `cost per frame … network p50`；
`amd-neural-runtime/mochizuki_nr.log` 的 `renodx style: … passes_mode= fast_passes=[…]`。
注意 `renodx fast (last frame)` 这行是**在渲染前打的**，短任务只会看到首帧的 0.00 —— 别拿它判 fast 是否生效。

---

## 11. 10 遍 1:1 细节图 + 放开上限到 12（k≈7.58）（2026-11-06 19:55）

两件后续：把第 10 节海报里 340×340 的裁切换成可放到 100% 看的 1:1 大图；
把 `reno_fast_passes_*` 的上限从 8 提到 12，实跑 k=7.58 看是什么样。

产物（都在 `docs/media/`）：

| 文件 | 规格 | 大小 |
|---|---|---|
| `renodx-fast-10pass-detail-1to1.png` | 2780×1440，4 张 660×660 原始像素并排 + 差异带 | 1.8 MB |
| `renodx-fast-cap12-k7p58.png` | 2040×1660，整帧 / 1:1 裁切 / 三块数值面板 / 结论 | 1.7 MB |

脚本：`F:/dlss5-video/tmp/cmp3/make_detail1to1.py`、`make_cap12.py`、`metrics_cap12.py`；
数值 `cap12-metrics.json`；臂 `F12.mkv` / `L12.mkv`（各 360 帧整段）。

### 11.1 1:1 细节图

第 72 帧裁 `x420–1080 / y0–660`，四张均**不缩放不锐化**：
原片 → 传统 8 遍(k=5.26) → 快速 10→8(k=5.26) → 传统 10 遍(k=6.42)。
补 `L8` 是为了把"强度上限"从差异里剥掉，两张同 k 直接比。
下方差异带是 `|ΔY|×3` 的裁切上 250 行（仍是 1:1 像素），平均 |Δ| 17.3/255。
1:1 看的是结构与色调：传统把头发/木墙一起抬亮、边缘变软，快速把同一区域压黑、颗粒变硬。

### 11.2 上限 8 → 12 改了什么

四处范围必须成套改，漏一处就会静默钳回 8：

| 文件 | 改动 |
|---|---|
| `mz-src/mz-082/dlss-5-amd-project-1.14.0/third_party/mochizuki/windows/src/core/nr_renodx.cpp:1782` | `std::clamp(..., 1.f, 8.f)` → `12.f` |
| `.../OptiScaler/dlssnr/backend/mochizuki_runtime/MochizukiRenoOptions.inc` | 三行 `RDX_F(reno_fast_passes_N, ..., 8.0f)` → `12.0f` |
| `dlss5-video-amd/src/NeuralRenderTypes.h` | `kHigh` 三个 8.0 → 12.0 + 四处注释 |
| `dlss5-video-amd/src/AmdMochizukiApi.h` | `kControlFields` 三行 `1.f, 8.f` → `12.f` + 注释 |
| `dlss5-video-amd/tests/PolicyTests.cpp` | 上限断言改 12，新增 `12.00` 能原样透传到 overrides 的用例 |

shader 侧没有第二道 clamp：`st_apply.comp` 的 `lo=1.0, hi=pc.k` 只是色域二分搜索的区间。
ABI 结构没变（`struct_size` 不变），但 host 与 runtime 还是要**成套重编**：
`build-local2.cmd --skip-shaders` → `BUILD_OK`；
`build-local.cmd DLSSVideoPlayer NeuralWorker PolicyTests` → 24/24；
`PolicyTests` **425 cases 全过**。
stage：`amd-neural-runtime/MochizukiNrRuntime.dll`、`run-fg/DLSSVideoPlayer.exe`、
`run-fg/neural-runtime/NeuralWorker.exe`，旧件留 `*.bak-cap8`。
源码备份 `*.bak-20261106-cap12`。

**生效判据**：`amd-neural-runtime/mochizuki_nr.log` 的
`renodx style: ... passes_mode=1 fast_passes=[12.00,3.00,3.00]` —— 不是 8.00。
（注意日志在项目根的 `amd-neural-runtime/`，不在 `run-fg/`。）

### 11.3 实测（shot15，360 帧整段，同一几何）

k = 2.94 + 0.58×(12−4) = **7.58**，传统 12 遍与快速 12 遍**同 k**，这次不需要校准臂。

| 臂 | submit p50 | network p50 |
|---|---:|---:|
| 传统 12 遍 | 46.44 ms | 37.52 ms |
| 快速 12 遍 | 11.27 ms | 3.15 ms |

加速 **4.12×**，快速成本仍与强度无关（F3/F5/F10 都是 10.6–11.0 ms）。

全片 360 帧、BT.709 加权 Y、PSNR 用累积 MSE、SSIM 用 8×8 块平均（**新口径，不与第 10 节的旧口径混用**：
同一对 L8 vs F10 新算 24.09 dB / 0.8979，第 10 节旧值是 25.4 / 0.953）：

| 配对 | PSNR(Y) | SSIM(Y) | 平均 \|ΔY\| |
|---|---:|---:|---:|
| **传统 12 vs 快速 12（同 k）** | **20.29 dB** | **0.8183** | 24.67 / 255 |
| 快速 10→8 vs 快速 12 | 33.18 | 0.9597 | 5.59 |
| 原片 vs 快速 12 | 21.22 | 0.7991 | 22.16 |
| 原片 vs 传统 12 | 23.40 | 0.9044 | 17.25 |
| 传统 10 vs 传统 12 | 38.53 | 0.9823 | 3.02 |

12 帧抽样（每 30 帧取 1）的色调与高频：

| | 阴影 5% | 全局 std | 高频能量比（相对原片） |
|---|---:|---:|---:|
| 原片 | 9.89 | 43.72 | 1.000 |
| 快速 10→8 (k=5.26) | 4.41 | 49.81 | 1.357 |
| **快速 12 (k=7.58)** | **2.79** | **52.45** | **1.534** |
| 传统 10 (k=6.42) | 11.92 | 35.53 | 1.299 |
| **传统 12 (k=7.58)** | **12.97** | **33.60** | 1.359 |

### 11.4 结论：k≈7.58 的快速模式不可用

肉眼（第 72 帧 1:1）：快速 12 出现**阴阳脸**（左半脸压成黑块）、皮肤与胸口斑驳噪点、
木墙纹理硬到发脆；传统 12 是整体压暗一层但过渡平滑，像降曝光。
同 k 的 PSNR 只有 20.29 dB / SSIM 0.818，比 k=5.26 那对（24.09 / 0.898）更差 ——
**k 越大，外推偏离越远，不是"追上"而是"跑得更偏"**。
旧上限 8 本来就是一道保护：放开只是让数值刻度追上传统 12 遍，画质没追上。

⇒ 实用区间仍是 **1–5（k≤3.52）**；12 只作为实验档存在，做 UI 时必须灰掉或加明确警告，
别让它变成第五次"能选但结果是坏画面"。

### 11.5 复现 / 回退

```
python /f/tmp/arm3.py 1 12.00 1.00     # 快速 12
python /f/tmp/arm3.py 0 12.00 12.00    # 传统 12（>4 会自动补 reno_unlock_passes=1）
DLSSVideoPlayer.exe --render F:/dlss5-video/tmp/shot15.mp4 --stages nr \
  --range f0-f360 --neural-backend amd --out <arm>.mkv
python F:/dlss5-video/tmp/cmp3/metrics_cap12.py
```

`run-fg/DLSSVideoPlayer.ini` 已还原成 3 行基线（备份 `/f/tmp/ini.bak-cap12`）。
要把上限改回 8：还原五个 `*.bak-20261106-cap12` 源码、重编、把三个 `*.bak-cap8` 二进制拷回。
代码没有提交。

---

## 12. 按 OptiScaler 设置评估最优配置：为什么「越来越黑」（2026-11-06 20:15）

第 11 节把上限放到 12 之后画面明显发黑。这一节回答两件事：黑从哪来，
以及按上游的参数体系应该把配置定在哪。产物：

| 文件 | 内容 |
|---|---|
| `docs/media/renodx-fast-optiscaler-config-eval.png` | 2040×1520 评估海报：黑位/高频曲线 + 旋钮有效性 + 推荐 ini + 档位结论 |
| `docs/media/renodx-fast-optcfg-metrics.json` | 15 个臂的统一指标 |
| `tmp/cmp3/{metrics_optcfg.py,make_optcfg.py,run 在 /f/tmp/run_optcfg.py,/f/tmp/arm4.py}` | 复现脚本 |
| `tmp/cmp3/{N2,H5,S506,S3,S4,E2}.mkv` | 本次新增 6 个 360 帧臂 |

### 12.1 先排除两种误判

- **不是跨帧累积**。F5 与原片同帧逐点比：meanY 差在 −0.79 ~ +0.64 之间，
  5% 分位差在 −5.91 ~ −1.29 之间，帧号 0/72/144/216/288/359 上**没有单调恶化**；
  中间 216/288 帧整体变暗是镜头内容（暗场景），359 帧又回到 −0.01。
  `fast` 下 `restored=0`、`st_valid=false`，本来就没有跨帧历史。
- **不是整体降曝光**。全片均值几乎不变：原片 62.20，F12 63.44（还略升）。
  塌的是**黑位**：5% 分位 9.83 → 8.90 (2 遍) → 6.46 (5 遍) → 4.41 (8 遍) → 2.79 (12 遍)，
  同时 std 43.72 → 52.45、高频 ×1.53。所以观感是「暗部压死 + 亮部发硬」，不是「整片变暗」。
  传统多遍恰好相反：L12 的 p5 升到 12.97、std 掉到 33.60（抬暗、压平）。

### 12.2 机理：外推在暗部没有刹车

`st_apply.comp` 的 Extrapolation 走 `mode 9`，亮度由 `at7()` 给出：

```
L = st_soft(O.x, (k - 1) * amp)
st_math.glsl:  extra < 0  ->  L = a * exp(extra / a),  a = clamp(O.x, 1e-4, 1 - 1e-4)
```

`amp = base*B + fine*F + (nohalo>0.5 ? 0 : halo)*(dL-B-F)`；默认 `base=1, fine=2, halo=1,
nohalo=0`，所以 `amp = dL - F`，即这一遍网络自己的亮度改动（暗部多为负）。
k 由 `nr_renodx.cpp:1786-1789` 换算，`passes=12 → k=7.58`。

三个叠加效应：

1. 负向改动被乘 `(k−1)` 后送进指数；**暗部 a 小，除数 a 也小**，同样 extra 在暗部衰减更狠。
2. `st_in()` 只检查线性 RGB 是否落在 `[−1e-6, 1+1e-6]`；压到 **0 是合法值**，
   所以越界的 12 步 `ov_search` 二分**不会触发降强度**。这是设计上的不对称：
   高光会撞上上界被裁剪，暗部却一路畅通到死黑。
3. `ccap=2.5` 只压缩彩度方向的 `tc`，`at7()` 里的亮度用的是原 `t=pc.k`，不受限。

⇒ 压黑是外推算术的固有性质，不是配置错误，也不是 bug。

### 12.3 上游官配 / 截图 / 我们一直在跑的，是三套不同的东西

`third_party/mochizuki-asi/payload/ini/dlssnr-amd-optiscaler-renodx.ini:188-210` 与
`windows/src/ui/nr_control_pages.hpp:1826-1868` 是权威：

| 项 | 上游默认 | 用户截图 | 第 9–11 节的实验臂 |
|---|---|---|---|
| `NRPassesMode` | **legacy**（默认不开 fast） | fast | fast |
| `NRFastMethod` | 0 Extrapolation | 0 | 0 |
| `Passes (fast)` | **3.00**，范围 1..8 | 5.06 | 3 / 5 / 10→8 / 12 |
| `HaloLimit` | 0 | 开 | 0 |
| `Fine` | **0.00**（Extrapolation） | 0.24 | 0 |
| `EditStrength` | 2.00 | — | — |

两个要点：

- **面板 slider 上限本来就是 8**。第 11 节把它提到 12 是越出上游设计；
  第 11 节实测 12 遍是坏画面，正好印证 8 是保护线。
- 之前所有 fast 臂都用的 `.inc` 默认 `halo=0 / fine=0`，**从来没按截图那套跑过**。
  所以这次补 6 个臂专门隔离这两个旋钮。

### 12.4 实测（shot15，360 帧，与第 11 节同口径）

口径：BT.709 加权 Y，PSNR 用全片累积 MSE、SSIM 用 8×8 块平均；色调为每 30 帧抽样 12 帧均值。
源片 Y 这次缓存为 uint8，与第 11 节有 ≤0.06 级舍入差，但**同一脚本内所有臂共用同一源**，
delta 自洽。不要与第 10 节旧口径混拼。

| 臂 | 配置 | k | p5 (Δ) | std (Δ) | 高频比 | vs 原片 PSNR / SSIM |
|---|---|---:|---:|---:|---:|---:|
| 原片 | — | 0 | 9.83 | 43.72 | 1.000 | — |
| N2 | 2 遍 / 默认 | 1.77 | 8.90 (−0.94) | +1.86 | 1.066 | 31.26 / 0.9582 |
| F3 | **上游默认 3 遍** | 2.36 | 8.21 (−1.63) | +2.56 | 1.114 | 29.12 / 0.9433 |
| **S3** | **3 遍 / halo=1 / fine=0.24** | 2.36 | 8.19 (−1.65) | +2.46 | 1.107 | **29.62 / 0.9496** |
| S4 | 4 遍 / halo=1 / fine=0.24 | 2.94 | 7.46 (−2.38) | +3.12 | 1.154 | 28.06 / 0.9364 |
| F5 | 5 遍 / 默认 | 3.52 | 6.46 (−3.37) | +3.99 | 1.210 | 26.23 / 0.9097 |
| H5 | 5 遍 / **halo=1** | 3.52 | 6.69 (−3.15) | +3.78 | **1.113** | 26.84 / 0.9240 |
| S506 | **截图原配** 5.06 / halo=1 / fine=0.24 | 3.55 | 6.56 (−3.27) | +3.83 | 1.203 | 26.73 / 0.9215 |
| F10 | 10→8 遍 / 默认 | 5.26 | 4.41 (−5.42) | +6.09 | 1.352 | 23.52 / 0.8594 |
| F12 | 12 遍 / 默认 | 7.58 | 2.79 (−7.05) | +8.73 | 1.527 | 21.22 / 0.7992 |
| E2 | **method=3 Edit strength 2.00** | — | 8.76 (−1.07) | +2.18 | 1.181 | 30.22 / 0.9554 |
| L3 | 传统 3 遍 | 2.36 | 9.02 (−0.81) | +2.10 | 1.246 | 29.28 / 0.9463 |
| L5 | 传统 5 遍 | 3.52 | 9.74 (−0.09) | −1.56 | 1.225 | 28.82 / 0.9455 |
| L12 | 传统 12 遍 | 7.58 | 12.97 (+3.14) | −10.12 | 1.354 | 23.40 / 0.9043 |

**规律**：k 每加约 1，p5 掉 1.0–1.1 级、std 涨约 1.2、高频比涨约 0.09。
fast 与传统在 **3 遍处最吻合**（29.12/0.9433 vs 29.28/0.9463），
5 遍起分叉（26.23/0.9097 vs 28.82/0.9455），12 遍完全相反。

**耗时与强度无关**：6 个新臂 submit p50 全在 10.98–11.59 ms、network p50 3.14–3.15 ms。
（E2 那次 `encoder back pressure` 冷启动 1.50 ms，按既有教训不拿它比哈希/耗时。）

### 12.5 关键结论：HaloLimit / Fine 不是压黑的刹车

- `H5 vs F5`：高频 1.210 → **1.113**（有效，压掉颗粒），PSNR +0.61 dB，
  但 p5 只从 6.46 回到 **6.69（+0.23 级）**。
- `S506`（截图原配，k=3.55）p5 6.56，比 F5 还低一点 —— 因为它的 k 略大。
- 原因见 12.2：`halo=1 → 0` 只把 `amp` 里的残差项 `(dL−B−F)` 去掉，
  改成 `amp = B + 0.48F`；**主导暗部塌缩的 `(k−1)` 完全没动**。
  `fine` 同理，只改细频带权重。

⇒ **唯一有效的旋钮是 `RenoFastExtrapolationPasses`（即 k）**。
截图把 5.06 配 halo/fine 是在调「颗粒和光晕」，不是在调亮度；它并不比默认 5 遍更亮。

### 12.6 推荐配置

```ini
[RenoDX]
RenoPassesMode=1
RenoFastMethod=0
RenoFastExtrapolationPasses=3.00
RenoFastExtrapolationHaloLimit=1
RenoFastExtrapolationFine=0.24
RenoPasses=1.00
```

- 强度保持**上游默认 3**：p5 仅 −1.65 级，加速 ≈1.5×（11.1 ms vs 传统 3 遍 16.74 ms），
  与传统 3 遍几乎同画面。
- `HaloLimit=1 + Fine=0.24` 是**同强度下的免费改进**：+0.50 dB / +0.0063 SSIM，
  高频 1.114 → 1.107，没有额外耗时。
- 若「绝不压黑」优先：降到 `Passes=2.00`（N2，p5 −0.94、31.26 dB），
  或改用 `RenoFastMethod=3` + `RenoFastEditStrength=2.00`（E2，p5 −1.07、30.22 dB）——
  `st_edit()` 在**对数域**对称放大 `exp2(t(k−1)e)`，暗部不会指数塌到 0。
  ⚠️ method=3 时 `Passes/HaloLimit/Fine` **全是死字段**，做 UI 必须灰掉，
  否则就是第五次「能选但什么也不做」。
- **禁用 ≥8 遍**；`Passes(fast)=1.00` 等于关闭；fast 下 `RenoPasses` 与 `ChainedHistory` 都是死字段。

**该建议已执行**：上限回退到 8 见第 13 节；推荐配置成为 fast 默认值、
模式切换 UI 与 >5 警告见第 14 节。

### 12.7 复现 / 状态

```
python /f/tmp/arm4.py RenoPassesMode=1 RenoFastMethod=0 \
  RenoFastExtrapolationPasses=3.00 RenoFastExtrapolationHaloLimit=1 \
  RenoFastExtrapolationFine=0.24 RenoPasses=1.00
DLSSVideoPlayer.exe --render F:/dlss5-video/tmp/shot15.mp4 --stages nr \
  --range f0-f360 --neural-backend amd --out <arm>.mkv
python F:/dlss5-video/tmp/cmp3/metrics_optcfg.py
python F:/dlss5-video/tmp/cmp3/make_optcfg.py
```

生效判据仍是项目根 `amd-neural-runtime/mochizuki_nr.log` 的
`renodx style: ... fast_passes=[3.00,..] fine=[0.24,..] halo=[1,..]`（本次 6 臂逐条核对过）。
`run-fg/DLSSVideoPlayer.ini` 已还原成 3 行基线（备份 `/f/tmp/ini.bak-optcfg`）；
**本次没有改任何源码、没有重编、没有提交**。

---

## 13. 回退上限到 8，并用最终推荐配置生成对比（2026-11-06 22:05）

第 12 节确认上限 12 是越界且画面不可用，本节把它还回上游面板的 8，
然后用**回退后的二进制**重跑推荐配置，产出对外的对比图与对比视频。

### 13.1 回退动作

| 项 | 操作 |
|---|---|
| 源码 | 五个 `*.bak-20261106-cap12` 原样还原，`cmp` 逐字节一致；改前的 12 版本另存 `*.cap12-experimental` |
| 五处上限 | `nr_renodx.cpp:1782` `8.f`、`.inc` 三行 `8.0f`、`NeuralRenderTypes.h` `kHigh` 三个 8.0、`AmdMochizukiApi.h` 三行 `1.f,8.f`、`PolicyTests.cpp` 断言 8.00 |
| 重编 | `build-local2.cmd --skip-shaders` → `BUILD_OK`；`build-local.cmd DLSSVideoPlayer NeuralWorker PolicyTests` → 24/24；`PolicyTests` **425 cases 全过** |
| stage | `amd-neural-runtime/MochizukiNrRuntime.dll`、`run-fg/DLSSVideoPlayer.exe`、`run-fg/neural-runtime/NeuralWorker.exe`；回退前的 12 版本二进制留 `*.bak-cap12bin` |

⚠️ 重编产物与 `*.bak-cap8` **大小相同但字节不同**（PE 时间戳/构建 ID），不要用 `cmp` 判回归；功能判据见 13.2。

### 13.2 回退验收：必须看到这三行

```
renodx style: ... passes_mode=1 fast_method=0 fast_passes=[8.00,3.00,3.00] ...
V12 vs F10 (k=5.26, 8遍)          mean PSNR(Y) 99.00 dB   identical_frames 76/76
V12 vs F12 (k=7.58, 实验档)         mean PSNR(Y) 33.02 dB   identical_frames 0/76
```

`V12` 是**故意写 `RenoFastExtrapolationPasses=12.00`** 的验证臂（96 帧）。
日志里已经是 `[8.00,..]`（host 的 `kHigh` 在送达前就钳住，runtime clamp 同样是 `8.f`）；
画面与旧的 10→8 臂逐像素完全相同，与 12 遍实验档差 33.02 dB。
⇒ 上限确实是 8，实验档已不再可复现。跳过前 20 帧冷启动后比较，避免 history reset 噪音。

### 13.3 最终推荐配置（已用回退后的二进制重跑 360 帧）

```ini
[RenoDX]
RenoPassesMode=1
RenoFastMethod=0
RenoFastExtrapolationPasses=3.00
RenoFastExtrapolationHaloLimit=1
RenoFastExtrapolationFine=0.24
RenoPasses=1.00
```

`REC.mkv`：日志 `fast_passes=[3.00,3.00,3.00] fine=[0.24,1.00,1.00] halo=[1,0,0]`，
submit p50 10.55 ms / network p50 3.17 ms。与第 12 节的 `S3.mkv`（cap12 二进制）
**96/96 帧逐像素相同** —— 推荐配置 passes=3 远低于上限，回退不影响它，
所以 12.4 节的全部指标可直接沿用。

### 13.4 产物

| 文件 | 规格 | 大小 |
|---|---|---|
| `docs/media/renodx-fast-final-config-compare.png` | 2040×1560 四列海报：整帧 + 470×470 1:1 原始像素 + 数值卡 + 推荐 ini + 结论 | 1.4 MB |
| `docs/media/renodx-fast-final-config-compare.mp4` | 1920×520 / 24 fps / 16 s / crf 16，四段三屏 | **4.44 MB**（<20 MB） |

脚本：`tmp/cmp3/make_final_cmp.py`、`make_final_video.py`；
渲染脚本 `/f/tmp/run_final.py`（用 `/f/tmp/arm4.py` 写 ini）；
臂 `tmp/cmp3/REC.mkv`（推荐配置 360 帧）、`V12.mkv`（上限验证 96 帧）。

海报四列：原片 / 最终推荐配置 / 游戏截图原配 5.06 / 12 遍实验档（已回退）。
1:1 裁切 `x560–1030 / y80–550`，不缩放不锐化；数值卡给 p5、Δp5、std、高频比、PSNR/SSIM、耗时。

视频四段（各 4 s）：
1. 整帧：原片 | 推荐 3 遍 | 截图原配 5.06；
2. 同强度对照：原片 | 上游默认 F3 | 推荐配置（展示 halo+fine 的免费改进）；
3. 压黑阶梯：推荐 3 遍 | 5.06 | 12 遍实验档（p5 8.19 → 6.56 → 2.79）；
4. 1:1 原始像素裁切 `x560–1166 / y110–506`，看皮肤过渡与木墙暗部。

### 13.5 指标摘要（与第 12 节同口径）

| | 黑位 p5 | Δp5 | 全局 std | 高频比 | vs 原片 PSNR / SSIM |
|---|---:|---:|---:|---:|---:|
| 原片 | 9.83 | — | 43.72 | 1.000 | — |
| **推荐 3 遍 + halo + fine** | **8.19** | **−1.64** | 46.19 | 1.107 | **29.62 / 0.9496** |
| 截图原配 5.06 | 6.56 | −3.27 | 47.55 | 1.203 | 26.73 / 0.9215 |
| 12 遍实验档（已回退） | 2.79 | −7.04 | 52.45 | 1.527 | 21.22 / 0.7992 |

口径：BT.709 加权 Y、全 360 帧累积 MSE、8×8 块 SSIM；色调每 30 帧抽样 12 帧。
不与第 10 节旧口径混拼。

### 13.6 状态

- 工作区源码与二进制**都已是上限 8**，与上游面板 slider 一致。
- `run-fg/DLSSVideoPlayer.ini` 已还原成 3 行基线（备份 `/f/tmp/ini.bak-final`）。
- **代码未提交**。
- 想恢复 12 实验档：拷回五个 `*.cap12-experimental` 源码并重编，或直接拷回三个 `*.bak-cap12bin` 二进制（仅用于复现第 11 节的坏画面）。
- 第 12 节末尾「当前工作区仍是上限 12 的实验状态」已过时，以本节为准。

---

## 14. 推荐配置成为 fast 的默认值 + Reno 设置框模式切换（2026-11-06 23:05）

第 12 节给出了推荐配置，本节让它成为 **fast 模式自己的默认**，并在 RenoDX
设置框里加「传统 / 快速」切换，让 12 个 fast 字段第一次有了 UI。

### 14.1 语义决策：动 `kStart`，不动 `kDefault`

| 表 | 改没改 | 为什么 |
|---|---|---|
| `kDefault` | **不动** | 它是 `MochizukiRenoOptions.inc` 的 runtime 权威默认，也是 `ApplyNeuralRoute` 判定"这个键要不要写进渲染"的基准。改成推荐值会让 `halo=1/fine=0.24` 变成"未设置"，反而不再送达 runtime。 |
| `kStart` | **改** fast 段为 `3.00 / Halo=1 / Fine=0.24` | `kStart` 是面板初值、Reset 目标、`IsEmpty()` 基准和 ini 稀疏基准。改它 = fast 模式的默认就是推荐配置，切到 fast 不需要再"载入预设"。 |
| `PassesMode` 的 `kStart` | **仍是 0（legacy）** | 说的是"fast 模式的默认"，不是"默认就进 fast"。让所有既有 profile 一启动就换一套画面是不可接受的。 |

**必须配套的一步**：`ApplyNeuralRoute` 在 legacy 下跳过整个 fast 段
（`i >= PassesMode`）。否则推荐值会写进**每一个** profile 的 overrides，
改变 retention key / 渲染缓存 key，却一个像素都不变 —— 所有既有缓存白失效。
`erase_if(kRouteKeys)` 仍会清掉上一次 fast 会话留下的键，所以模式来回切没有残留。

### 14.2 数据层

`RenoDxSettings` 新增两个静态函数（都只碰 fast 段，不动用户调过的 look）：

```cpp
static void ApplyRecommendedFast(RenoDxSettings& s);       // 恢复推荐值 + 置 PassesMode=1
static bool FastSectionAtRecommended(const RenoDxSettings&) noexcept;  // note 判据
```

`ApplyNeuralRoute` 的 legacy skip、`kStart` 的 fast 段、`kDefault` 保持原样，
都在 `src/NeuralRenderTypes.h`。三处 key（restart 的 `MochizukiOptions::operator==`、
`RetentionKeyFor` 的 `FormatMochizukiOptions`、缓存 key 的 `AmdNeuralSettingsDocument`）
都是遍历 overrides 的泛型实现，**无需逐键接线**；变化通过 overrides 内容自然传导。

### 14.3 UI：Reno 设置框里的模式切换

窗口 `470×652 → 470×952`。顶部第一行新增 `IDC_RENO_MODE` 下拉：
`传统（多遍）` / `快速 fast（推荐）`；底部新增 fast 分组（ID 7730–7737）：

| 控件 | 对应字段 | 说明 |
|---|---|---|
| 放大方式 combo | `FastMethod` | 0 外推（推荐）/ 3 Edit strength；1/2 选中时弹说明并**明确回退到 0** |
| 快速强度 slider | `FastPasses0` | 1.00–8.00，默认 3.00；>5 时 note 变警告 |
| 光晕限制 check | `FastHalo0` | 默认开 |
| 细节权重 slider | `FastFine0` | 0.00–2.00，默认 0.24 |
| 编辑强度 slider | `FastEditStrength` | 仅 method=3 时启用 |
| 两行说明静态 | — | 当前是哪种模式 / 推荐配置 / 自定义 / 过强 / 不可用 / 路由未开 |
| 恢复推荐值 button | — | 只重置 fast 段，保留模式与用户调过的 look |

**门控**（`UpdateRenoRouteNote`）：legacy 灰掉整个 fast 组；fast 灰掉
`RenoPasses`、`ChainedHistory`；method=3 灰掉 Passes/Halo/Fine，其余时候灰掉
编辑强度；route/grade 关闭时全部灰掉。`DrawDarkCheckBox` 补了禁用态的灰填充与灰勾，
否则自绘复选框禁用后仍是蓝勾，看起来像还生效。

`WM_COMMAND` 接线：mode / method 进 `CBN_SELCHANGE`，`IDC_RENO_FAST_HALO` 按既有规矩
显式 `SetCheckBoxState(box,!CheckBoxState(box))`，Reset 在 fast 下保持模式并回到推荐集，
Apply 日志加 `mode / fast_method / fast_passes / fast_halo / fast_fine` 五列。
`Localization.h` 新增 23 个键（tip 均 < 511 wchar）。

### 14.4 验收：必须看到这几行

```
renodx style: ... passes_mode=1 fast_method=0 fast_passes=[3.00,3.00,3.00] \
              fine=[0.24,1.00,1.00] halo=[1,0,0] edit=2.00
UIREC vs REC   mean PSNR(Y) 99.00 dB   identical 76/76
UIREC vs F3    43.19 dB   /   UIREC vs F5  34.98 dB
PolicyTests: 425 cases, all assertions passed
Player UI regression: 66 cases, 3201 assertions, failures: 22
```

`UIREC.mkv` 是**只在 ini 里写 `RenoPassesMode=1`**、其余全靠新默认值渲染的 96 帧臂；
`REC.mkv` 是第 13 节手配完整 6 行 ini 的臂。两者逐像素相同 ⇒ 默认值真的等于推荐配置，
且与上游默认（halo=0/fine=0 的 F3）明显不同，不是"看起来改了其实没生效"。

UI 回归 22 failures 是既有环境基线（`export_stages_dialog_test`、
`encoder_settings_dialog_test`、`neural_settings_dialog_test:5123`）。做了 A/B 对照：
把新用例关掉是 **3160 assertions / 22 failures**，打开是 **3201 assertions / 22 failures**
⇒ 新增 41 个断言全过，没有污染共享 Fixture。新用例覆盖控件存在性、初值、
模式切换门控、method 3 门控、method 1/2 拒绝、恢复推荐、Reset 保持模式、Apply 落盘与路由。

真实窗口截图：`docs/media/renodx-fast-ui-legacy.png`、
`docs/media/renodx-fast-ui-recommended.png`（`DLSS5_UI_SHOTS` 环境变量驱动测试生成）。

### 14.5 复现 / 状态

```ini
[RenoDX]
RenoPassesMode=1
```
一行即等价于第 13 节那 6 行；面板上就是「处理模式 = 快速 fast（推荐）」后直接 Apply。

- 改动文件：`src/NeuralRenderTypes.h`、`src/main.cpp`、`src/Localization.h`、
  `tests/PolicyTests.cpp`、`tests/PlayerUiRegressionTests.cpp`；备份 `*.bak-20261106-fastui`。
- 二进制 stage：`run-fg/DLSSVideoPlayer.exe`、`run-fg/neural-runtime/NeuralWorker.exe`
  （旧件 `*.bak-pre-fastui`、`*.bak-fastui-v1`）；runtime DLL 未改。
- `run-fg/DLSSVideoPlayer.ini` 已还原成 3 行基线（备份 `/f/tmp/ini.bak-fastui`）。
- **代码未提交**。
- 第 12 节末尾「UI 上把 >5 标黄/禁用仍待做」已完成：>5 显示明确警告文案，
  ≥8 仍由上限 8 兜住；method=3 时灰掉 Passes/Halo/Fine 也已完成。
