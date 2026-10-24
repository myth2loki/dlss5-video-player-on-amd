# 统一 GPU 资源调度器实现方案

## 文档状态

- 状态：P0/P1 已实现，待 Windows 11 + RX 9070 XT 构建验证
- 日期：2026-10-24
- 目标分支：`main-subtitle`
- 目标平台：Windows 11 x64
- 目标显卡：AMD Radeon RX 9070 XT，RDNA 4
- 目标工程：DLSS 5 Video Player AMD
- 文档语言：中文
- 文档位置：`docs/GPU-SCHEDULER-DESIGN-zh.md`

本文档描述统一 GPU 调度器的实现方案。第一阶段不添加 ASR、翻译或其他新功能，只建立可观测、可测试、可逐步强制执行的 GPU 资源控制层。

## 1. 目标

统一 GPU 调度器的目标是：

- 以 GPU 适配器为单位统一观察视频播放相关 GPU 资源。
- 统一描述神经渲染、超分、插帧和未来字幕任务的优先级。
- 在任务启动前进行显存和播放压力准入。
- 在资源不足时按固定顺序降级。
- 保护正常播放和神经渲染，不让低优先级任务触发无必要的缓冲暂停。
- 为后续 GPU ASR、GPU 翻译和其他 AI 功能提供统一接入边界。
- 让 GPU 资源行为可测试、可记录、可回放、可解释。

## 2. 非目标

第一阶段明确不做：

- 不接管 D3D12、Vulkan 或 HIP 命令提交。
- 不实现跨 API 的统一命令队列。
- 不实现 GPU 硬件级抢占。
- 不改变现有画面、字幕或播放行为。
- 不引入 ASR、翻译或 OCR。
- 不自动选择显卡。
- 不自动修改超分或插帧设置。
- 不把调度器做成第二个渲染器。

调度器定位为控制面：

```text
观察资源
计算压力
批准或拒绝任务
通知客户端降级
记录结果
```

它不负责数据面：

```text
不搬运纹理
不执行推理
不录制 GPU 命令
不等待 GPU fence
不创建交换链
```

## 3. 核心设计决策

### 3.1 先做进程内协调器

第一阶段调度器只存在于播放器进程。

原因：

- 播放器进程掌握播放状态、神经渲染会话、音频时钟和用户操作。
- D3D12Renderer 可以直接提供显存和画面状态。
- NeuralWorker 已经通过协议向播放器报告显存、进度和结果。
- 跨进程共享 D3D12 fence 不现实，不应该作为第一版前提。

未来 GPU 字幕 helper 必须通过播放器申请任务许可，而不是自行决定启动模型。

### 3.2 不在每帧路径上做调度决策

调度决策发生在：

- 播放健康采样周期，现有代码约每 2 秒一次。
- 神经渲染 job 开始和结束。
- GPU 字幕 chunk 开始和结束。
- 媒体打开、关闭、seek 和切音轨。
- 显存或设备异常事件。

渲染帧路径只读取原子快照，不加调度器锁。

### 3.3 使用租约而不是回调式暂停

任务启动前申请 GPU 租约：

```text
Request -> Admit / Reject -> Lease -> Release
```

租约是 RAII 对象。任务结束、取消、崩溃恢复和媒体切换都必须释放租约。

运行中的任务不会被调度器直接杀死。调度器只能请求客户端在安全点停止或降级。

### 3.4 持久任务按会话计费

神经渲染和播放超分不是单帧临时任务。

建议：

- 神经渲染按实时会话或渲染 job 持有租约。
- 超分按渲染器会话持有租约。
- 插帧按插帧会话持有租约。
- GPU ASR 按识别 chunk 持有租约。
- GPU 翻译按翻译批次持有租约。
- 缓存预生成按单文件任务持有租约。

不应为每个视频帧申请、释放一次租约。

### 3.5 播放关键任务不可被拒绝

优先级最低的任务可以拒绝，播放关键任务不能。

播放关键任务包括：

- 当前神经渲染会话。

超分和插帧属于 `PlaybackSupport`。它们可以降级或关闭，以保护基础播放和神经渲染，但不允许被后台字幕或缓存任务无序挤占。

如果播放关键任务本身无法满足资源，调度器应产生保护事件，让播放器决定切回原画、暂停神经渲染或关闭附属功能。

