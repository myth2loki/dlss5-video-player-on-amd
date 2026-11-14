# 运动引导三臂对比：CPU 块匹配 / FidelityFX 光流 / 关闭（2026-11-14，RX 9070 XT）

一句话结论：**在片子最常见的运动量级上（1–4 px/帧），CPU 块匹配报告"没动"，
FidelityFX 光流报告得出来且准。** 真实片段上，FFX 臂解释了 58.5% 的帧间变化，
CPU 臂 38.5%，关闭引导 0%。

## 为什么不是"比 VMAF"

这条链路里，运动场只喂给 FSR 4 的历史重投影。画面好坏隔着上采样、锐化、时域累积
三层，用整片 PSNR/VMAF 比三个臂，等于用"改了多少"去回答"改得对不对"——
`BENCHMARK-METRIC-VALIDITY-zh.md` 已经为这件事踩过一次（97% 的低频带把中高频差异稀释掉）。

所以这里直接量**运动场本身**，也就是上采样器真正读到的那张纹理：

- 已知答案的素材上量 EPE（端点误差，DLSS 输入像素）、覆盖率、0.5 px 内命中率；
- 真实片段上没有真值，量 warp 一致性：把当前帧按场搬回上一帧，残差降多少。
  方向是 `prev(p) = cur(p − v)`，写反了不会显示成"符号错"，会显示成"场错"。

## 怎么量的

播放器加了一个 env 开关（`src/D3D12Renderer.cpp`，`DumpFlowField`）：

```
DLSS5_FLOW_DUMP=<目录>        # 每帧把 m_motion（RG16F，DLSS 输入尺寸）存盘
DLSS5_FLOW_DUMP_FROM=12       # 从第几个源帧开始
DLSS5_FLOW_DUMP_COUNT=12      # 存几个
```

`manifest.txt` 每行记 `帧号 宽 高 行距 源宽 源高 缩放x 缩放y 是否真的跑了估计器 CPU网格填充率`。
最后两列是判读的前提：没有它们，"场是零"分不清是估计器没找到还是展开环节丢了。

选臂用 `DLSS_VIDEO_MOTION_FLOW=cpu|ffx|off`。`off` 是这次新增的控制臂：
两个估计器都不起，guide pass 之后把场清零。

脚本已归档到仓库 `tools/flow-ab/`（`gen.py` / `run_arm.sh` / `analyze.py` /
`warp.py` / `truth.json`）。素材（`base.png`、`clips/`、`runs/`）不入库：一是体积
（约 1 GB），二是 `base.png` 为影片截图。复现顺序：

1. 准备 `tools/flow-ab/base.png`：从测试片源 60 s 处取一帧 1280×720，保留上下黑边
   与烧录字幕。`gen.py` 的 `plate()` 会裁 `[115:635, 0:1280]` 成 1280×520。
2. `python gen.py` 生成 `clips/*.mp4` 与 `truth.json`（`FFMPEG` 环境变量可指定 ffmpeg）。
   真机片段 `real60.mp4` 需自行从同一片源 60 s 起切 3 s，三臂共用。
3. `bash run_arm.sh <clip> <cpu|ffx|off>` 逐臂跑，产物落在 `tools/flow-ab/runs/<clip>/<arm>/`。
   `PLAYER_DIR` / `FLOWAB_RUNS` / `FLOWAB_CLIPS` 可覆盖默认路径；脚本用赋值前缀设环境变量
   （不要用 `env`，也不要用 `eval` 二次展开 Windows 路径）。
4. `python analyze.py --clip pan4 --arms cpu,ffx,off` 量已知答案；
   `python warp.py --clip real60 --arms cpu,ffx,off --calibrate` 先定帧偏移，
   再带 `--offset` 量真实片段。两个分析脚本同样读 `FLOWAB_RUNS` / `FLOWAB_CLIPS`，
   与第 3 步的覆盖值保持一致。

## 素材：为什么不能用 testsrc2

第一版素材用 ffmpeg 的 `testsrc2` 做平移。结果是**所有 SAD 估计器都合理地报告 0**：
周期图案平移 4 px 后，零位移仍然完全匹配，SAD 本来就已经是 0。那不是估计器差，
是素材欠定。

改用真实画面的静帧（《新喜剧之王》60 s 处，裁掉上下黑边到 1280×520，字幕保留——
字幕钉在屏幕上、真值是 0，正好惩罚乱报运动的臂）：

| 片段 | 真值 |
|---|---|
| `pan0p5`…`pan32` | 每帧整幅下移 0.5 / 1 / 2 / 4 / 8 / 32 px |
| `zoom` | 每帧绕中心放大 0.5% |
| `layers` | 底板不动 + 220×160 的贴片每帧右移 6 px |
| `real60` | 该片 60 s 起 3 s 原片（无真值，量 warp 一致性） |

