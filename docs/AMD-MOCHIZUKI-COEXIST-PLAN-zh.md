# AMD 第二后端：把 Mochizuki 接进播放器并与 lmxxf 并存

_基线：本仓库 `1894a92` / 0.28.x；对照包 `G:\补帧\OptiScaler-AMD-PreSR\OptScaler-NR-1.12.1`；实测日期 2026-10-30。_

## 1. 结论先说

**这不是"再接一个后端"，而是"给现有 AMD 后端加一根分叉"。**

MochizukiNrRuntime.dll 不提供自己的 API 表，它把 **lmxxf 那张表**原样交回来：

```
MochizukiNrGetApi(6, table{struct_size=176}) -> OK, abi_version 6, 21/21 非空
```

凡是 `src/AmdLmxxfApi.h` 已经做过的事——`kAcceptedAbiVersions`、`ValidateApiTable`、
`HasRequiredFunctions`、租约生命周期的顺序、`LmxxfNrJob`（32 字节）——**一个字都不用改**。
差别只有两处：

| | lmxxf | mochizuki |
|---|---|---|
| 表内 `PrepareFrame` | 真实现 | **拒绝桩**：`INVALID_ARGUMENT`，`"Use MochizukiNrPrepareFrame from the current package"` |
| 帧提交 | `api.PrepareFrame(ctx, &LmxxfNrFrameInfo, &job)` | 独立导出 `MochizukiNrPrepareFrame(ctx, &MochizukiNrFrameInfo, &job)` |

其余 20 个表项（`Create/PrepareSession/RecordInputs/EnqueueHip/RecordOutputs/
ExecuteAfterProducer/Poll/Retire/ResetHistory/Drain/GetStatus/GetLastError/
BeginRecordingExecution/EndRecordingExecution/InvalidateRecording/CollectRecording/GetTimings`）
行为与 lmxxf 一致。

所以正确做法**不是**照 `AmdLmxxfEvaluator.cpp`（约 3500 行）再抄一份，而是把
"帧结构体长什么样、用哪个导出提交"抽成一个分叉点，共享 D3D12 资源、上传、引导纹理、
租约机、读回、编码器交接。

## 2. 实测记录（对真实 DLL，2026-10-30）

写代码之前先跑通的四件事，都是可复现的：

| 探测 | 结果 | 含义 |
|---|---|---|
| `MochizukiNrGetApi(v, 176)`，v=1..5,7,8 | `UNSUPPORTED_ABI` | 只认 6，没有降级空间 |
| `MochizukiNrGetApi(6, 176)` | `OK`，`abi_version=6`，21/21 非空 | 与本仓库现有协商完全同一份契约 |
| `MochizukiNrGetApi(6, 136/144/168/184)` | `INVALID_ARGUMENT` | 表长度是精确契约，不做截断 |
| `table.QueryCapabilities(32)` | `OK`，box `16384x16384`，`history=1`，`gfx1201=1` | 时序历史**可用**，RDNA4 路径正确 |
| `table.PrepareFrame(...)` | `INVALID_ARGUMENT` + `"Use MochizukiNrPrepareFrame..."` | 表内入口是桩，必须走独立导出 |
| `MochizukiNrPrepareFrame(null, info, job32)` | `UNAVAILABLE` + `"PrepareFrame: execution unresolved"` | 失败点是"没有会话"，不是 ABI |
| 真机 Create（flags=0） | `INVALID_ARGUMENT` + `"Create: current package recording ABI required"` | **只接受租约会话**：flags 必须是 `RECORDING_LEASES` 或 `RECORDING_LEASES\|INLINE_QUEUE` |
| 真机 PrepareFrame 扫描（job × frame 尺寸） | job 32 + frame **88** → `UNAVAILABLE "building the network"`；frame 112/92/96… 全部 `INVALID_ARGUMENT "PrepareFrame: bad arguments"` | **1.12.1 的帧结构是 88 字节，没有 depth 块**；112 是 0.0.5/主干加了 depth 之后的形态 |

`sizeof(LmxxfNrJob) = 32`（两个后端同尺寸）。帧结构有 **两个版本**，只能靠调用被接受与否来分辨
（`PrepareFrame` 对"尺寸不对"和"参数为空"给的是同一句话）：

```
88  = struct_size…reset（1.12.1 包；无 depth/depth_state/depth_width/depth_height/depth_inverted）
112 = 88 + depth 块（0.0.5 / 上游主干 / 本仓库 vendored 头文件声明的那一版）
```

所以 `Session::PrepareFrame` 先按 112 试，被 `INVALID_ARGUMENT` 拒后用 88 重试，并记住这次会话
的答案（`MochizukiFrameInfoSize()`）。**1.12.1 上 depth 引导等于不存在**，只有运动矢量可用。
注意 fork 现在的 `LmxxfNrJob` 第四个字段叫 `first_pass_output`（偏移 24），本仓库头里叫
`abi6_reserved`（同偏移同长度，都是 8 字节）——**字节兼容，不读它就行**，不必改头文件。

## 3. 并存怎么放：一个目录，两套资产，按 kind 选

文件集合天然不冲突，所以两套可以放同一个 runtime 目录：

```
amd-neural-runtime/
├─ LmxxfNrRuntime.dll            ← lmxxf 走这里
├─ lmxxf-modules/gfx1201/…       ← lmxxf 算子
├─ shaders/                      ← lmxxf 编解码着色器
├─ native-game-tiled-assets/     ← lmxxf 权重
├─ MochizukiNrRuntime.dll        ← mochizuki 走这里
└─ dlssnr-amd/
   ├─ shaders/     (61 个)
   ├─ aco/         (58 records + 58 shaders + shell-aliases.txt)
   ├─ dlssnr.bin   (147,756,560 B, sha256 2b41c888…f7fbd)
   └─ int4/        (只在开 INT4 时需要)
```

