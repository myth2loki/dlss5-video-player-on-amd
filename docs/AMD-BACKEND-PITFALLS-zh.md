# AMD 后端踩坑清单（改代码前必读）

_最后更新 2026-10-22（a97e632 之后）。每一条都对应一次真实的错误、一次误判或一次实测。_

这份文档不是架构说明，是"已经踩过的坑"清单：症状 → 为什么 → 怎么改。
改 AMD 相关代码之前先扫一遍第 1 节和第 10 节；改引导、配置键、UI 时再看对应小节。

**通用心法：占位值不是无害的。** 一个 1×1 的零纹理、一个 `0.0f` 常量、一个"先绑个有效纹理免得调试层报错"的注释，
都在悄悄定义行为。看到它们先问：这个输入原本应该是什么？谁把它替换成了常量？

---

## 1. 铁律：AMD 的改动不许碰到 NVIDIA

这是硬约束，不是风格建议。

- backend 由 `src/main.cpp` 选：默认 `Nvidia`，只有"找到并验证过 AMD 运行时包"才切 `Amd`。
- 新增逻辑一律在 `if (request.backend == Amd)` 分支里**早退**；NVIDIA 臂逐字不动。
- **共享文件禁区**：
  - `src/TemporalStabilityShader.h` —— NVIDIA capture 路径也用，改它等于改 NVIDIA。AMD 侧要不同行为就在 evaluator 里换绑定/换常量。
  - NGX / feature-18 / FSR1 / CAS 相关代码。
- 播放超分回退链 **FSR4 → FSR1(EASU+RCAS) → CAS** 不得破坏、不得收窄。状态栏显示 FSR1 是预期降级，不是 bug。
- 生成帧必须与源帧同引擎同画质（cadence 两帧对等）；失败 latch 回合成器普通拉伸。

**自检**：`git diff --stat` 里出现 nvidia/ngx/capture 相关文件，就得在提交信息里说明为什么必须动它；说不清就是不该动。

---

## 2. 引导与运动场（最容易踩，代价最高）

### 2.1 一张"占位"纹理可能就是这个 pass 的全部行为定义

**症状**：打开神经渲染后拖影严重；关掉插帧（Interpolate）完全没变化。

**原因**：`AmdLmxxfEvaluator` 把时域累积 pass 的 **t2（运动）绑成了 1×1 的 `zeroMotion_`**，
常量 `MotionToUv = 0`。而 HLSL 里是

```hlsl
previous = uv + motion * MotionToUv;   // 零场 + 零缩放 → 恒等于 uv
```

于是它退化成**同屏幕位置的递归 EMA**：历史帧不位移，只靠 source luma trust（3/255~10/255）去挡。
慢/中速运动时同一像素变化不够大 → trust 部分通过 → 上帧内容残留 = 拖影。
`High` 档权重 0.70，所以最严重。`TemporalStabilityShader.h` 文件头原文就写着 "ghost on anything that moves"。

**改法**（c5b1888）：新增 `accumulateMotion_`（场尺寸 `motionW_ × motionH_`，`R32G32_FLOAT`）+
`RecordAccumulationMotion()` 每帧把场 copy 进去，`MotionToUv = (1/motionW_, 1/motionH_)`。
无场时仍回退 `zeroMotion_` + 零 scale（旧行为）。

### 2.2 不能直接把 `motion_` 绑给消费方

`motion_` 这张引导纹理**永远是帧尺寸**——runtime 拒绝比帧小的引导。场只写在**左上角** `motionW_ × motionH_` 区域。
而累积 pass 按 `uv * textureSize` 采样 t2，直接绑过去会把左上角那小片场当成整帧来读，画面全乱。

**所以**：要另建一张"就是场"的纹理，每帧 copy 一次（1920×822 / 160×68 = 87 KB，可以忽略）。

### 2.3 场的数值单位是"场自有像素"，不是帧像素

`UploadGuides` 里已经用 `motionUnitX/Y` 换算过；runtime 内部用 `motion_width` 归一化。
所以 AMD 侧的归一化必须用 **`1/motionW_`，不是 `1/frameWidth_`**——两者等价只在场等于帧尺寸时成立，
写错会让重投影位移差一个 `frameW/motionW` 的倍数（视觉上就是"拖影还在，只是方向对了一点"）。