### 3.6 第一版只观测

第一阶段采用影子模式：

- 记录所有现有 GPU 功能状态。
- 计算压力等级。
- 记录将会拒绝或暂停的任务。
- 不实际拒绝。
- 不改变开关和画面。

影子模式必须至少运行完整的真机测试矩阵后，才允许进入准入控制。

## 4. 总体架构

```text
PlayerApp
  |
  +-- GpuScheduler
  |     |
  |     +-- GpuSchedulerPolicy       纯策略，可单测
  |     +-- GpuRuntimeState          GPU 快照和压力状态
  |     +-- GpuReservationRegistry   租约与预留
  |     +-- GpuSchedulerReceipt      低频日志和统计
  |
  +-- D3D12Renderer
  |     +-- 显存点采样
  |     +-- 当前神经渲染/超分/插帧状态
  |     +-- 最近 GPU 时间
  |
  +-- NeuralWorker 协议
  |     +-- WireMemory
  |     +-- WireProgress
  |     +-- WireResult
  |
  +-- 未来 SubtitleWorker
        +-- 请求任务许可
        +-- 报告实际显存
        +-- 在安全点释放资源
```

数据流：

```text
Renderer / Worker 采样
  -> PlayerApp 汇总为 GpuRuntimeSample
  -> GpuScheduler.Observe
  -> GpuSchedulerPolicy 计算压力
  -> GpuScheduler 发布原子快照
  -> 播放器和未来 worker 读取快照
```

准入流：

```text
Worker 请求租约
  -> GpuScheduler.TryAcquire
  -> Policy 返回批准或拒绝
  -> 批准时创建 GpuLease
  -> Worker 启动任务
  -> 任务结束释放 GpuLease
```

## 5. 模块划分

### 5.1 新增文件

建议新增：

```text
src/GpuSchedulerPolicy.h
src/GpuScheduler.h
src/GpuScheduler.cpp
tests/GpuSchedulerTests.cpp
```

### 5.2 GpuSchedulerPolicy.h

纯头文件，不依赖 Windows、D3D12、DXGI 或具体推理后端。

包含：

- 工作负载类型。
- 优先级。
- 压力等级。
- 拒绝原因。
- 运行时快照。
- 申请描述。
- 准入策略。
- 迟滞策略。
- 降级动作选择。

可以只依赖标准库，并由 `GpuSchedulerTests` 独立测试。

### 5.3 GpuScheduler.h/.cpp

属于 `player_app` 对象库。

原因：

- 调度器由 PlayerApp 拥有。
- `player_render` 和 `player_neural` 不应反向依赖 `player_app`。
- NeuralWorker 不需要实例化播放器调度器。
- 播放器通过现有 renderer getter 和 worker 协议采样。

CMake 计划：

- 把 `src/GpuScheduler.cpp` 和 `src/GpuScheduler.h` 加入 `player_app`。
- 把 `src/GpuSchedulerPolicy.h` 加入 `DLSSVideoPlayer` 的头文件源列表。
- 新增 `GpuSchedulerTests`，只包含纯策略头，不链接 GPU 或播放器对象库。
- 不让 `NeuralWorker` 链接 `player_app`。

### 5.4 D3D12Renderer

第一阶段不直接依赖 `GpuScheduler`。

Render 侧只提供无所有权快照：

```text
CurrentLocalVideoMemoryMiB
PeakLocalVideoMemoryMiB
DLSSEnabled
UpscalePresentActive
PlaybackFrameGenInterpolating
LastNeuralGpuMs
GpuUnusable
LastFenceWaitResult
```

PlayerApp 读取并交给调度器。

这样可以避免：

- 在 `D3D12Renderer.cpp` 中引入调度器全局状态。
- NeuralWorker 链接 `player_app`。
- 在每帧渲染路径中加锁。

### 5.5 NeuralWorker 协议

影子阶段复用现有字段：

- `WireMemory.localVramMiB`
- `WireResult.peakLocalVramMiB`
- `WireResult.neuralGpuMsP50`
- `WireResult.neuralGpuMsP95`
- `WireProgress`

不升级协议版本。

未来加入任务准入命令时，再升级协议版本，增加：