选型落点三处，优先级从高到低：

1. 命令行 `--amd-nr-runtime=mochizuki|lmxxf|auto`（`auto` = 目录里有哪个用哪个，两个都在选 lmxxf，保持现有行为不变）
2. `DLSS5.ini` `[AmdNeural] Runtime=`
3. 菜单：AMD 神经子菜单里一对单选（2 项，符合"每组 2~7 项"的规则）

## 4. 要改的文件（按依赖顺序）

### 4.1 已落地（本次提交，纯新增，不影响任何现有路径）

- `third_party/mochizuki/MochizukiNrApi.h` —— vendored 帧契约：`MochizukiNrFrameInfo`(112)、
  `MOCHIZUKI_NR_FRAME_FLAG_*`(1<<8..1<<16)、两个导出的 PFN 类型、两条 `static_assert`。
- `src/AmdMochizukiApi.h` —— 宿主侧构造：`MakeFrameInfo` / `ApplyTemporalGuides` /
  **`ConvertFrame(const LmxxfNrFrameInfo&, passes)`**。最后这个是把改动压到最小的关键：
  求值器继续只填一份 `LmxxfNrFrameInfo`，分叉就是"转换一下 + 换个导出"。
  （`ConvertFrame` 产出的 `struct_size` 只是头文件的 112；真正下发的尺寸由 Session 按会话协商结果覆盖。）

### 4.2 `src/AmdLmxxfRuntime.h/.cpp`：加载器加一个 kind

```cpp
enum class NrKind { Lmxxf, Mochizuki };
const wchar_t* NrKindModuleName(NrKind kind);   // 两个 dll 名
NrKind NrKindForDirectory(const fs::path&);     // 两个都在 -> Lmxxf（不改现有行为）

bool Load(const fs::path& runtimeDirectory, NrKind kind, std::wstring* error);
NrKind Kind() const noexcept;
PFN_MochizukiNrPrepareFrame MochizukiPrepare() const noexcept;
```

`Load` 内部只有三行不同：模块名、`GetProcAddress` 的名字（`LmxxfNrGetApi` /
`MochizukiNrGetApi`）、以及 Mochizuki 额外解析 `MochizukiNrPrepareFrame`。
**协商循环一个字不动**——实测两边都只认 6。

### 4.3 `src/AmdLmxxfSession.h`：加一个重载

```cpp
// 构造时给 prepare 钩子；为空 = lmxxf 路径，行为与今天逐字节相同
Session(const LmxxfNrApi& api, void* context, bool leases = false,
        PFN_MochizukiNrPrepareFrame mochizukiPrepare = nullptr) noexcept;

bool PrepareFrame(const MochizukiNrFrameInfo& info)
{
    if (!mochizukiPrepare_) return Refuse("PrepareFrame: no Mochizuki prepare hook");
    if (!CanPrepare()) return Refuse("PrepareFrame is out of order");
    job_ = MakeJob();                       // 32 字节，两个后端共用
    // 这里不调 StampJobForAbi：mochizuki 只有 ABI 6，没有 24 字节的历史版本
    const int32_t status = mochizukiPrepare_(context_, &info, &job_);
    if (status != LMXXF_NR_OK) return Fail(status, "PrepareFrame");
    if (job_.handle == nullptr) return Refuse("PrepareFrame returned a null job");
    phase_ = CallPhase::Prepared;
    return Succeed();
}
```

### 4.4 `src/AmdLmxxfEvaluator.h/.cpp`：两处提交点 + 一个配置项

配置里加 `amd_lmxxf::NrKind kind{Lmxxf}` 和 `uint32_t modelPasses{1}`（mochizuki 的 pass 数
是**每帧**的，lmxxf 是网络建好时定死的，所以这个字段只在 mochizuki 路径被读）。

两个 `session.PrepareFrame(frameInfo)` 调用点（当前在 3247 / 3426 行附近）改成：

```cpp
if (config.kind == amd_lmxxf::NrKind::Mochizuki)
    ok = session.PrepareFrame(amd_mochizuki::ConvertFrame(frameInfo, config.modelPasses));
else
    ok = session.PrepareFrame(frameInfo);
```

引导期要补的**唯一**语义差异：style/intensity/tone 在 lmxxf 上是"写文件、重载 DLL、网络按
它编译"；在 mochizuki 上是会话态 `MochizukiNrSetControls(MochizukiNrControls)`，网络重建由
它自己在后台做。所以：

- `Runtime` 增加 `SetControls(const MochizukiNrControls&, std::wstring*)`（依赖 4.5 的控件头）；
- `AmdLmxxfEvaluator::BringUpRuntime()` 里，mochizuki 分支在 `PrepareSession` 之前下发一次
  controls，然后**不**等待网络就绪——首帧建网络要几分钟，期间帧直通。

### 4.5 控件与进度（可以晚一个提交）

Mochizuki 的画面参数全在 `MochizukiNrControls`（约 60 个字段：intensity/style/local_tone/
local_structure/skin_structure/automatic_mask/max_ratio/history_strength/white_point/apply_model/
linear_input/max_passes/drs_mode/preprocess 六个/enlarge_mode/classic_scaler/aco_mode/int4_mixed/
renodx 一整套）。头文件不在发布包里，要从 fork 取
`OptiScaler/dlssnr/backend/mochizuki_runtime/MochizukiNrControls.h`（同 MIT，vendored 到
`third_party/mochizuki/`）。

第一版**建议只接 6 个**：`intensity`、`style`、`local_tone`、`local_structure`、
`skin_structure`、`max_passes` —— 与现有菜单的"风格/强度/多 pass"一一对应，其余保持默认。
`MochizukiNrGetControlDefaults` 需要一个活会话（无会话调用返回非 OK），所以默认值只能拿
结构体零值 + 头文件注释里的 default 作为初值。