全部 `libx264rgb -qp 0` 无损重编码，三臂读到的是同一批像素。

符号约定不假设，用 `pan4` 量出来：内容下移 4 px 时场是 `(0,−4)`。
帧对齐用一个全局整数偏移量对 `real60` 校准（残差对偏移量是干净的 V 形：
7.53 / 8.85 / 11.25 / 13.06 dB 级残差对应 −1 / 0 / +1 / +2），对所有臂用同一个值。

## 结果一：已知答案（EPE，DLSS 输入像素，越低越好）

`cov` = 有向量的像素占比，`within` = 误差 < 0.5 px 的占比。

| 片段 | 真值 | cpu EPE / cov | ffx EPE / cov | off EPE |
|---|---|---|---|---|
| pan0.5 | 0.50 | 0.502 / 0.0% | 0.558 / 45% | 0.500 |
| pan1 | 1.00 | 1.017 / 0.2% | **0.512** / 49% | 1.000 |
| pan2 | 2.00 | 2.086 / 1.1% | **1.036** / 48% | 2.000 |
| pan4 | 4.00 | 4.083 / 2.0% | **1.292** / 68% | 4.000 |
| pan8 | 8.00 | 5.767 / 40% | **2.793** / 65% | 8.000 |
| pan32 | 32.0 | 29.93 / 8% | 28.54 / 11% | — |
| zoom | — | 1.820 / 0.3% | **0.438** / 94% | 1.800 |
| layers | — | 0.274 / 1.1% | **0.166** / 2.8% | 0.230 |

读法：

- **CPU 臂在 4 px 以下几乎什么都不报**。`cov` 0–2%，EPE 等于"当成没动"。
  8 px 时才报（40% 面积，中位 −7.2 对真值 −8，差 0.8 px）。
  也就是说它有一条**约 5–6 px/帧的探测下限**，低于它宁可沉默。
- **FFX 臂报出来的地方是准的**：`within ≈ cov`，即"报了的基本都在 0.5 px 内"。
  它的误差几乎全部来自覆盖率（没报的像素），不是来自报错。
  覆盖率随位移增大而升高（1 px 时 49%，4 px 时 68%，zoom 时 94%）。
- **0.5 px/帧上谁也不比"没动"更好**（0.558 vs 0.500）。这个量级本来就在
  reprojection 的自由误差内，不该拿来当卖点。
- **layers 一栏值得单独看**：底板不动、贴片动 6 px。CPU 臂比关闭还差
  （0.274 对 0.230）——它既没找到贴片，又在静区报了 0.2% 的假运动。
  FFX 0.166，找到了贴片（cov 2.8% ≈ 腐蚀后的贴片面积），静区假运动 0.9%。

`pan32` 两个臂都不行（29.9 / 28.5），这条素材已经超出两者的有效范围，不代表优劣。

## 结果二：真实片段（warp 一致性，残差 0–255 灰阶）

`res` = 按场搬回去之后的残差；`base` = 完全不动时的残差。括号里是残差被解释掉的比例。

| 臂 | 覆盖率 | res vs base | 只在该臂声称有运动的像素上 |
|---|---|---|---|
| cpu | 41.4% | 11.17 / 18.15（**−38.5%**） | 15.08 / 32.00（−52.9%） |
| ffx | 94.8% | 7.53 / 18.15（**−58.5%**） | 7.55 / 18.73（−59.7%） |
| off | 0% | 18.15 / 18.15（0%） | — |

自检：同一套代码在 `pan4` 上，FFX 臂在它声称有运动的像素上把残差从 8.35 降到 0.12
（**−98.6%**）——场对了，帧间变化几乎能被完全解释。所以上面那 58.5% 不是工具上限。

代价（720p→2560×1440，FSR 4 在服务，24 fps）：

| 臂 | guide | render_path p50 | p95 | 掉帧 |
|---|---|---|---|---|
| cpu | 0.835 ms | 1.636 ms | 6.51 ms | 0 |
| ffx | 0.780 ms | 1.607 ms | 5.03 ms | 0 |
| off | 0.791 ms | 1.613 ms | 6.90 ms | 0 |

FFX 不比 CPU 块匹配贵（这里甚至略便宜，p95 更低）：一个是 GPU compute，一个是
CPU 上每格 7×7 的搜索。注意 `guide=` 这一列三个臂都差不多，因为它在三臂里量的
都是同一个 CPU 紧凑网格的生成——FFX 的 dispatch 记在帧自己的 list 上，算在
`render_path` 里。

## 结论