- RequestLease
- LeaseGranted
- LeaseRejected
- ReleaseLease
- CancelForPressure

## 6. 核心类型

### 6.1 工作负载类别

```cpp
enum class GpuWorkClass : uint8_t {
    PlaybackCritical,
    PlaybackSupport,
    Interactive,
    Background,
    Maintenance,
};
```

含义：

| 类别 | 示例 | 是否可拒绝 |
| --- | --- | --- |
| `PlaybackCritical` | 当前神经渲染会话 | 否 |
| `PlaybackSupport` | 超分、插帧 | 压力高时可降级 |
| `Interactive` | 当前播放头附近 ASR | 可暂停 |
| `Background` | 字幕翻译、下一集预生成 | 可暂停 |
| `Maintenance` | 缓存整理、模型校验 | 可无限延后 |

### 6.2 工作负载类型

```cpp
enum class GpuWorkKind : uint8_t {
    NeuralRender,
    Upscale,
    FrameGeneration,
    SpeechRecognition,
    Translation,
    Ocr,
    CachePrefetch,
    Maintenance,
};
```

`WorkClass` 表示用户重要性，`WorkKind` 表示实际技术类型。

### 6.3 调度模式

```cpp
enum class GpuSchedulerMode : uint8_t {
    Observe,
    Admit,
    Enforce,
};
```

- `Observe`：只记录，不拒绝。
- `Admit`：新任务需要租约，已在运行的任务不受影响。
- `Enforce`：压力高时主动通知客户端在安全点降级。

第一版只使用 `Observe`。

### 6.4 压力等级

```cpp
enum class GpuPressure : uint8_t {
    Normal,
    Soft,
    Hard,
    Critical,
};
```

策略：

| 压力 | 行为 |
| --- | --- |
| `Normal` | 所有类别按优先级允许 |
| `Soft` | 拒绝 Maintenance、暂停 Background 新任务 |
| `Hard` | 拒绝 Interactive 新任务，允许已有任务完成当前小批次 |
| `Critical` | 只允许 PlaybackCritical，请求所有低优先级任务停止 |

### 6.5 拒绝原因

```cpp
enum class GpuRejectReason : uint8_t {
    None,
    WrongAdapter,
    InvalidRequest,
    InsufficientVram,
    Pressure,
    PlaybackProtected,
    Cancelled,
    StaleGeneration,
    DeviceLost,
};
```

拒绝必须有可记录原因，不能只返回 false。

### 6.6 运行时采样

```cpp
struct GpuRuntimeSample {
    uint64_t adapterLuid;
    uint64_t localBudgetBytes;
    uint64_t localUsageBytes;
    uint64_t peakLocalUsageBytes;

    double frameBudgetMs;
    double frameP50Ms;
    double frameP95Ms;
    uint64_t droppedFrames;

    double neuralRealtimeRatio;
    double neuralLeadSeconds;
    bool neuralActive;

    bool upscaleActive;
    bool frameGenerationActive;
    bool rendererUsable;

    uint64_t workerLocalUsageBytes;
    uint64_t sampleSequence;
    int64_t sampleTimeMs;
};
```

字段来源：

- `localBudgetBytes`、`localUsageBytes`：DXGI Adapter3。
- 帧时间：PlayerApp 的播放健康窗口。
- `droppedFrames`：现有计数器。
- `neuralRealtimeRatio`、`neuralLeadSeconds`：现有 live session 数据。
- 超分和插帧：D3D12Renderer 状态。
- worker 显存：NeuralWorker `WireMemory` / `WireResult`。

### 6.7 任务申请

```cpp
struct GpuWorkloadRequest {
    GpuWorkKind kind;
    GpuWorkClass workClass;
    uint64_t adapterLuid;
    uint64_t estimatedVramBytes;
    uint64_t generation;
    uint32_t estimatedDurationMs;
    bool cancellable;
    bool canTimeSlice;
    bool persistent;
};
```

要求：

- `estimatedVramBytes` 必须来自模型或运行时测量，不能填 0 绕过准入。
- `generation` 在媒体切换和 seek 时更新。
- `cancellable` 表示任务是否能在 chunk 或 batch 边界停止。
- `persistent` 表示会话级租约。

### 6.8 租约