### 2.4 状态机要对称

`motion_` 平时声明为 `kGuideState`。拷贝前后必须切回原状态：

```
kGuideState → COPY_SOURCE → 拷贝 → kGuideState
```

否则帧信息声明的状态和实际不符，runtime 与下一帧看到的都是错的状态。

### 2.5 `DLSS5_AMD_GRID_GUIDES=0` 不是拖影的解药

它只影响 **worker 侧上传给神经网络的 MV**（160×68 网格）。
**FSR4 和 FFX 帧生成用的是播放器侧全分辨率的 `m_motion`**，和这个键无关。当年差点在这上面浪费一个下午。

同理：`DLSS5_FSR4_GUIDES=0`（FSR4 用恒等重投影）和 `DLSS5_FG_GEN_UPSCALE=0`（生成帧不走 FSR4）才分别对应那两级。

### 2.6 场尺寸会变

`accumulateMotion_` 必须跟着引导一起重建，并在 extent 变化时重写描述符槽位（`RecordAccumulationMotion` 现在就是这么做的）。
只建一次、之后只 copy，遇到分辨率切换会读到错尺寸的纹理。

---

### 2.7 场的精度不是拖影的开关（2026-10-22 实测，别再为此写光流）

直觉上"场越准越好"，但在这条链上**错的场不会拖影，只会少稳**。机制：trust 门比较的是
重投影位置上两帧的 luma 差——场错了，重投影位置对不上，差值大，门关掉，这个像素就不累积。
**所以场的精度是"稳定覆盖率"的旋钮，不是"干不干净"的旋钮。** 真正会拖影的是"零位移但门开着"（2.1）。

实测（720p，`--stages nr --neural-backend amd --passes 1 --temporal-stability off|high`，
拟合 own / t-1 / t-2 / t-3，边界带 = 运动前景外扩 26-40px 减内部）：

| 素材 | 场能否描述 | ghost t-1/t-2/t-3 | 细节（off 为 100%） |
|---|---|---|---|
| 整帧平移 −10px/帧 | 全局搜索命中 | 0.000 / 0.000 / 0.000 | 98% |
| 前景 +20px/帧 + 背景 −3px/帧（含遮挡边界） | 全局+局部命中 | 0.010 / 0.004 / 0.002 | 99.3% |
| 前景 +55px/帧（≈ 全局量程 ±70px 内） | 全局命中 | 0.009 / 0.005 / 0.006 | 99.3% |
| 前景 +90px/帧（超全局量程） | 失配 → 退零位移 → 门关掉 | 0.006 / 0.005 / 0.004 | 99.3% |

对照：旧的零位移绑定在这套素材上是 t-1 ≈ 0.20、细节只剩 59%。

**顺带记住两个量程**（都在 `TemporalGuides.cpp`）：
- 全局搜索 `globalRadius = 7` **格** → 1080p 约 ±70px，4K 约 ±168px（格更大）；
- 局部细化 `localRadius = 3` 格，只覆盖"相对全局向量的差分运动" → 1080p 约 ±30px。

**推论**：加密网格是陷阱。格变小，±7 格/±3 格的**像素**量程同比缩小，成本按格数平方涨——
等于用一个问题换另一个。要更大运动量程就扩 `radius`，不要加密。
真要提升场精度，收益在别的消费方（FSR4 长历史重投影在 12px 格下的边缘爬行、4K 内容），
**不在这个 pass**；而且先做"用 depth guide 拒绝遮挡边界"比写光流便宜两个数量级。
代价参考：项目里已有的 8×8 GPU matcher（`FrameGenFlowShader.h`）在 1080p 是 **~20 ms/帧**、
`kSearch` 二次方缩放，而当前一次 submit 已 ~15ms、网络 3-7ms——实时播不动，只适合离线导出。
上游那句注释也值得记住：更细的场"on screen it did not"去掉鬼影，这是他们后来转向 FidelityFX 的原因。

### 2.8 场估不出来时，重投影修不了拖影（2026-10-23 复核）