1. **AMD 上默认该用 FidelityFX 光流做运动引导**。2026-11-14 已按此把默认值定下来，
   落地方式见下面「默认值怎么落的」——从「NVOFA 先试、失败落 FFX」改成「按适配器
   厂商直接选 FFX」，AMD 上的行为不变，但默认不再依赖一次注定失败的探测。
   CPU 块匹配在片子最常见的慢速运动上等于没有引导，而它正是此前唯一的来源。
2. **CPU 臂不是"差一点"，是"在 4 px 以下沉默"**。这解释了为什么 AMD 上的
   FSR 4 时域累积在慢摇镜头上容易糊：guide 一直告诉它别动。
3. 关掉引导不是中性选项：真实片段上它让帧间 18.15 的残差一点都不被解释，
   这些残差全部落到 FSR 4 的历史里。
4. 0.5 px/帧这个量级不必争：两个臂都不比"没动"更好。

### 默认值怎么落的（2026-11-14 晚）

两处改动，都在 `src/D3D12Renderer.{h,cpp}`：

- `CreateDeviceAndSwapchain()` 把所选适配器的 `DXGI_ADAPTER_DESC1::VendorId`
  存进 `m_adapterVendorId`（`0` = 还没选到适配器）。
- 估计器选择从「先试 NVOFA，失败落 FFX」改为
  `tryNvof = (vendor == 0x10DE || 显式请求 nvof) && ...`：**NVIDIA 上顺序不变，
  其他厂商直接把 FFX 当默认**，不再先跑一次注定失败的探测。

为什么不是「保持 NVOFA 先试」：AMD 上每次会话都会打一行
`NVOFA unavailable: nvofapi64.dll is not present in the driver install.`。
这行在日志里读起来像故障，而它是预期路径；更要紧的是默认值当时是由一次**失败**
决定的，而不是由一次**选择**决定的——换机器、换驱动版本默认都可能漂。

四个臂一个都没动：`DLSS_VIDEO_MOTION_FLOW=cpu|ffx|off` 语义不变；
`nvof` 仍可在非 NVIDIA 上被显式请求（拿不到就回 CPU 块匹配，不跨臂），
这样「失败臂」本身仍可复现。

**验证（必须看到这三条）**——同一片段、不设任何 `DLSS_*` 变量跑播放：

```
grep -a "Motion guide backend" DLSSVideoPlayer.log
# 1) 期望：Motion guide backend: FidelityFX optical flow (compute, 8x8 blocks) ...
#     ; the default motion source on this adapter, whose vendor ships no flow engine.
# 2) grep -ac "NVOFA unavailable" DLSSVideoPlayer.log   → 期望 0（默认臂不再出现）
# 3) 同一条日志里必须能看到 SR 真的接了 FFX 的消费者：
#    Playback upscale: AMD FSR Upscaling 4.1.1 is serving the SR switch at
#    1280x720 -> 2560x1440，且 Playback health 的 guide= 在 0.6~0.8 ms
#    （没有这一行时 FFX 只是初始化成功，dispatch 一次都没跑，验证无效）
```

实测（RX 9070 XT，720p 实片）：后端行如上述、`NVOFA unavailable` 计数 **0**、
`1280x720 -> 2560x1440`、`guide=0.63~0.79 ms`、`dropped=0`。
对照臂 `DLSS_VIDEO_MOTION_FLOW=nvof` 仍打 `NVOFA unavailable: ...` 并落到
`CPU block matcher, because the NVOFA arm DLSS_VIDEO_MOTION_FLOW named did not
come up for the decoded frame`。

没动的：ini `[NeuralGuides] MotionVectors` 默认仍是 `1`（引导总开关，与选哪个
估计器是两件事）；`DLSS5_FFX_OF_FORCE` 仍是构建自检开关。

## 没有证明的

- **没有整片画质指标**。FFX 臂的场更好，不等于 FSR 4 的输出一定更好看——
  中间还隔着 FSR 4 自己的重投影与钳制。真要回答"画面差多少"，得把输出帧也存下来
  做逐帧对比（目前播放器只能 dump 场，不能 dump 输出），这不在本次范围内。
- 单卡（RX 9070 XT）、单素材（一部片的两个镜头）、1280×520→2560×1040。
  分辨率更高时 CPU 网格更细，探测下限会变；这里没有测。
- `pan32` 超出两者范围，没有结论。
- 真实片段的 58.5% 是"帧间变化被解释的比例"，不是 VMAF，也不是画质分数。
- 帧对齐靠一个全局整数偏移量校准（对 `real60` 是 −1），不是从解码器读出来的帧号；
  播放器在 SR 会话里没有把 `FrameIdentity` 传给渲染器。

## 阶段 2：输出帧 A/B（2026-11-14）