```cpp
class GpuLease {
public:
    GpuLease() = default;
    ~GpuLease();
    GpuLease(GpuLease&&) noexcept;
    GpuLease& operator=(GpuLease&&) noexcept;
    GpuLease(const GpuLease&) = delete;
    GpuLease& operator=(const GpuLease&) = delete;

    bool Valid() const noexcept;
    void Release() noexcept;
    uint64_t Id() const noexcept;

private:
    std::shared_ptr<gpu_scheduler::LeaseState> state_;
};
```

约束：

- 析构不得等待 GPU、fence 或 worker。
- 析构只允许在短临界区内更新租约状态，不写日志。
- 重复 Release 必须安全。
- 调度器关闭后不得访问悬空状态。
- 进程退出时未释放租约必须被记录。

### 6.9 调度器接口

```cpp
class GpuScheduler final {
public:
    explicit GpuScheduler(GpuSchedulerLimits limits = {});
    ~GpuScheduler();

    void SetMode(GpuSchedulerMode mode) noexcept;
    GpuSchedulerMode Mode() const noexcept;

    void Observe(const GpuRuntimeSample& sample) noexcept;
    GpuPressureSnapshot Snapshot() const noexcept;

    GpuAdmissionResult TryAcquire(const GpuWorkloadRequest& request);
    void CancelGeneration(uint64_t generation) noexcept;
    void ResetSession(uint64_t generation) noexcept;

    GpuSchedulerReceipt Receipt() const;
};
```

线程规则：

- `Observe` 和 `Snapshot` 可以在任意线程调用。
- `TryAcquire` 只能在 worker 或 UI 调度线程调用，不得在每帧 present 路径调用。
- `TryAcquire` 不应执行阻塞 GPU 查询。
- 内部锁只覆盖租约表和准入状态，不覆盖渲染。

## 7. 显存预算设计

### 7.1 数据来源

使用 DXGI 的适配器本地预算：

```text
DXGI_MEMORY_SEGMENT_GROUP_LOCAL
Budget
CurrentUsage
```

注意：

- `CurrentUsage` 是整个适配器的本地段使用，不是单进程。
- 其它应用占用显存也会计入。
- 适配器预算会随桌面、其它 GPU 应用和驱动状态变化。
- 不能把 `Budget` 当作固定物理显存容量。

### 7.2 安全余量

默认保留预算：

```text
reserve = max(512 MiB, budget * 10%)
```

这个值只是起点，必须在 RX 9070 XT 真机测试中确认。

### 7.3 预留与实际使用

租约中的 `estimatedVramBytes` 是预留，不一定立刻变成实际使用。

调度器维护：

```text
actualUsage
pendingReservations
committedReservations
```

规则：

- 租约批准时进入 `pendingReservations`。
- 任务首次报告实际显存后，把租约转为 `committedReservations`。
- 同一租约不能同时全额计入 pending 和 committed。
- 实际使用仍通过适配器采样观察。
- 任务释放租约时删除对应预留。

### 7.4 准入公式

保守公式：

```text
effective = max(actualUsage, appCommittedEstimate) + pendingReservations
admit = effective + request.estimatedVramBytes + reserve <= budget
```

如果无法获得可靠预算：

```text
只允许 PlaybackCritical
拒绝所有新增非关键任务
记录 UnknownBudget
```

### 7.5 过度占用

如果实际显存超过预留：

- 立即把压力提升到至少 `Soft`。
- 更新租约的实测估计。
- 禁止新的 Background 和 Maintenance 任务。
- 记录实际值与预留值。

### 7.6 显存释放

租约释放不等于显存立即归还。

策略：

- 模型释放后继续观察至少两个采样周期。
- 若实际显存未下降，记录 `release-unobserved`。
- 不因为一次采样未下降就认定失败。
- 驱动或运行时延迟释放属于正常现象，但必须可见。

## 8. GPU 时间与播放压力

普通 Windows 桌面应用不能可靠获得各 D3D12/Vulkan HDR 队列的精确 GPU 时间。

因此不把厂商 GPU 利用率作为唯一判据。

使用以下信号：

- 源帧预算。
- 最近窗口的 p50/p95 帧时间。
- 掉帧数。
- 神经渲染相对实时比。
- 神经渲染前置距离。
- 是否进入 live buffering。
- NeuralWorker 自报的 GPU 时间。