`MochizukiNrGetBuildProgress(ctx, &progress)` 给出 `active/completed/total/start_tick/stage[96]`，
正好接现有状态栏——首帧那几分钟**必须**有进度，否则用户会当成卡死（这是与 lmxxf 最大的
体验差异：lmxxf 的建网没有可读进度）。

### 4.6 装配与打包

- `NeuralWorker.cpp` / `OfflineNeuralRenderer.cpp`：`request.amdRuntimeKind` 透传到
  `AmdLmxxfEvaluatorConfig`；日志行 `Neural backend: amd` 后面补 `nr=mochizuki`。
- `src/NeuralPreflight.cpp`：预检按 kind 找对应 DLL + 模型；mochizuki 缺 `dlssnr-amd/dlssnr.bin`
  要在预检就报，而不是等首帧。
- `tools/package_amd_release.ps1` + `package_amd_release.bat`：把
  `MochizukiNrRuntime.dll`、`dlssnr-amd/**` 纳入清单（模型 147MB，建议单独一个
  `-WithMochizuki` 开关，别把默认包撑大一倍）。
- `tests/AmdMochizukiApiTests.cpp`：纯逻辑，不需要 GPU——`ConvertFrame` 的字段映射、
  flags 传递、`reset`/`depth_inverted` 提升、`struct_size` 恒定 112。挂进现有 ctest。

## 5. 风险与坑（都是实测/读源码得来）

1. **首帧建网络几分钟，且改任何"容量"都会重建**：分辨率、`model_scale`、`passes`、
   `max_passes`、`linear_input`、`preprocess`、`compact_transfer`、`enlarge_mode`、
   `int4_mixed` 都算。`max_passes=0`（自动）在降 pass 时保留大分配，只是省 GPU 时间不省显存。
2. **ACO 在 Vulkan 设备创建时定死**：`aco_mode` 改了要重启进程才生效；`dlssnr-amd/aco/`
   不全时自动退回驱动编译器（首启更慢，功能不受影响）。
3. **`_FORCE_INLINE`/`INLINE_QUEUE`/`DIRECT_RECORDING` 一条都别开**：那是 adopted vkd3d
   的路径，Windows D3D12 宿主开了会被 `PrepareFrame` 直接拒（`INVALID_ARGUMENT`）。
4. **不要给覆盖层传 `depth_inverted=1`**：那是游戏反深度的语义；本机合成的深度是正向。
5. **日志分开看**：mochizuki 的初始化/建网/失败都进统一日志（OptiScaler 那边是
   `OptiScaler.log`），播放器这边沿用现有 `neural-worker.log`，但**不要**去找
   `dlssnr_on_amd.log`（那是 daniel 后端的）。
6. **两套资产同目录时不要共享 `shaders/`**：lmxxf 用根 `shaders/`，mochizuki 用
   `dlssnr-amd/shaders/`，串版本会静默编出错的网络。
7. **1.12.1 的 `LOCAL-TEST.txt` 自述未跑 release 测试**；同目录还有 `1.13.0-alpha8`，
   真要上生产先在两版之间做一次同片 A/B。

## 6. 验收（照现有口径，不新造指标）

- `AmdMochizukiApiTests`：字段映射与尺寸，无 GPU。
- 真机：同一段 1080p30 10s 素材，`--amd-nr-runtime=lmxxf` 与 `=mochizuki` 各跑一次，比
  `NeuralEvaluations` / 墙钟 ms/帧 / `history=` 状态行 / 输出帧 framemd5 差异率。
- 并存验收：同一目录两套资产，切 kind 不需要删文件；切回 lmxxf 后行为与今天逐字节一致
  （日志 `abi_version 6, table 176 bytes` 不变）。
- 首帧体验：建网期间必须能看到进度行且在推进；期间帧直通、不报失败。

## 7. 已经跑通的验证（2026-10-30，本机 9070 XT）

探针 `tests/AmdMochizukiRuntimeProbe.cpp`（`build-mz/AmdMochizukiRuntimeProbe.exe <包根>`）：

```
runtime   G://补帧//...//OptScaler-NR-1.12.1//MochizukiNrRuntime.dll
          sha256 176bc30841b0a3f94404a8977f4cf614d70c121dfaf987e16dcc0062f42ae751
ABI       GetApi(6,176) OK / 21 项全非空；1..5、7 全 UNSUPPORTED_ABI
caps      16384x16384 history=1 gfx1201=1
Create    flags=RECORDING_LEASES -> OK（flags=0 -> "current package recording ABI required"）
Prepare   112 被拒 -> 88 接受 -> UNAVAILABLE "building the network"
建网      0s→70s，stage "Creating pipelines" 3/25→20/25，最后 "Finishing"
链         PrepareFrame / RecordInputs / RecordOutputs / Begin / EnqueueHip /
          End / Invalidate / Collect —— 全部 OK(0)
输出      private_output 独立纹理 1920x1080 format 87
          均值亮度 128.26（输入 127.00），mean|产−入| RGB = 3.69
          → 既非黑帧也非原帧；PNG: amd-mochizuki-probe-output.png（平滑渐变，无块效应/接缝）
status    "mochizuki 1920x1080, network 0.00 ms (p95 0.00), 1 frames"（首帧，计时未采集）
```

排错注意：`last_error` 会在协商成功后仍显示 `GetApi: unsupported abi_version`——那是探针自己
用 1..5/7 试出来的**全局**残留错误，不是这次会话的失败，别照它排查。

尚未验证：多帧时序（history）、带运动矢量的一帧、`SetControls` 改风格后的重建、
以及 112 字节的 0.0.5 包（本机只有 1.12.1）。

## 8. 播放器求值器真机验收（2026-10-30，`DLSS5_AMD_NR_RUNTIME=mochizuki`）