上面第一条要回答的问题只有一个：**更好的引导场，有没有让 FSR 4 的输出帧更好**。
下面是动手前定好的方案；`tools/flow-ab/output_metrics.py --selftest` 先验证指标能区分
成败，通过后才动的产品代码。

### dump 什么

FSR 4 的输出是 `m_upscaleSharp`（`R16G16B16A16_FLOAT`、线性光、`m_outputW × m_outputH`），
在 `RecordUpscalePresent()` 末尾回到 `PIXEL_SHADER_RESOURCE`，present 从
`SRVGPU(UpscaleSRV)` 采样它。dump 它，不 dump swapchain。

**不能用 `CaptureComposedView()`**：`RecordViewDraw()` 在默认分支只判
`m_lastDLSSUsed && DLSSEnabled()`，AMD 上 `DLSSEnabled()` 恒 false，于是绑
`SRVGPU(4)`（源尺寸 `m_dlssColor`），拿不到 FSR 4 结果；而且它按窗口客户区尺寸出图，
不是输出分辨率。顺带一提这也是「保存对比图」在 AMD FSR 4 路径上的一个既有缺陷，
但本阶段不改产品行为——只加一条独立的只读诊断路径。

已实现：`DLSS5_OUTPUT_DUMP=<dir>`（另有 `DLSS5_OUTPUT_DUMP_FROM` / `_COUNT`，默认
借用场 dump 的 FROM/COUNT），写 `out_NNNNN.rgba16f` + `manifest.txt`。挂点是两个
`RenderFrame()` 重载里 `RenderFrameInternal()` 成功返回之后（帧 list 已执行、present
已完成），照 `DumpFlowField()` 的独立 command list + `WaitGPU()` + 有界帧数实现。不进
任何渲染缓存 key / restart 条件，因为它是纯诊断、不改变任何像素。`run_arm.sh` 加第三
参数 `field|output|both` 选择 dump 哪一类，`both` 让两者共用同一帧序号便于配对。

**manifest 现在是 9 列**：`index w h rowPitch flowDumpIndex frameNumber pts100ns
historyGeneration reset`。前 5 列是初版，后 4 列是阶段 2 复测加的源帧身份：
播放路径走的是没有 `FrameIdentity` 参数的 legacy `RenderFrame()` 重载，所以产品侧
加了 `SetNextFrameDumpIdentity()`，由 main 在渲染前一次性盖章（`m_dumpFrameId` 用完即
清零，忘了盖章的调用方写 0 而不是沿用上一帧的身份）。有了 `frameNumber` 就不用再
用 MSE 猜全局整数偏移；`historyGeneration` / `reset` 还能直接查出一次 dump 中途发生
的时域历史重建（见下文「脏臂」）。列追加在末尾，旧的 5 列读取器仍兼容。

### 真值怎么来

用仓库里 1920×1080 的真实采集片段 `tools/benchmark/fixtures/demo-capture-20260912.mp4`
做高分辨率真值：抽帧存 `truth_NNNNN.png`（1920×1080），再用 2× 降采样生成
960×540 的输入片段，播放器 FSR 4 升回 1920×1080。这样**有真实高分辨率 ground truth**，
不是拿另一个臂当参照，也不是拿 `testsrc2` 那种周期图案（平移后零位移也完全匹配）。
合成镜头按输出分辨率 1/2/4/8 px 平移（对应输入 0.5/1/2/4 px），正好落在阶段 1
测出「CPU 沉默、FFX 有效」的区间。`gen_truth.py` 生成 6 个镜头 × **96 帧**：4 个合成
平移（同一真实帧做反射扩边后裁切，不引入黑边）+ 2 段真实影片运动（全片按 6 帧步长
扫 luma 差，自动挑最平稳的 96 帧窗口，避开未标注的切镜；旧锚点 300/470 在 36 帧时
够安静，拉到 96 帧会跨切镜、cost 32.6）。输入片段用 `libx264rgb -qp 0` 无损 RGB；
**降采样在线性光域做**（sRGB→linear→2× box→sRGB），与播放器 `SRGBToLinear`
后 FSR 4 的工作域一致，否则会给三个臂统一烘进一层色域偏差。

拉长到 96 帧是为了把 dump 起点推到 `from=60`，等 FSR 4 时域历史收敛：同配置噪声
地板随预热单调下降（`o_pan2` 在 from=12 是 2.08 dB、from=36 是 0.369 dB、
from=48 是 0.074 dB、from=60 是 0.0000 dB）。冷启动历史状态不同是噪声主因，
逐帧差异呈单调衰减而不是随机抖动。`ffmpeg` 默认路径改为仓库内
`external/ffmpeg/bin/ffmpeg.exe`（原写死的 `F:\rvc\...` 在本机不存在），可用
`FFMPEG` 环境变量覆盖。