### 8.1 帧预算

```text
frameBudgetMs = 1000 / sourceFps
```

压力判断示例：

```text
frameP95Ms <= 0.90 * frameBudgetMs   Normal
frameP95Ms <= 1.00 * frameBudgetMs   Soft
frameP95Ms <= 1.20 * frameBudgetMs   Hard
frameP95Ms >  1.20 * frameBudgetMs   Critical
```

这些阈值必须可配置且可测试，不应散落在业务代码中。

### 8.2 神经渲染前置

```text
lead >= 6 秒      正常
3 <= lead < 6     软压力
1 <= lead < 3     硬压力
lead < 1          临界
```

字幕或后台任务必须在 `Hard` 阶段停止，不能等到 `lead < 1`。

### 8.3 迟滞

为避免压力等级抖动，使用进入和退出双阈值。

示例：

| 等级 | 进入条件 | 退出条件 |
| --- | --- | --- |
| `Soft` | p95 超过 90% 预算或显存超过 78% | 连续 4 个样本低于 80% 和 70% |
| `Hard` | p95 超过 100% 或显存超过 88% | 连续 4 个样本低于 90% 和 78% |
| `Critical` | 进入 buffering、lead 小于 1 秒或分配失败 | 连续 6 个样本正常且 lead 大于 6 秒 |

压力等级只允许逐级上升，可以按策略跨级下降或逐级下降。

### 8.4 采样频率

- 正常播放：每 2 秒一次，复用现有 Playback health 周期。
- 新任务申请：读取最近快照，不额外查询。
- job 边界：可主动采样一次。
- 显存分配失败：立即进入 Critical。
- 设备移除：立即失效当前适配器状态。

## 9. 准入策略

### 9.1 优先级规则

```text
PlaybackCritical > PlaybackSupport > Interactive > Background > Maintenance
```

低优先级任务不能阻塞高优先级任务。

### 9.2 Normal

允许所有类别，但非关键任务仍需满足显存预算。

### 9.3 Soft

- PlaybackCritical：允许。
- PlaybackSupport：允许。
- Interactive：允许一个小批次。
- Background：拒绝新任务。
- Maintenance：拒绝。

### 9.4 Hard

- PlaybackCritical：允许。
- PlaybackSupport：允许，但准备降级。
- Interactive：拒绝新任务，已有任务完成当前小批次后停止。
- Background：拒绝。
- Maintenance：拒绝。

### 9.5 Critical

- PlaybackCritical：允许。
- 其它类别：拒绝新任务。
- 已有 Interactive 和 Background：请求在安全点停止。
- 若播放关键任务仍无法获得资源，产生 `PlaybackProtectionRequired` 事件。

### 9.6 饥饿避免

Background 在长时间 `Normal` 后允许获得最低配额。

建议策略：

- 至少每 30 秒检查一次是否有可运行 Background。
- 只在没有播放关键任务压力时运行。
- 不因为预生成任务导致播放质量下降。

如果实现复杂，第一版可以不做最低配额，只保留可配置接口。

## 10. 租约生命周期

### 10.1 获取

```text
Worker 构造请求
  -> 检查 generation
  -> TryAcquire
  -> 批准：获取 GpuLease
  -> 拒绝：记录原因，按指数退避重试或放弃
```

### 10.2 运行

任务运行期间：

- 每完成一个 chunk 或 batch 重新检查压力快照。
- `Hard` 或 `Critical` 时不再启动下一个 chunk。
- 不在 GPU 命令提交一半时强制停止。
- 不共享调度器内部容器给 worker。

### 10.3 释放

释放触发：

- 任务正常完成。
- 任务取消。
- 模型卸载。
- 媒体关闭。
- seek 导致 generation 失效。
- helper 退出或崩溃。
- 播放器退出。

### 10.4 泄漏检测

调度器维护未释放租约的年龄。

若租约超过预期寿命：

- 记录 warning。
- 标记对应 worker。
- 在媒体关闭时强制清理本地租约记录。
- 不直接杀死进程，进程生命周期仍由 Job Object 负责。

## 11. 线程模型与性能

### 11.1 状态分层

建议分开：

```text
atomic pressure snapshot
mutex protected lease registry
periodic receipt writer
```