`tests/AmdLmxxfEvaluatorSmoke.cpp` 就是产品渲染路径（`OfflineNeuralRenderer` →
`AmdLmxxfEvaluator` → 运行时），改的只是 evaluator 的 `kind`：

```
run 1  300 帧 1080p30          result ok / nativeEvaluations=300 / historyResets=1
       墙钟 8710 ms (29.0 ms/帧)，其中建网 1303 ms
       submit 中位 12.1 ms、最慢 20.3 ms；运行时自报 network 8.32 ms
run 2  seek+preroll 18 帧      ok，18/18 求值
run 3  编码器失败重试 12 帧     ok，NVENC→软件回退策略照旧
run 4  借用 device/queue 3 帧   ok，显存可读（2262 MiB / 15419）
run 5  合成静止序列 24 帧 ×2    ok（见下方"历史"结论）
run 6  文件引导 pan 6 臂 ×20    ok
run 7  真实解码视频 静止/行走    ok
RESULT: ok（95 条断言全绿，exit 0）
```

冷启 vs 热启：首轮建网 66 s（"Creating pipelines" 2/25→23/25），之后每次 **1.3 s**。

**重要未证明项（不要当成已支持）**：给运动矢量/深度之后，产出的帧与不给时
**逐位相同（divergence 0.000）**，而且这个包的 `GetStatus` 根本没有 `history=` 字段
（只有 `mochizuki WxH, network X ms, N frames`）。也就是说 **1.12.1 上的时序历史未观测到任何效果**；
最可能的原因是它的 88 字节帧结构装不下 depth 块（`MochizukiNrFrameInfo` 的 depth 四件套在 112 字节
版本里），而历史融合需要 depth。smoke 里相关断言已按后端分开：mochizuki 只打印 `note`，不再假装
"engaged"。

### 产品选择链路（A，已接完）

- **类型**：`NeuralRenderTypes.h` 加 `AmdNrRuntime{Lmxxf,Mochizuki}` + `AmdNrRuntimeName/ParseAmdNrRuntime`；
  `NeuralRenderRequest::amdNrRuntime`（**默认 mochizuki** —— 它一次建网、之后从这张网出图；
  lmxxf 仍在菜单里可选）。
- **协议**：worker 参数 `--amd-nr-runtime`（只在非默认时发，即只有 lmxxf 才发），
  旧 helper 收到的命令行逐字节不变。
- **端到端**：`RenderSettings::amdNrRuntime`（聚合初值同步）→ `NeuralJobInputs` → `StageExport::Job`
  → `NeuralRenderRequest` → `AmdLmxxfEvaluatorConfig.kind`。
- **持久化**：`[AmdNeural] Runtime=lmxxf|mochizuki`，读在 `ReadSavedAmdNrRuntime()`，
  与 Style 同一节一起保存。**ini 里没有这一项时走默认（mochizuki）**；
  已有的 `Runtime=lmxxf` 会继续生效，改默认不会覆盖别人机器上存下来的选择。
- **缓存键**：`AmdNeuralSettingsDocument()` 首行是 `backend=amd-<runtime>`，**函数签名里的默认
  runtime 跟产品默认一致（mochizuki）**，所以省略参数写出来的键不会描述另一个后端的画；
  显式 lmxxf 的键仍逐字节是 `backend=amd-lmxxf`（`AmdLmxxfApiTests` 用显式 wrapper 钉住这一点）。
  两个后端不会互相命中缓存。
- **菜单风格/Passes 真的生效**：`MochizukiNrSetControls` 已接。控件结构的 **struct_size 由运行时决定**
  （`GetControlDefaults` 只接受包自己的尺寸，1.12.1 与我们 vendored 的 2026-10-28 头不同），
  所以 `Runtime::MochizukiControlsSize()` 探一次、把头部字段拷进该尺寸的零填充 payload。
- **预检**：mochizuki 树不走 lmxxf 的摘要锁（它没有本仓库钉住的 SHA256SUMS），改查
  `MochizukiNrRuntime.dll` / `dlssnr-amd/dlssnr.bin` / `dlssnr-amd/shaders` 三个必要物。
- **打包**：`tools/package_amd_release.ps1` 的布局校验允许 mochizuki-only 树，清单里两个 DLL 都记 SHA256。

回归：全量 `cmake --build build-ninja` 绿（`/W4 /WX`），`PolicyTests` 406 例全过，
smoke 七个 run 全绿（300 帧 30.2 ms/帧、submit 中位 12.5 ms、带 controls 日志）。

⚠️ 同一份 smoke 在**本机 lmxxf 包**上 run 5–7 报 `history=off`（run 1–4 全绿）。逐点审过本次改动对
lmxxf 路径的六个接触面（Load、assets 目录、lease 标志、Session 的 prepare 钩子、PrepareFrame 分叉、
WaitForMochizukiNetwork 早退）都是行为中性的，判断是本地包/时序模式既有问题，不是这次引入的；
需要在 lmxxf 侧单独确认 `DLSS5_TEMPORAL_MODE`。

## 9. UI 开关（A + C，已落地）

**位置**：`DLSS → Neural presets` 子菜单，紧跟 AMD style 单选块之后：

```
── AMD style: what the model is asked for ──
   Style 0 / Style 1 / Style 2
──────────────────────────────
   AMD runtime: which network draws the frame      (灰标题)
 ✓ lmxxf · HIP modules (default)
   mochizuki · Vulkan, builds its network once
```