### 指标（`tools/flow-ab/output_metrics.py`）

| 轴 | 定义 | 为什么需要 |
|---|---|---|
| `band_mid` / `band_high` | 对真值的分带 PSNR（复用 `tools/benchmark/bands.py` 的三带分解） | 低频带占 97–99% 预算，head-line PSNR 会把中频的真实改善稀释成 0.02 dB |
| `detail` | 输出高频 RMS ÷ 真值高频 RMS | 挡住「更糊所以更稳」——flicker/SSIM/dE 对平滑都是单向友好的 |
| `static_flicker` | 真值判定为静止的区域上，输出亮度的二阶时间差（4× 池化） | 时域闪烁，一帧看不出来 |
| `ghost_leak` | 输出误差在「上一帧 − 当前帧」方向上的投影，只在真值判定发生变化的像素上 | 读数是旧帧残留比例；10% 拖影就读到约 0.10 |

不拿 `off` 臂当「正确」的参照：零运动向量会让 FSR 4 干脆丢掉历史，时域上反而更干净，
单看闪烁会赢。只有对真值的中/高频正确性才能判这个。

### 自检结果（`--selftest --size 960x540`，2026-11-14）

在真实影片静帧上注入已知强度的退化，要求指标单调响应、且能识破陷阱：

| 注入 | ghost Δ（对照） | flicker | detail | head-line |
|---|---|---|---|---|
| 干净 / 完全相同的臂 | 0 / 0 | 0.316 | 0.515 | 27.943 |
| 拖影 10% / 20% / 30% | +0.077 / +0.155 / +0.232 | — | — | 27.854 / 27.677 / 27.422 |
| 闪烁 1 / 2 / 4 | — | 4.128 / 8.069 / 16.022 | — | 单调下降 |
| 模糊 σ=0.5 / 1 / 2 | — | 0.259 / 0.146 / 0.130（**更低**） | 0.451 / 0.298 / 0.126 | 单调下降 |
| 闪烁 4 + 模糊 σ=1（陷阱） | — | 6.622（比 16.022 低） | **0.309** | 26.256 |

7 项断言全过：拖影单调且读数≈注入比例、闪烁单调、模糊被 `detail` 抓住、
「模糊闪烁臂」虽然 flicker 变好但 `detail` 掉到 0.309、未改动臂完全无差异。
第一版自检确实抓出两个设计错误（ghost 方向写反、闪烁用整帧常量偏移导致模糊消不掉），
改完才通过——这正是先验证再写产品代码的原因。

### 判决规则（沿用 `docs/BENCHMARK-METRIC-VALIDITY-zh.md`）

- 不用整片均值：**≥2/3 镜头同号，且每镜头 |Δ| > 0.05 dB**（mid+high 分带）才算数。
- `detail` 掉过 0.2% 的噪声底线，或 `ghost_leak` / `static_flicker` 反向变差，
  一律不判「更好」，只报 trade-off。
- 三臂（`ffx` / `cpu` / `off`）同配置复跑取噪声地板；对照臂换组合重跑，
  避免把冷启动差异当回归。
- 结论只写「在哪些镜头、哪个频带、差多少」，不写「画质更好」这种没有对应测量的话。

### 实施中实测到的两个前提修正

1. **输出尺寸是 rung 盒子，不是「2× 源」。** `UpscalingTarget()` 按
   `UpscaleRungWidth(height)` 给出的 16:9 盒子 fit 源：实验 ini 里
   `UpscaleHeight=1440`（盒子 2560×1440）会把 960×540 放大到 2560×1440（2.667×），
   而阶段 1 的 1280×520 是宽度受限的 2× → 2560×1040。要拿到与真值逐像素对应的
   1920×1080，跑批前把实验 ini 设成 `UpscaleHeight=1080`（已备份、跑完恢复）。
   这不是产品缺陷，是「真值必须和输出同尺寸」这条测量前提没先验证。
2. **帧偏移必须逐镜头量。** `output_metrics.py --calibrate` 扫 −3…+3 的全局整数
   偏移；`o_pan2/ffx` 量出 **+1**（offset 0 的 head PSNR 24.08 dB，+1 是 28.61 dB），
   而阶段 1 的 `real60` 是 −1。方向和大小都不固定，绝不能把 dump 的文件号直接
   当解码帧号用。

> **以下两节（「INCONCLUSIVE」与「下一步先修测量」）是 11-14 第一轮的历史记录，保留
> 原样以便追溯。复测后的最终结论见「阶段 2 复测」一节。**