平坦/低纹理画面（渐变背景、纯色墙面、模糊焦外）上，CPU 块匹配**一个向量都不给**。
`smooth-pan`（渐变 + 柔和亮斑，−3 px/帧）实测每帧场值都是 `min=0 max=0 mean=0`；
场为零 → `previous = uv` → 又退回"同位置 EMA"，而这类画面同位置差异本来就小、trust 门大开
——**拖影最容易被看见的地方，恰好是重投影帮不上的地方**。

同一条链上有纹理的素材（`pan`，−10 px/帧）场是 `max=1 mean=0.446`（1 格 = 10 px @720p），
修好的绑定立刻见效：自身项 0.913→0.972、细节 92.5%→97.9%。

**诊断顺序**：先抓场值分布（`UploadGuides` 里把 `motionValues` 的 min/max/mean 落盘），
`全零 = 估计器没输出`，不是绑定错、更不是 pass 没跑。这也说明"要不要更准的场"的真正动机
不是精度而是**覆盖率**：让低纹理区的向量从"没有"变成"有"。


### 3.1 环境变量到不了 runtime

runtime 和播放器都是 **/MT 静态 CRT**，环境变量在进程之间传不过去。
**只能写文件**：`<amdRuntimeDir>/DLSS5-AMD/custom-config.txt`。

### 3.2 runtime 一个进程只读一次 config

所以必须在**渲染开始前**写；改完必须重启（每次渲染是新进程，播放中改旧进程无感）。
诊断时如果"改了没反应"，先确认渲染进程是不是那次改写之后才启动的。

### 3.3 写入时机：必须在 `amdRuntimeDirectory` 赋值之后

`StageExport.cpp` 里曾经把写 config 放在 `request.amdRuntimeDirectory = ...` 之前，
结果在播放器旁边建了一个空的 `DLSS5-AMD/` 目录（空壳），真正的 runtime 一个字节都没改。

**规则**：任何"写进 runtime 目录"的动作，位置都要在目录赋值之后，并且检查目录里确实有 `custom-config.txt`。

### 3.4 两个渲染入口都要写

- 播放 / 离线 job：`NeuralJob` 构造 request 之后
- 命令行导出：`StageExport.cpp`

只改一处，另一条路就会用旧值 → 出现"播放正常、导出不一致"这种极难查的分裂。

### 3.5 写 config 只能改目标行

`WriteAmdTemporalMode` 逐行替换 `DLSS5_TEMPORAL_MODE=`，**保留文件里其余所有内容**（用户手改过的其他键、注释、顺序）。
不要整文件重写——那是用户的文件。

### 3.6 新键必须进缓存键

`AmdNeuralSettingsDocument` / `CanonicalNeuralSettingsForBackend` 里加一项（如 `temporal-mode=`），
否则换了设置会命中旧渲染，表现为"设置没生效"。这也是判断"设置到底有没有被算进身份"的第一现场。

### 3.7 `DLSS5_TEMPORAL_MODE` 只在 Original + 单 pass 有效

- 上游 `scripts/hip-* flags.txt` 与 custom-config 注释写明：**每个 active mode 都要求 `DLSS5_MULTI_PASS=1`**。
- 所以 `EffectiveAmdTemporalMode(mode, small, passes)` 在 `small` 或 `passes != 1` 时把 mode 归 0；
  面板置灰用同一个谓词 `AmdTemporalModeCanRun()`，**两处必须同源**，否则 UI 允许选、实际被静默改掉。
- 状态词对照：`history=low-freq-active` = MODE2 正在跑；`history=off` = 没跑；
  `model_reason=low-frequency-requires-original` / `low-freq-requires-mp1` = 冲突被回退。

### 3.8 "low-frequency" 是模式名，不是素材属性

曾经把 `model_reason=low-frequency-requires-original` 误读成"素材时间频率太低"，于是去测帧率、测逐帧噪声，全都测不出来。
真相是它指 **TEMPORAL_MODE=2（独立低频残差 EMA）**。
**教训**：status 里的词先在上游源码里搜一遍定义，别按字面理解。

---

## 4. Small 模型