- ID 段：`IDM_NEURAL_AMD_RUNTIME_FIRST = 498`，`_COUNT = 2`（避开 495-497 style、480-486 scale/history）。
- 本地化：`Localization.h` 三个键（`menu.amd_runtime_header/_lmxxf/_mochizuki`）。
- 同步：`SyncFeatureMenuState` 里 `CheckMenuRadioItem` + NVIDIA 上两项一起 `MF_GRAYED`。
- 动作：`SetAmdNrRuntime()` —— 与 `SetAmdStyle()` 同一条重启路径（`DetachLivePlayback` →
  `StopLiveNeuralSession` → `PerformSeek` → `StartLiveNeuralSession`，暂停态走
  `SchedulePausedSettingsPreview`），随后 `SaveVideoSettings()` 写 `[AmdNeural] Runtime=`。
- 启动页状态行会明说当前是哪个：`lmxxf · N files` / `mochizuki · model and shaders present`。

验收（本机，2026-10-30）：

| 指标 | 结果 |
|---|---|
| 全量构建（含播放器/worker/测试，`/W4 /WX`） | 绿 |
| `PolicyTests` | 406 例全过（菜单分组规则含在内） |
| `PlayerUiRegressionTests` | 64 例 / 3062 断言 / **22 failures** |
| 同上，但把我的改动 `git stash` 掉重编 | **同样 22 failures**（`export_stages_dialog_test`、
  `neural_settings_dialog_test`、`encoder_settings_dialog_test`）→ **本次改动净增 0** |
| `[AmdNeural] Runtime=lmxxf` 落盘 | 已由测试进程写入，说明保存路径通 |

## 10. 选中 mochizuki 时弹"NVIDIA 驱动太旧"——根因与修复（2026-10-30）

现场：选中 mochizuki 后弹 `Neural rendering needs a newer NVIDIA driver / 570.29 < 610.47`。
日志（`build-ninja/DLSSVideoPlayer.log`）里的真相是两件事叠加：

```
[07:33:10] AMD neural runtime identified: digest 73 ms over 707 files   ← 走的是 lmxxf 的摘要锁
[07:33:29] Active neural session ended early: kind=preflight
           detail=The AMD neural runtime directory could not be resolved; ...
```

1. **包不在运行时目录里**：播放器只解析一棵树（`amd-neural-runtime/` 或 `DLSS5_AMD_RUNTIME_DIR`），
   而里面只有 lmxxf；worker 按 kind=mochizuki 找 `MochizukiNrRuntime.dll` 失败 → Preflight。
2. **失败被误报成 NVIDIA 驱动问题**：`NoteNeuralFailure` 只判断
   `failure==Preflight && ClassifyNeuralDriver(detectedGpu)!=Ok`，**没看后端**；这台机器上
   `detectedGpu` 是 NVIDIA（570.29），于是 AMD 的"包缺失"被翻译成"换个 NVIDIA 驱动"。

修复：
- `NoteNeuralFailure`：driver-floor 分支加 `neuralBackend == Nvidia` 前提，AMD 的 Preflight
  现在显示它自己的 detail。
- `PrepareAmdRuntime`：按 kind 分叉。mochizuki 不再跑 lmxxf 的 707 文件摘要锁与 manifest 校验，
  改为对 `MochizukiNrRuntime.dll` + `dlssnr-amd/dlssnr.bin` + `dlssnr-amd/shaders/**` 做身份摘要，
  缺件时明确说缺什么。
- `StageExport` / `OfflineNeuralRenderer`：解析失败文案点名**缺哪个模块、在哪个目录找的**
  （旧句 "put amd-neural-runtime beside the module" 对已经这么做过的人只会误导）。
- `StageExport` 的解析补上 `job.amdNrRuntime`（原来写死 lmxxf）。
- smoke 的 borrowed-device 臂新建 config 时漏了 `kind`，一直悄悄在测 lmxxf → 已补。

部署：把 `MochizukiNrRuntime.dll` + `dlssnr-amd/` 放进**同一个** `amd-neural-runtime/`（与 lmxxf
并存，文件名不冲突）。已在本机 `F:/dlss5-video/dlss5-video-amd/amd-neural-runtime/` 完成，
smoke 以该目录跑 300 帧全绿（31.2 ms/帧，热建网 1.5 s）。

## 11. 建网时间：ACO 打开、顺序修正、等待状态（2026-10-30）

原来的首帧等待是 **66–73 s**（`Creating pipelines 3/24 → 24/24`，驱动在编译网络）。三个发现：

1. **`aco_mode` 一直是 0**。`MochizukiNrControls` 的零值不等于运行时默认值：文档默认 ACO 开（1），
   而零初始化＝关。包里的 `dlssnr-amd/aco/`（58 records + 58 shaders + shell-aliases）因此从未被用过。
2. **下发顺序错了**。ACO 在 **Vulkan 设备创建时**定死，而设备在 `PrepareSession` 里创建；
   我们原来在 `PrepareSession` 之后才 `SetControls`，所以即使写 1 也只会得到
   “ACO mode applies on the next game start”。现在 mochizuki 的 `SetControls` 移到 `PrepareSession` 之前
   （lmxxf 顺序不变）。
3. **运行时的日志默认是关的**（`MochizukiNrSetLogging`），所有 ACO / pipeline / 建网决策都在
   `<包根>/mochizuki_nr.log`，不打开就只能看到状态码。已在 bring-up 里打开。

实测（9070 XT，`mochizuki_nr.log` 的 phase 归因）：

```
ACO=0，驱动 LLPC 缓存冷（首次）     graph build 66–73 s   pipelines 66 s（24 条驱动编译）
ACO=1，删掉 AMD VkCache 后           graph build 6.41 s    pipelines 5.11 s（29 条 PAL 导入）
ACO=0，同状态                        graph build 6.81 s    pipelines 5.33 s（24 条）
ACO=1，稳态（已有缓存）              graph build 1.49 s    pipelines 0.23 s
```

- 删除的只是 `%LOCALAPPDATA%\AMD\VkCache`（250 MB，可重建）。**`AMD\DxcCache`（6.4 GB）动不了**
  （`Permission denied`，被占用），所以“完全首次”的对照做不了 —— 上面 ACO=0 那一行吃到的是它，
  这也意味着 66–73 s 是一次性成本，落在 LLPC 的编译缓存里，**换驱动版本才会再次支付**。