当时 `diagnose_alignment.py` 先逐臂量偏移、并用「同一 ffx 臂跑两次」量噪声地板；
`summarize_output.py` 再对每个臂用**自己的**偏移对真值评分，把判决规则写成代码
（`summary.json` 里带每条 Δ、每臂偏移、噪声地板与 trade-off 原因），避免人工挑数。
复测后这两个脚本改为按 `frameNumber` 取公共连续帧段对齐，MSE 偏移只剩交叉校验。

### 结果：本轮无法判定（INCONCLUSIVE，2026-11-14）

6 镜头 × 3 臂全部跑通（18 次播放器运行、每次 12 帧），dump 管线、真值方案、
颜色转换、四轴指标都验证可用。**但这一轮不能回答「场更好是否画面更好」**，
原因是两个测量前提在跑批之后才暴露出来，且都不是换指标能绕过的：

**1. 跨臂帧偏移不一致，共享偏移会造出假差异。** `o_pan1` 上 ffx 的最佳偏移
是 +0、cpu/off 是 +1。第一版汇总按 ffx 定一次偏移套给所有臂，得到
`Δ band_mid = +0.84 dB`（看似 FFX 更好）；改成每臂独立校准后**翻转成 −1.13 dB**。
方向都能反，那个数就不是画质。已修 `summarize_output.py`。

**2. 同配置噪声地板远大于判决阈值。** `o_pan2` 的 ffx 臂原样复跑两次：
`band_mid` 差 **−2.0844 dB**、`detail` 差 −0.0839；两次的 `out_00012.rgba16f`
**md5 不同**，全分辨率校准分别给出 +1 与 0 的偏移。也就是说播放器每次冷启动的
时域历史 / 丢帧状态不同，同一配置本身就有 2 dB 级波动，而预设阈值是 0.05 dB。
`o_real_a` 两次复跑则逐字节相同（该段 FSR 4 输出确定），说明噪声来自运行态而非指标。

修正后（每臂独立校准）的 `ffx − cpu`：

| 镜头 | Δ band_mid | Δ band_high | Δ detail | 判读 |
|---|---|---|---|---|
| `o_pan1` | −1.1309 | −0.1942 | +0.00595 | trade-off（flicker） |
| `o_pan2` | +0.4928 | −0.1060 | +0.03854 | trade-off（flicker） |
| `o_pan4` | +2.5442 | +0.0308 | +0.08678 | high 带未过阈值 |
| `o_pan8` | −0.4682 | −0.1375 | +0.10159 | trade-off（flicker） |
| `o_real_a` | 0 | 0 | 0 | 三臂输出字节级相同 |
| `o_real_b` | +1.7979 | +0.1245 | −0.00244 | trade-off（detail） |

`o_real_a` 三臂的 `out_00012.rgba16f` 是同一个 md5，而 `o_pan4` 三臂各不相同：
平稳真实片段里 FSR 4 根本没吃到引导场差异，合成平移段才吃得到。`o_pan4` 的
`off` 臂 `band_mid 37.44` 反而高于 ffx 34.97 / cpu 32.43，再次印证零运动臂
不能当「好」的参照（它会丢掉历史，时域更干净）。

`summary.json` 的结论是 **INCONCLUSIVE**：噪声地板 2.0844 dB > 0.05 dB 阈值，
臂间差异与运行间噪声不可分离，`0+ / 0− / 6×0`。按预先写定的规则，
这里**不写「画质更好」也不写「没有差异」**——测不准和没差异是两回事。
阶段 1 关于场本身的结论不受影响：它量的是引导场，不是输出帧。

### 要回答这个问题，下一步先修测量而不是加指标

1. **根治对齐**：把 `FrameIdentity`（解码帧号 / PTS）写进 output dump 的
   `manifest.txt`。产品里已经有这个标识，只是 SR 会话没把它传到渲染器的
   dump 路径；有了它就不必再用 MSE 猜全局整数偏移。
2. **压住冷启动噪声**：dump 起点推到预热之后（如 `FROM=36`），每个臂跑
   3 次取中位数，或用单进程内切换 backend 并显式重置时域历史。
3. 备选（不依赖改产品）：真值角落烧入逐帧编号，用模板匹配无歧义对齐。

修完这两条再重跑同一批脚本；只有噪声地板降到 0.05 dB 以下，
`summarize_output.py` 的判决才有意义。

## 阶段 2 复测：可判（2026-11-14 第二轮）

按上一节的三条修完测量后重跑同一套脚本。这次**判决有效**，不再是 INCONCLUSIVE：

| 门禁 | 结果 |
|---|---|
| 身份/偏移 | **PASS**：18 个臂（6 镜头 × 3 臂）全部 `best_shift=0`，PTS 与 `frameNumber` 偏差仅 0.7 tick（< 1 帧） |
| 噪声地板 | **0.0284 dB**（6 镜头同配置复跑的最大值，`o_pan2`），**< 0.05 dB 判决阈值** |
| 有效公共帧 | 每镜头 12 帧，解码帧号 61–72，三臂完全一致 |
| 判决 | `1+ / 0- / 5×0`，需 4 个同号 → **no measurable output-frame difference under the pre-registered rule** |