`Snapshot` 使用原子值或单个不可变快照指针。

### 11.2 帧路径

帧路径允许：

- 读取压力快照。
- 更新已有帧时间统计。

帧路径不允许：

- 查询 DXGI。
- 新建立租约。
- 写日志。
- 等待条件变量。
- 获取长时间互斥锁。

### 11.3 采样线程

第一版不新增常驻线程。

采样使用：

- 主线程 Tick。
- 现有 2 秒健康报告。
- job 生命周期回调。

如果未来 worker 需要异步等待租约，再增加一个轻量调度线程或线程池任务。

### 11.4 开销目标

- 影子模式不能改变现有渲染行为。
- 帧路径增加的成本应低于现有测量噪声。
- 日志频率不超过现有 2 秒健康报告。
- 单次准入决策不做 GPU 查询。

## 12. 现有功能接入

### 12.1 神经渲染

数据：

- `m_liveSession`
- `m_liveAttached`
- live lead
- `NeuralJobActive()`
- `WireMemory`
- `WireResult`

接入方式：

- PlayerApp 向调度器报告当前会话和前置。
- 影子阶段不改变会话。
- 强制执行阶段只阻止下一个 chunk 启动，不终止当前 chunk。

### 12.2 超分

数据：

- `UpscalePresentActive()`
- `LastNeuralGpuMs()`
- 帧时间和掉帧。

接入方式：

- 把超分登记为 `PlaybackSupport` 会话租约。
- 不按每帧申请。
- 仅在压力临界时建议降低质量或关闭。

### 12.3 插帧

数据：

- `PlaybackFrameGenInterpolating()`
- 输出帧率。
- 帧时间和掉帧。

接入方式：

- 把插帧登记为 `PlaybackSupport` 会话租约。
- 插帧质量降级在安全点执行。

### 12.4 未来 GPU ASR

建议：

- `Interactive`。
- chunk 级租约。
- 20 到 30 秒音频窗口。
- 每 chunk 结束重新申请。
- `Hard` 或 `Critical` 不再启动新 chunk。
- seek 或切音轨时使用新 generation。

### 12.5 未来 GPU 翻译

建议：

- `Background`。
- batch 级租约。
- 每次 4 到 8 条字幕。
- `Soft` 及以上不启动新 batch。
- 翻译停止不影响原文显示。

## 13. 跨进程设计

### 13.1 第一阶段

只做播放器进程内观测。

NeuralWorker 数据通过现有协议提供。

### 13.2 未来 helper

GPU 字幕 helper 不能自行决定模型加载和任务启动。流程：

```text
SubtitleWorker -> Player：请求执行 chunk
Player -> GpuScheduler：TryAcquire
GpuScheduler -> Player：批准 / 拒绝
Player -> SubtitleWorker：Grant / Deny
SubtitleWorker -> Player：实际显存和 GPU 时间
SubtitleWorker -> Player：任务完成
Player -> GpuScheduler：Release
```

### 13.3 跨进程限制

- 不共享 D3D12 fence。
- 不共享命令队列。
- 不共享 C++ 对象。
- 只通过版本化协议传递任务许可和测量值。
- 任务许可是准入许可，不是 GPU 时间片。
- helper 必须在 chunk 边界主动停止。

### 13.4 协议安全

- 使用现有固定头部和版本号。
- 限制 payload 大小。
- 不传路径和原始模型文件。
- lease ID、generation 和 adapter LUID 使用固定宽度整数。
- 不接受 helper 自行扩大预留。

## 14. 可观测性

### 14.1 状态快照

```cpp
struct GpuPressureSnapshot {
    uint64_t adapterLuid;
    uint64_t budgetBytes;
    uint64_t usageBytes;
    uint64_t reservedBytes;
    GpuPressure pressure;
    GpuSchedulerMode mode;
    uint64_t generation;
    uint64_t sampleSequence;
    bool playbackProtected;
};
```

### 14.2 低频日志

在现有 2 秒 Playback health 行后追加一行或在同一行追字段。

建议字段：

```text
gpu pressure=Soft vram=7341/15416MiB reserved=2048MiB
frame p95=18.7/16.7ms lead=12.3s
leases=critical:1,support:2,interactive:1,background:0
rejected=3 reason=pressure
```

不要每帧输出。