- `GetInfo` 现已上报 `aco_requested=1 / aco_effective=1`（`Runtime::Info`，结构尺寸同样是按包探测的，
  1.12.1 的 `MochizukiNrInfo` 与我们 vendored 头不同长）。
- `mochizuki_nr.log` 里的 `ACO records 58/58 match aco/shaders` 与
  `ACO network ready: 29 imported pipelines` 是 ACO 真的生效的证据，不是“设了就算”。

### 等待状态

新阶段 `NeuralRenderPhase::BuildingNetwork`（追加在枚举末尾，`IsKnownPhase` 同步放宽）：

- worker 在 `evaluator.Initialize()` 之前发一条该阶段的 progress（只有 AMD+mochizuki）；
- 播放器 `warm_up` 多一步 `BuildingNetwork`，文案
  `building the network (a first build takes about a minute, later ones seconds)`；
- `StateForProgressPhase` 把它映射到 `Acquiring`（会话还在起，不是渲染停滞）。

也就是说：第一次（或换驱动后）那次 60 多秒，缓冲窗现在会明说是**在建网**，而不是冻在
“0.0 s buffered”。

## 12. Mochizuki 选项进 Neural settings（与 lmxxf 分开）

**右栏按运行时生成，不是共用**：`BuildNeuralSettingControls` 在 AMD+mochizuki 时调用
`BuildMochizukiCostGroup`，lmxxf 走原来的列。区别：

| lmxxf 右栏 | mochizuki 右栏 |
|---|---|
| Passes（含 0.5× Small） | Passes（1/2/3，无 Small —— Small 是 lmxxf 的 0.44 网络） |
| Skin protect（多 pass 蒙版）、Chained history | **Highlight guard**（1.00–8.00，默认 2.00） |
| Multi-pass predict / skip | **History strength**（0–100 %） |
| Temporal mode（Off/Fast/EMA/EMA+detail） | **Render the network**（关＝原帧直通，A/B） |
| Stability（Off/Low/Medium/High） | **Preprocess · auto exposure, filmic** |
| Guides | **INT4 mixed network** + Guides |

窗口标题带上运行时名（`Neural settings · mochizuki`），切换运行时会**重建面板**（两套列共用坐标，
就地改不了）。

**数据链路**（一个结构，一条参数，一个 ini 段）：

```
MochizukiOptions (NeuralRenderTypes.h)
  → RenderSettings.mochizuki → NeuralJobInputs → StageExport::Job
  → NeuralRenderRequest.mochizuki → 命令行 --amd-mochizuki="k=v,..."（默认值不发）
  → AmdLmxxfEvaluatorConfig.mochizuki → MochizukiNrSetControls（PrepareSession 之前）
```

- **默认值＝空串**：默认面板不发参数，老 helper 的命令行逐字节不变，缓存键也不变。
- **缓存键**：`AmdNeuralSettingsDocument` 多一项 `mochizuki=`，**仅在 mochizuki 且非默认时**写入；
  lmxxf 的键逐字节不变（PolicyTests 钉住）。
- **ini**：`[AmdNeural] MochizukiMaxRatio / MochizukiHistory / MochizukiApply / MochizukiPreprocess /
  MochizukiInt4 / MochizukiDrs`。**Intensity / Local tone / Local structure / Skin structure /
  Automatic mask 不在其中**——它们就是 `[NeuralSettings]` 的那五个键，建 job 时拷进去，
  避免两处写同一个值。
- **会重建网络的项**：Preprocess、INT4（运行时的规则），这两条在 tooltip 里写明了。
  `drs_mode` 只走 ini（视频一个 job 一个分辨率，面板上不占位）。
- **真机验证**（`DLSS5_MZ_OPTIONS` 喂同一个 wire 格式给 smoke）：
  `controls(intensity=1.250 style=1 tone=0.50 structure=1.50 skin=0.75 mask=0)`，
  `prep(want=1 latch=1 active=1 mode=1 curve=3)` —— 值真的到了运行时，不只是到了我们的结构体。

### 已知缺口（不要在文档里假装有）

- **Color strength 在 AMD 上是死的**（两个运行时都一样）：它是每帧字段，求值器恒发 1.0。
  NVIDIA 的 add-on 会读它，所以这一栏只在 NVIDIA 上有效。要修就同时改 lmxxf 的渲染，
  属于另一轮（会改变既有 lmxxf 出片）。
- **Intensity 上限仍是 1.00**（`kIntensityMax`，NVIDIA 实测 >1 无效），而 mochizuki 接受 0–2。
  放开它要动 `CanonicalNeuralSettings` 的折叠规则和缓存身份，没做。
- `linear_input` / `max_passes` / `compact_transfer` / `enlarge_mode` / `classic_scaler` /
  RenoDX 整块（`reno_*`）没有进面板：前三项要么是重建项要么没有画面差别，后两块是另一张网络。

## 13. 画质 A/B：mochizuki 到底比 lmxxf 好在哪（2026-10-30）

观感上"mochizuki 更好看"这件事，可以测，但不能用一个数测。下面这套是实际跑出来的，
连同**两个被我判为不可用**的指标一起记下来，免得下次又拿它们下结论。

### 13.1 测法

新增 `tests/AmdNrAbCompare.cpp`（`tests/HardwareTests.cmake` 里用 `dlss_video_player_test()` 建目标，
**不注册 ctest**——它不是断言型测试，是量具；一个量具没有 pass/fail，只有读数），
配套指标脚本 `tools/ab-metrics/ab_metrics.py`：

```
AmdNrAbCompare <runtimeDir> <input.bgra> <w> <h> <fps> <frames> <outDir>
  -> <outDir>/lmxxf/NNNNN.bgra 与 <outDir>/mochizuki/NNNNN.bgra
```