`ffx − cpu` 逐镜头（正值=FFX 更好）：

| 镜头 | Δ band_mid | Δ band_high | Δ detail | 判读 |
|---|---|---|---|---|
| `o_pan1` (1 px/帧) | **−2.9372** | −0.2731 | −0.03488 | 0 · trade-off: detail,flicker |
| `o_pan2` (2 px) | +2.7301 | −0.2801 | +0.15410 | 0 · trade-off: flicker |
| `o_pan4` (4 px) | +3.3272 | −0.0300 | +0.13792 | 0 · 双带未同时过阈 |
| `o_pan8` (8 px) | **+3.4408** | +0.1549 | +0.11283 | **+** · 双带 > 0.05 dB |
| `o_real_a` | +0.0319 | +0.0267 | −0.00460 | 0 · trade-off: detail,ghost |
| `o_real_b` | +0.0045 | +0.0016 | +0.00002 | 0 · 无可测差异 |

**怎么读这个结果**（不要把「无显著差异」误读成「FFX 没用」，也不要反过来）：

- 中频带确有大幅改善，且方向随运动幅度系统性变化：2/4/8 px 平移上 FFX 比 CPU
  高 **+2.7~3.4 dB**（远超 0.0284 dB 噪声地板，是真信号）；但 1 px/帧上反而
  **−2.94 dB**。这与阶段 1「CPU 在 <5~6 px/帧 有探测下限」的结论一致又互补：
  慢速运动下 FFX 的整像素向量会把小位移报偏，重投影反而伤画质。
- **高频带几乎没跟上**（除 `o_pan8` 外都不是正增益），且 `detail` / `static_flicker`
  的 trade-off 守卫在 5/6 镜头触发。预先写定的规则要求 mid+high 双带同向过阈
  且守卫不响，所以只有 `o_pan8` 拿到 `+`，凑不满 ≥2/3 的门槛。
- 两段真实影片运动三臂几乎逐字节相同（`o_real_b` 三臂 `band_mid` 40.307/40.3025/
  40.3025），说明平稳真实片段里 FSR 4 没吃到引导场差异；收益只出现在合成平移段。
  这限制了结论的外推范围，**不能据此说实片画质会变好**。
- 判决规则是跑批前写死的，本轮没有为了凑结论调整任何阈值或守卫。

### 复测中新增的一条教训：脏臂要按 manifest 查，不能只看帧数

全量 36 次运行里，第一遍有 3 个臂（`o_pan2/ffx`、`o_pan8/cpu`、
`o_real_a/cpu`）虽然都写满 12 帧，但 manifest 显示 `reset=2`、
`historyGeneration` 3→4、解码帧号从 63 跳到 76（或 64→82、63→75）；日志里
对应 `ffmpeg: [vf#0:0] Error reinitializing filters!`。这是播放器冷启动时解码器
seek 后滤镜重初始化失败的**偶发故障**，导致跳帧 + 时域历史从头开始。

这类臂**不能靠放宽门禁救**：跳跃帧会让 `static_flicker` / `ghost_leak` 把
「63→76」当成一次巨大运动，指标直接失效；而且它的时域历史状态与别的臂不可比。
正确做法是拿 `frameNumber` 连续性 + `reset` + `historyGeneration` 自动检出脏臂并
补跑（这 3 个臂各补跑一次即干净）。「每臂都写出 12 个文件」不等于「12 帧可用」，
**自检报告要写「必须看到哪一行」而不是「跑通即可」**——这次又是靠 manifest 的新列
一眼看出来的。

### 复现顺序（复测版）

```bash
# 实验 ini: Playback/UpscaleHeight=1080（跑完恢复 1440；本轮已恢复并逐字节比对备份）
# F 盘空间不足时把产物放 C 盘：dump 约 10 G，且 run_arm.sh 的 trash 也要指过去
python tools/flow-ab/gen_truth.py --out 'C:\Temp\dlss5-stage2\outputs'   # 6 镜头 × 96 帧
FLOWAB_OUTPUTS=/c/Temp/dlss5-stage2/outputs \
FLOWAB_TRASH=/c/Temp/dlss5-stage2/trash \
FLOWAB_PASSES=2 bash tools/flow-ab/run_output_ab.sh        # 36 次，第二遍量噪声
python tools/flow-ab/diagnose_alignment.py --out 'C:\Temp\dlss5-stage2\outputs'  # 出 alignment.json
python tools/flow-ab/summarize_output.py --out 'C:\Temp\dlss5-stage2\outputs'    # 再判
```