### 14.3 调度回执

每个媒体会话结束生成摘要：

- 观测时长。
- 各压力等级持续时间。
- 准入请求数量。
- 拒绝数量。
- 按类别拒绝数量。
- 最大显存。
- 最大预留。
- 租约释放异常数。
- 播放 buffering 次数。
- 字幕任务导致的风险事件数。

## 15. 故障处理

### 15.1 无适配器预算

- 记录 `budget-unavailable`。
- 压力视为 Critical。
- 只允许 PlaybackCritical。

### 15.2 设备移除

- 立即把适配器状态标为不可用。
- 取消该适配器全部租约。
- 不自动批准新任务。
- 等渲染器重建后重新采样。

### 15.3 helper 崩溃

- 接收方释放该 helper 的租约。
- 记录租约来源。
- 不影响播放器。
- 允许下一次任务重新申请。

### 15.4 租约泄漏

- 达到阈值记录 warning。
- 在媒体关闭时清理本地记录。
- 不依赖泄漏租约做长期预算。

### 15.5 采样过期

每个样本带 `sampleSequence` 和采样时间。

超过有效期的样本不能降低压力，只能保持或提高压力。

## 16. 测试方案

### 16.1 纯策略测试

新增独立测试目标 `GpuSchedulerTests`，按现有 `AmdLmxxfApiTests` 的模式构建，不依赖 GPU、D3D12 或实际 worker。

建议用例：

- Normal 下允许 Background。
- Soft 下拒绝 Background。
- Hard 下拒绝 Interactive。
- Critical 下仍允许 PlaybackCritical。
- Critical 下拒绝所有非关键类别。
- 显存预算无效时只允许 PlaybackCritical。
- 请求显存大于预算时拒绝。
- 租约释放后预算恢复。
- 多个租约累计不超预算。
- 同一租约不能重复释放。
- generation 取消拒绝旧请求。
- 旧样本不能降低压力。
- 压力迟滞不会在阈值附近抖动。
- adapter LUID 不匹配拒绝。
- 设备移除后所有请求拒绝。
- 租约泄漏检测记录 warning。

### 16.2 影子集成测试

不依赖 GPU 的测试：

- 注入伪造 RuntimeSample。
- 注入伪造 lease 请求。
- 验证日志和回执。
- 验证没有改变渲染状态。

### 16.3 真机测试

Windows 11 + RX 9070 XT：

- 720p24 神经渲染。
- 1080p24 神经渲染。
- 720p30 神经渲染。
- 加超分。
- 加插帧。
- 播放中暂停、seek、切音轨。
- 最小化和全屏切换。
- 连续 30 分钟播放。
- 显存压力外部注入。
- 强制关闭 helper。
- 驱动恢复或 device removed 路径。

### 16.4 验收

影子模式：

- 输出行为不变。
- 帧率和掉帧不回归。
- 日志能解释主要资源状态。
- 没有新增阻塞。
- 没有租约泄漏。

强制模式：

- Critical 时不会批准低优先级任务。
- 字幕任务不能让 live session 进入 buffering。
- 设备移除后不会继续批准任务。
- 关闭媒体后所有租约清零。

## 17. 实施顺序

### P0：数据模型和纯策略

- 新增 `GpuSchedulerPolicy.h`。
- 定义类别、压力、请求、决策。
- 编写纯逻辑测试。
- 不接任何运行时。

### P1：进程内影子和采样

- 新增 `GpuScheduler.h/.cpp`。
- PlayerApp 持有实例。
- 在 2 秒健康报告处采样。
- 接入 renderer 显存和 live session 状态。
- 接入 NeuralWorker `WireMemory`。
- 只写日志。

### P2：租约和准入

- 实现租约注册表。
- 实现 generation 失效。
- 新增只读诊断接口。
- 暂不用于控制现有功能。

### P3：现有功能登记

- 神经渲染登记会话租约。
- 超分登记会话租约。
- 插帧登记会话租约。
- 不改变开关。
- 观察预留与实际显存差异。

### P4：安全点降级

- 在 Hard/Critical 下暂停 Background。
- 在 Critical 下暂停 Interactive。
- 先只对未来的后台任务生效。
- 不允许字幕任务触发播放缓冲。

### P5：GPU 字幕接入