输入是 **raw BGRA 序列而不是视频文件**：解码在外部做一次，两个 arm 读到逐字节相同的帧，
不会有一个 arm 在解码器上赢。引导场由渲染器自己算（同一套 estimator），也不手动喂。

素材：`tools/benchmark/fixtures/demo-capture-20260912.mp4`，

```
crop=1280:720:500:149      # 砍掉播放器面板控件栏与左右黑边，只留画面
mid.mkv                    # ffv1 rgb24 无损
  -> truth.bgra / truth720.bgra       # 无损下采样到目标尺寸（640x360 那份 scale 过）
  -> input.mp4 / input720.mp4         # 同尺寸 x264 crf28，模拟低码率源
```

两份都从**同一个 mid** 派生，所以帧索引严格一致（66 帧 / 30 fps / 2.2 s），不需要对齐。

### 13.2 数据

**640×360**（左＝与真值比，越小越好；temporal 是无参考的帧间运动比）

| | PSNR↑ | 平坦区噪声↓ | 误差抖动↓ | temporal | 高频细节 | 色偏↓ | **低频闪烁↓** |
|---|---|---|---|---|---|---|---|
| 真值 | — | 0 | 0 | 1.000 | 1.000 | 0 | **7.55** |
| 压缩输入（未处理） | 33.96 | 2.27 | 3.34 | 0.944 | 0.962 | 0.662 | **7.35** |
| lmxxf | 25.25 | 8.78 | 10.27 | 1.283 | 1.038 | 0.458 | **10.32** |
| mochizuki | **27.74** | **6.78** | **7.12** | **1.083** | 0.945 | 1.750 | **8.44** |

**1280×720**

| | PSNR↑ | 平坦区噪声↓ | 误差抖动↓ | temporal | 高频细节 | 色偏↓ | **低频闪烁↓** |
|---|---|---|---|---|---|---|---|
| 真值 | — | 0 | 0 | 1.000 | 1.000 | 0 | **8.95** |
| 压缩输入 | 37.28 | 1.94 | 2.51 | 0.958 | 0.976 | 0.634 | **8.68** |
| lmxxf | 26.24 | 9.73 | 9.18 | 1.252 | 1.017 | 1.365 | **11.20** |
| mochizuki | **27.36** | 9.73 | **7.36** | **1.117** | 0.935 | **0.280** | **9.97** |

每帧网络耗时（runtime 自报）：mochizuki 更快，也更稳。

| | lmxxf | mochizuki |
|---|---|---|
| 640p 单帧网络 | 3.22 ms | **1.98 ms**（p95 2.02） |
| 720p 单帧网络 | 8.79 ms | **3.48 ms**（p95 3.53） |
| 66 帧全程 | 2693 / 3225 ms | **2179 / 2540 ms** |

### 13.3 结论

**唯一跨分辨率稳定的差异是低频时间稳定性**（先把画面 4× 均值池化去掉高频，再算时间二阶差分
`|y[2:] - 2y[1:-1] + y[:-2]|`，即"每帧重新摇一次"的程度）：

| | 内容本身 | mochizuki | lmxxf |
|---|---|---|---|
| 640p | 7.55 | 8.44（+12%） | 10.32（**+37%**） |
| 720p | 8.95 | 9.97（+11%） | 11.20（**+25%**） |

即 **lmxxf 每帧都在重新决定画面，mochizuki 基本贴着内容走**。这是观看时最容易察觉的劣化
（运动镜头里的"沙沙"感），也正是"看起来更好"最可能的来源。

次要结论，方向一致但幅度小：
- PSNR 一致略好（+2.5 dB @640p、+1.1 dB @720p）。
- **锐度是 lmxxf 略强**（高频能量 +3.8% @640p / +1.7% @720p；强边缘梯度比 1.006，mochizuki 0.915，
  均为 640p）。喜欢"更锋利"的观感，lmxxf 反而更醒目；代价就是上面那 2~3 倍的额外抖动。

### 13.4 两个被剔除的指标（不要在别处引用）

换一个分辨率就**方向翻转**，按"换个尺度还能不能复现"的判据作废——不是"分不开好坏"，
而是"分得开但结论不稳定"：

| 指标 | 640p | 720p |
|---|---|---|
| 平坦区噪声 | mochizuki 好 23%（6.78 vs 8.78） | **打平**（9.73 vs 9.73） |
| 色偏 | lmxxf 好（0.458 vs 1.750） | **mochizuki 好**（0.280 vs 1.365） |

它们测的是"这一帧内容 + 这一个缩放比的巧合"，不是运行时的性质。

### 13.5 ⚠️ PSNR 单独用会得出反向结论

两个后端的 PSNR（25~27）**都远低于什么都不做的压缩输入（34~37）**。照这个数读，结论会是
"神经渲染在毁画"。不成立：降噪和细节重建本来就会偏离原始像素，PSNR 惩罚一切重建。

本轮一开始就是被这个数带偏的，第一版报告写成"后端把噪声放大 3~4 倍"，实际是**指标实现错了**
（加权残差没除以权重和）**加上取帧取错了**（`-frames:v 1` 取的是第 0 帧，不是你以为的那帧；
序列要 `-vf select=eq(n\,30) -fps_mode passthrough`）。两次都靠"已知好/已知坏对照"发现的。

### 13.6 复现

```bash
# 两个后端各渲染一遍（同一份输入；outdir 下得到 lmxxf/ 与 mochizuki/ 两套帧）
./build-ninja/AmdNrAbCompare.exe amd-neural-runtime input.bgra 1280 720 30 66 frames

# 指标（需要 numpy；代码在 tools/ab-metrics/ab_metrics.py）
python tools/ab-metrics/ab_metrics.py <workdir> 1280 720 --frames frames
```