路径注意：本机 Python 是原生 win32 构建，POSIX 形式 `/c/Temp/...` 会被解析成
「当前盘符 + `\c\Temp\...`」。`--out` 给 Python 时必须用 Windows 风格；shell 脚本
内部用 POSIX 形式，读 `truth.json` 前先 `cygpath -w` 转换。

复测产物全部落在仓库外的 `C:\Temp\dlss5-stage2\`，不入库；仓库内
`tools/flow-ab/{clips,runs,trash,outputs}/` 已在 `.gitignore` 内。

## 阶段 3：实片冒烟（2026-11-14 第三轮）

阶段 2 的结论只覆盖合成平移段，文档自己写了「不能外推实片」。这一段补的是实片，
但**没有真值帧**，所以这里不产生 PSNR、也不产生判决词，只回答四个可回答的问题：
三臂是否都真的渲染了、停在哪几帧、彼此差多少、差在画面内还是黑边上。

素材：《新喜剧之王》720p 源（`1280x720`，24 fps，片源自带上下黑边），用
`ffmpeg -ss 600 -t 30 -c copy` 无损切出 30 s 剧情段，dump 起点 60 帧、12 帧。
脚本：`tools/flow-ab/run_real_probe.sh <clip> [outdir]`，报告由
`real_probe_report.py` 生成（`real_probe.json` + PNG + 5× 放大对照）。

| 项 | 结果 |
|---|---|
| 三臂是否挂上 | 是。FFX 报 `FidelityFX optical flow (compute, 8x8 blocks) on the decoded 1280x720 frame`，CPU 为块匹配，off 为零场 |
| SR 通路 | `AMD FSR Upscaling 4.1.1` 服务 SR 开关，`1280x720 -> 2560x1440` |
| dump | 每臂 12 帧，无 `Error reinitializing filters!` |
| 公共帧 | 仅 **61、75** 两帧（各臂丢帧位置不同，CPU 臂丢 47 帧、FFX 臂丢 8 帧） |

帧 61（三臂都有）内容区（已排除黑边与边界 8 行）差异：

| 对比 | mean | p99 | p99.99 | max | >6 级占比 | 亮部 max |
|---|---|---|---|---|---|---|
| `ffx` vs `cpu` | 0.364 | 2.00 | 7.00 | 34 | 0.0139% | 24 |
| `ffx` vs `off` | 0.381 | 2.00 | 8.00 | 24 | 0.0224% | 24 |

帧 75 上出现了阶段 1 就预言过的现象：**`cpu` 的输出与 `off` 逐字节相同**
（md5 一致），即 CPU 块匹配在这一帧报了「没动」；`ffx` 则不同（mean 0.760、
p99 6、>6 级占 0.92%、亮部 max 62）。但它**不是整体位移**：相位相关给出的
ffx↔cpu 位移是 `(-0.008, +0.007) px`（response 1.017），差异散布在 431 个
32×32 块里，属于重投影层面的局部差别。5× 放大看，差在失焦高光的边缘——
FFX 那版边缘略硬、带一点青绿色边，CPU/OFF 更软更中性。

**必须同时说的三件事**：

1. 全帧 `max=91.7` 那个数**不是伪影**：它出现在片源黑边与画面的分界行上，
   排除黑边后内容区最大差降到 34/24 级（帧 61）。任何拿全帧 max 说事的结论都错。
2. `dropped` 不能当性能指标：三臂 `dropped` 是 8 / 47 / 32，但同一份日志里
   `guide=` 耗时是 **0.65 / 0.55 / 0.58 ms**（后续样本 0.55–0.81 ms），三臂几乎相同。
   丢弃数被 dump 的逐帧 GPU drain 和冷启动抖动主导，用它判「谁更贵」会得出
   与 guide 耗时相反的结论。
3. 这一轮**没有**证明 FFX 在实片上更好，也**没有**证明更差。它证明的是：实片
   路径能跑、FFX 确实在真实解码帧上生效、输出没有可见鬼影，且与 CPU 的差别量级
   在 1% 像素以内、集中在失焦区。

### 这一轮踩到的两个实现坑

- `"C:\Temp\dlss5-real\$arm"` 里的 `\\$` 会被当成转义，三臂全写进一个字面名为
  `dlss5-real$arm` 的目录并互相覆盖。路径一律走 `cygpath -w`，别手拼反斜杠。
- `outputs/trash` 那类目录 `rm` 后会进回收站，**同一个盘不释放空间**（`df` 不动，
  回收站变大）。要真正腾空间得用 `[System.IO.File]::Delete` /
  `[System.IO.Directory]::Delete(...,$true)` 之类的永久删除，且必须按白名单
  逐项核对，别整盘清。