- `DLSS5_MULTI_PASS` 一个键两种域：`0.5` = Small（31 blocks，单 pass）；`1/2/3` = Original（71-block 堆叠）；**`4` 非法**，静默按 `0.5` 读。
- **`small` 是 Windows SDK 的宏**（`rpcndr.h`：`#define small char`）→ 代码里一律 `smallModel`，否则编译报出莫名其妙的错。
- Small 与 MODE 的冲突只在 `EffectiveAmdTemporalMode` 一处纠正；不要在别处再写一份"如果 small 就把 mode 清零"。
- 换模型后想确认真的换了网络：`model=small model_reason=small-ready`，并且**像素上确实不同**（Small vs Original 的 MAD ≈ 10，噪声底 ≈ 0.6）。

---

## 5. 二进制与构建

### 5.1 evaluator 在 `NeuralWorker.exe` 里，不在播放器里

改 `AmdLmxxfEvaluator` / `OfflineNeuralRenderer` 之后只拷播放器 exe = 改动完全不生效，而且看起来像"改了没用"。
**判断顺序**：先看 `run-fg/neural-runtime/NeuralWorker.exe` 的时间戳，再看日志里有没有新加的那行 `Log(...)`。

**A/B 时尤其致命**（2026-10-23 又踩一次）：两臂只换 `DLSSVideoPlayer.exe`、worker 保持同一个，
两次渲染必然**逐帧相同**，于是"修复无效"这个错误结论看起来证据确凿——实际是同一个 worker 跑了两遍。
所以：

- 两套二进制（`NeuralWorker.exe` + `DLSSVideoPlayer.exe`）**成套换**，换完 `md5sum` 两臂各存一份。
- **看到 A/B 输出逐帧相同，第一反应是"worker 没换"，不是"改动无效"。** 先比
  `git diff <旧提交> HEAD --stat -- src tests` 看差异落在哪个文件，再确认那个文件属于哪个目标
  （`AmdLmxxfEvaluator.cpp` → `player_neural` 对象库 → worker）。
- 加诊断日志时也要认准这个边界：写在 evaluator 里的日志只会在 worker 进程里打印，
  而 `DLSSVideoPlayer.exe` 是 WIN32 子系统，`fprintf(stderr)` 在管道里可能一个字都看不到——
  落盘到文件（`F:/tmp/xxx.txt` 追加）最稳。

### 5.2 重建会自动同步，但手拷后要验

`CMakeLists.txt` 的 POST_BUILD 已经把 `NeuralWorker` `copy_if_different` 到 `<build>/neural-runtime/`。
但 `run-fg` 是另一个暂存位，手拷时要两个都拷，并 `ls -la` 确认时间戳。
**实证法**：用 Python 在二进制里找新增的日志字符串（`.decode('latin-1')` 或 UTF-16LE needle），别靠猜。

### 5.3 换 runtime 包

- 换 DLL 不用改代码（ABI 仍 `GetApi(6,176)`）。
- 换载荷只拷增量，**但清单 SUMS 必须跟着换**，否则 `VerifyAmdRuntimeLock` 直接拒渲染。
- 切臂先 `rm -rf` 再拷（`cp -r` 会嵌套）。
- 换完清 `[NeuralPace]`，否则旧样本误导渲染时长预报。
- **过期二进制不能判新 DLL 好坏**：`AmdLmxxfRuntimeProbe.exe`（只扫 abi 1..4）、`AmdLmxxfEvaluatorSmoke`（偶发 init 失败）都不作数，只认播放器实测。

---

## 6. 渲染缓存

- `AmdRuntimeIdentityFiles` **必须排除 `.log` 和 shader-cache**，否则摘要每块都变，缓存永不命中。
- `Sha256FileCached` 的 memo 容量是 **4096、FIFO**：AMD 身份集有 700+ 文件，容量小会一趟撑爆然后每个 job 重算 ~400MB（1.2–2.9 s）。
- manifest 的 `payloadStamp` 含 **change time**，必须留在比较里（只比 mtime 会漏"同秒改写"）。
- 命中键只认整片 `range=[0,0)`；live 只做 header 探测。

---

## 7. UI 层

### 7.1 `TTM_GETTEXTW` 的 512 陷阱（动 tooltip 之前先读）

comctl32 把整条提示文本拷进 `lpszText`，**不遵守任何大小参数**。
提示 ≥ 512 字符 = 栈溢出 = `__fastfail` = **exit 127，静默无输出**（注意是 127，不是 139，很容易误判成"没崩溃只是没跑"）。