`<workdir>` 里要有 `truth.bgra`（干净参考）、`input.bgra`（喂给运行时的退化序列），
以及 `--frames` 指的那层目录下的 `lmxxf/NNNNN.bgra` 与 `mochizuki/NNNNN.bgra`。
输出列与本节的表一一对应，标定用的 640p/720p 数据可由上面的命令复算出来。

### 13.7 没覆盖的

- **单段素材、66 帧**。换片、换内容类型（动画 / 胶片颗粒 / 高对比夜景）结论可能不同。
- **两边都是各自默认配置**：lmxxf 默认 2 passes，mochizuki 1 pass。所以 mochizuki 是**用更少的
  计算量做到了更稳**，这个方向对结论有利，但"同 pass 数对比"没做。
- 只测了 `--amd-nr-runtime` 两条默认路径，没测 INT4 / Preprocess / 不同 Highlight guard。
- 素材是 1080p 录制下采样 + x264 重压，不是真实低码率片源。

## 14. ACO 到底开没开：读哪一行才算数（2026-10-31）

**结论：默认开着，而且在本机（9070 XT / 26.10.1 LLPC）确实生效。**

代码侧：`AmdLmxxfEvaluator::ApplyMochizukiControls()` 里 `controls.aco_mode =
AcoModeFromEnvironment()`，即**默认 1（开）**，只有环境变量 `DLSS5_MZ_ACO=0` 才关。
面板上没有这个开关——ACO 在 Vulkan 设备创建时就定死了，改它必须重建会话，所以只留了
环境变量这一个逃生口。

### 14.1 判据（三个来源，按可信度排）

1. **运行时自己的日志** `amd-neural-runtime/mochizuki_nr.log`，建网时会连着写三行：

   ```
   [mochizuki] ACO records 58/58 match aco/shaders; shells native=53 alias=5 self=0
   [nr] ACO network: pipeline binaries on AMD Radeon RX 9070 XT, 26.10.1 (LLPC) (driverVersion 0x008001a0)
   [mochizuki] ACO network ready: 29 imported pipelines
   ```

   以及每帧 input 行里的 `aco=1`。**必须看到 `ACO network ready: 29 imported pipelines`**，
   光有 `ACO records 58/58 match` 只说明包里的记录文件对得上，不代表导入成功。

2. **探针读数**（`build-mz/AmdMochizukiRuntimeProbe.exe`，见 14.3 复现）：

   ```
   --- step 6b SetControls (aco_mode is decided here) ---
     SetControls -> OK (struct_size 532), aco_mode requested=1 (DLSS5_MZ_ACO=0 turns it off)
     ACO: requested=1 effective=1
     session: network 1920x1080, passes=1, build 1.719839 s
   ```

   `requested` 是我们给的值，`effective` 是运行时实际走成的。**两者都是 1 才算开**；
   失败时 `effective=0` 且括号里带 `aco_fallback` 的原因文本（驱动编译 / PAL 拒绝）。

3. 播放器日志 `run-fg/DLSSVideoPlayer.log` 的 `  ACO: requested=1 effective=?` 行
   （`AmdLmxxfEvaluator` 在建网后打印）。**这行只在网络真的建起来后才出现**，
   receipt 里 `failure=cancelled` 的会话不会有。

### 14.2 🐞 三个会让"ACO 没开"被误判成"开了"的坑

- **别用建网耗时判断。** 冷启动时 ACO 是 1.5 s vs 驱动编译 66 s，看着很好分；
  但 `dlssnr-amd/pipeline.cache` 一旦命中，**关掉 ACO 也只有 1.38 s**（实测），
  和开着的 1.72 s 分不开。所以耗时是旁证，`aco_effective` 才是判据。
- **别在 `PrepareSession` 之后立刻读 `GetInfo`。** 那时网络还没建，
  `aco_effective` 恒 0、`model_w/h` 是 0x0、`build_seconds` 是 0——
  这是"还没发生"，不是"失败"。必须在首帧建网之后读（播放器在
  `WaitForMochizukiNetwork()` 之后读，探针在 step 9-13 之后读）。
- **`SetControls` 夹在 Create 与 PrepareSession 中间。** 早于 Create 会被拒：
  `set-controls before the session exists | GetApi: unsupported abi_version`；
  晚于 PrepareSession 则 ACO 已经定死，值到了也不改（运行时原话是"next game start"）。

### 14.3 复现

```bash
source /f/tmp/msvcenv.sh
cl /nologo /std:c++20 /EHsc /MD /O2 /W4 /WX /D NOMINMAX /D _CRT_SECURE_NO_WARNINGS \
   /D WIN32_LEAN_AND_MEAN /D UNICODE /D _UNICODE /c /Fo:build-mz/AmdMochizukiRuntimeProbe.obj \
   tests/AmdMochizukiRuntimeProbe.cpp /I src /I third_party/lmxxf /I third_party/mochizuki
link /nologo /OUT:build-mz/AmdMochizukiRuntimeProbe.exe \
   build-mz/AmdMochizukiRuntimeProbe.obj build-mz/AmdLmxxfRuntime.obj \
   bcrypt.lib d3d12.lib dxgi.lib ole32.lib

./build-mz/AmdMochizukiRuntimeProbe.exe 'F:\dlss5-video\dlss5-video-amd\amd-neural-runtime'
# 对照（已知坏）：关掉，requested=0 effective=0
DLSS5_MZ_ACO=0 ./build-mz/AmdMochizukiRuntimeProbe.exe 'F:\dlss5-video\dlss5-video-amd\amd-neural-runtime'
```

探针要传**绝对路径**的 runtime 目录：传相对路径 `amd-neural-runtime` 会在
`LoadLibraryExW` 上失败（GetLastError=87），看着像包坏了其实是参数问题。