- SubtitleWorker 通过 PlayerApp 申请租约。
- ASR 按 chunk 租约。
- 翻译按 batch 租约。
- 接入缓存和模型卸载。

### P6：可选强制执行

- 增加显式模式配置。
- 默认仍为 Observe。
- 真机通过后再考虑默认 Admit。

## 18. 验收指标

第一阶段必须满足：

- 不改变现有画面。
- 不改变现有功能开关。
- 不改变现有渲染命令顺序。
- 不增加每帧 DXGI 查询。
- 不增加每帧日志。
- 不增加每帧阻塞锁。
- 现有测试全部通过。
- 影子日志覆盖 720p 和 1080p 神经渲染。
- 峰值显存和 worker 报告一致。
- live lead 和现有状态一致。

后续强制执行阶段必须满足：

- 播放关键任务始终被批准。
- Critical 时非关键任务不再启动。
- 关闭媒体后零活跃租约。
- seek 后旧 generation 请求全部拒绝。
- helper 崩溃后无租约泄漏。
- 字幕任务不能单独造成 live buffering。

## 19. 风险与缓解

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| 调度器变成第二个渲染器 | 复杂度失控 | 限定为控制面 |
| 帧路径加锁 | 掉帧 | 只读原子快照 |
| 采样过频 | 性能下降 | 复用 2 秒健康周期 |
| 显存预算波动 | 误判 | 迟滞、安全余量和外部占用 |
| 预留与实际重复计费 | 过度拒绝 | pending/committed 分离 |
| 跨进程许可失效 | 任务重叠 | generation、超时和回执 |
| 运行中任务无法抢占 | 降级延迟 | chunk/batch 安全点 |
| 设备移除 | 状态陈旧 | 立即失效适配器 |
| 过度依赖 GPU 利用率 | 误判 | 使用帧时间和 live lead |
| 日志过多 | 性能问题 | 低频聚合 |

## 20. 待确认问题

- RX 9070 XT 上默认显存安全余量取 10% 还是固定值。
- 现有 PlayerApp 健康窗口中是否已有可直接使用的 p95，若没有需要补环形统计。
- `WireMemory` 是否足以覆盖所有阶段，还是需要新的实时采样字段。
- 超分和插帧是否使用会话级租约，还是由渲染器统一租约。
- 调度器是否需要暴露 UI，第一版建议不暴露。
- 强制模式默认值是否应保持 Observe。
- 未来字幕 helper 使用命名管道还是现有 stdin/stdout 协议。
- 是否允许用户覆盖资源阈值。

## 21. 最终建议

第一版统一 GPU 调度器应当只做四件事：

```text
看见所有 GPU 任务
看见显存和播放压力
能给出准入结论
能记录决策依据
```

不要在第一版做：

```text
统一 GPU 命令队列
硬件级抢占
自动关闭现有画面功能
跨进程共享 D3D12 设备
```

这样可以在不改变用户体验的前提下建立可靠基线。等影子和准入数据稳定后，再让未来的 GPU ASR 和翻译通过租约接入。

这项基础设施的直接价值不是增加一个新功能，而是让后续所有 GPU 功能都遵守同一条规则：

```text
播放优先
神经渲染优先
字幕和翻译按能力降级
任何资源决策都有记录
```

## 22. 当前代码落地

截至 2026-10-24，已落地第一阶段基础：

- `src/GpuSchedulerPolicy.h`：纯策略、压力等级、迟滞和准入规则。
- `src/GpuScheduler.h/.cpp`：进程内调度器、原子快照、租约和回执。
- `tests/GpuSchedulerTests.cpp`：不依赖 GPU、D3D12 或 Windows 的单元测试。
- `CMakeLists.txt`：加入 `player_app` 和 `GpuSchedulerTests`。
- `D3D12Renderer`：暴露适配器 LUID 和视频内存预算。
- `PlayerApp`：每约 2 秒采样一次帧时间、显存、live lead、超分和插帧状态。
- 默认模式仍为 `Observe`，不改变任何现有功能。

本机已使用 C++20 和 AddressSanitizer/UndefinedBehaviorSanitizer 运行 `GpuSchedulerTests`。Windows 11 + MSVC + RX 9070 XT 构建和真机影子数据仍需在目标机器补测。