三层防线：
1. `Localization.h` 表头注释；
2. `PlayerUiRegressionTests::ReadTip()` 在读之前 `CHECK(T(key).size() < 512)`；
3. 控件键配对表随后端选键。

约束归"会被回读的提示"，不是全局限制——从不回读的长提示（1200+ 字符）是合规的，别去"修"它们。

### 7.2 UI 不能读逐帧清零的标志

`m_fgDoublingActive` 每帧开头清零、末尾才置回 → 从它取值会让 Interpolate 按钮闪烁。
WM_PAINT 路径只能读粘性量（`PlaybackFrameGenInterpolating()` / `m_fgDoublingEstablished`）。
**通则：任何"每帧开头清零"的标志都不能进 UI 取值路径。**

---

## 8. 诊断方法论（拖影那次沉淀下来的流程）

1. **先定性：哪一级引入了"历史"？** 拖影必须有跨帧记忆。`TEMPORAL_MODE=0` 时神经网络本身无时域记忆 → 它不可能自己产生拖影，根因必在下游两条时域链（宿主累积 / FSR4 重投影 / FFX 插帧）。
2. **逐级关，不要猜。** 先关最下游（Interpolate），没变化就说明根因在上游——这一步直接把 FFX 从嫌疑名单里划掉。
3. **日志取证。** worker 的 `AMD neural runtime: model=... history=... net_gpu_ms=...` 一行能同时证伪多个假设；`temporal=on accumulation=high` 直接把"宿主累积在跑"钉死。
4. **代码坐实。** 找到绑定处，看输入到底是什么纹理/常量，而不是看注释和命名。
5. **量化。** 合成可控素材 + 拟合（见第 9 节）。肉眼说"好像好点了"不算结论。
6. **别把放大器当根因。** 插帧和 FSR4 都会让拖影更花，但都可能是无辜的；先找到"谁引入了历史"。

---

## 9. 测量工具的坑

- numpy 环境：`C:/Users/Administrator/.workbuddy/binaries/python/envs/default`。
- ffmpeg `-t` **必须在 `-i` 之前**；`-v error` 会吞掉 psnr 摘要。
- 未打标签的 720p 会被当 BT.601，比较前必须钳矩阵：`-vf "scale=in_range=tv:in_color_matrix=bt709,format=bgra"`。
- **别用 `ffmpeg -lavfi psnr` 量"对源 PSNR"**（出现过 7.84 dB 这种明显错误的值）。抽帧直接比 MAD。
- 二进制里扫字符串一律用 Python：bash 的 `$'\x00'` 不能含 NUL，`.decode('latin-1')` 之后 `find` 最稳。
- **合成鬼影素材的配方**：1280×720 低对比平滑纹理，每帧平移 10 px（用超宽底图裁窗），`libx264 -crf 10 -pix_fmt yuv444p`。平移量已知，才好判"残影落后几像素"。
- **量化鬼影的方法**：输出帧 9×9 高通后，对"当前源帧 + 前 1/2/3 帧的位移副本"做最小二乘拟合。
  系数就是各副本在画面里的占比：`t-1/t-2/t-3` 全 0 且 `own ≈ 1` = 无残影；旧实现是 0.199/0.136/0.089。
- 命令行渲染：`--render <in> --out <out> --stages nr --neural-backend amd --temporal-stability off|high --passes 1 --encode high`。

---

## 10. 提交前检查清单

- [ ] `git diff` 里 NVIDIA 路径（ngx / capture / feature-18 / FSR1 / CAS）**逐字未动**，或已说明理由
- [ ] 新的设置键进了缓存身份（`CanonicalNeuralSettingsForBackend`）
- [ ] 新增/修改的 tooltip 会被回读时 < 512 字符
- [ ] UI 取值路径只读粘性量，不读逐帧清零标志
- [ ] 改了 evaluator → `NeuralWorker.exe` 已重建并 stage 到 `run-fg`，且用二进制字符串确认过
- [ ] 回归：`AmdLmxxfApiTests`、`PolicyTests`（406）、`NeuralPrerenderTests`、`NeuralWorkerTests`
- [ ] 清 `[NeuralPace]`（如果动了渲染路径）
- [ ] 视觉类改动附上量化读数，不只写"看起来好了"
