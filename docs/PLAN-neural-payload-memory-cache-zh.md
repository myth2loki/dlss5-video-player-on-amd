# 神经渲染落盘数据内存化可行性分析

日期：2026-10-18
范围：`src/NeuralCache.*`、`src/OfflineNeuralRenderer.cpp`、`src/NeuralSegmentIndex.h`、`src/VideoDecoder.cpp`、`src/NeuralWorkerMain.cpp`
状态：**分析稿，未改任何代码**

---

## 结论（先说）

1. **能做，但在这套架构里几乎没有位置可放。** 每块渲完 120 s 立刻 `ReplaceRun` 发布成正式 cache entry，散段随即删除（18:30 的落盘验证已实证：`live/` 为空、`renders/` 每块一个条目）。神经渲染**不存在"长期躺在磁盘上的临时数据"**，只有块内 120 s 的分段这一段很短的中间态。
2. **payload 的磁盘流量比帧管线的内存流量小 1–2 个数量级**（720p：88 MB/s vs 0.33 MB/s；1080p-class：185 MB/s vs ~2 MB/s）。省掉它省不出可感知的东西。
3. **真正的问题是写放大，不是"落盘"本身**：每块 39 MB 的内容，实际产生 **~156 MB 的 IO**（concat 读 39 + 写 39 + 哈希再读 39 + 强制刷 39），实测耗时 **0.94–1.65 s/块**，而且这段就卡在块与块之间（渲染线程阻塞，GPU 空闲）。
4. **内存化只能吃掉这 156 MB 里的 39 MB**（concat 的读那一半）；把 156 降到 39 才是一步到位。**减法优于搬运。**
5. 真正值得"进内存"的对象不是 payload，而是 `OfflineNeuralRenderer` 里已经在内存里的 BGRA 帧队列；再叠 ring 只会推高提交峰值。

推荐：**不做 payload 内存化**；先做 §三的减法（发布不再物理拼接、不再重哈希、异步刷盘），收益是每块省 1 s 级 GPU 空闲 + 4× IO 降 1×。

---

## 一、今天到底落了什么盘（实测）

| # | 数据 | 位置 | 生命周期 | 实测大小 | 能否只留内存 |
|---|---|---|---|---|---|
| 1 | 已完成块的 payload | `cache/v1/renders/<key>/neural.mkv` | 持久、跨会话复用、`Get a copy` 的源 | 720p 块 33–52 MB / 120 s；今天累计 11 条、239 MB | **不能** |
| 2 | live 会话分段 | `cache/v1/live/pid<N>/job<runId>/*.mkv` | **块内 2 s/段，发布后立即 retired 删除** | 每块 61 段 | 能，但只活 ~120 s |
| 3 | 源副本 | `cache/v1/sources/<key>/source.mkv` | 持久 | 与源同尺寸 | 不能 |

分段不是缓存，只是"发布前的临时容器"：播放先读分段（`SynchronizedPlayback.Open(sourcePath, neuralPath, ...)`，`main.cpp:11914`），整块渲完就 `ReplaceRun` 成单一 `neural.mkv`，播放改读条目、分段退休（日志：`Live run N now plays from its published entry; 61 segment files retired`）。

**所以"把落盘数据缓到内存"在这条流水线上等价于"把发布机制换成内存机制"** —— 而发布机制本身才是瓶颈（§三）。

---

## 二、字节账本

### 2.1 帧管线 vs payload（本机实测几何）

| | 1280×720 @23.976 | 1920×966 @25 |
|---|---|---|
| 每帧 BGRA | 3.69 MB | 7.42 MB |
| 帧管线（readback → vector → ffmpeg stdin） | **88 MB/s** | **185 MB/s** |
| payload（实测） | 39.4 MB / 120 s = **2.62 Mbit/s ≈ 0.33 MB/s** | CQ16 HEVC ≈ 17 Mbit/s ≈ **2.1 MB/s** |
| 比值 | **≈ 270 : 1** | **≈ 88 : 1** |

payload 从磁盘搬进内存，最多省 0.33–2.1 MB/s 的顺序写；同期内存侧已在搬 88–185 MB/s。**磁盘不是瓶颈。**

### 2.2 但每块内容被搬运了 4 次（这是真问题）

`Neural publish timing`（本机 17:26–20:54，16 个块）实测：

| 阶段 | 720p 一块（39 MB） | 1080p 级一块（~255 MB） | 干了什么 |
|---|---|---|---|
| `concatMs` | **380–840 ms** | 630–760 ms | 读 61 段 + 写 joined（`MediaPipeline.cpp:1434` concat → copy） |
| `probeMs` | **430–590 ms** | 465–533 ms | ffprobe 读 joined（进程启动为主） |
| `promoteMs` | **100–415 ms** | 165–415 ms | SHA-256 全量读 + `FlushFileBuffers` + rename |
| `totalMs` | **940–1650 ms** | 995–1877 ms | **渲染线程阻塞，GPU 空闲** |

IO 账：39 MB 内容 → concat 读 39 + 写 39 + **probe 又把 joined 读一遍 39** + promote 哈希读 39 + 强制刷 39 = **195 MB（5× 放大）**；1080p 一块约 **1.3 GB**。

还有**第四笔不在表里**的代价：`ReplaceRun` 退休的 61 个分段是在**呈现线程**上删的，实测 **88–102 ms，且播放倒退 103–113 ms、掉 3–6 帧**（`main.cpp:10820-10822` 与 `BackgroundFileReaper.h` 的注释）。所以一块的真实关键路径 = `totalMs` **+ ~90 ms**。

代码注释已经点明这段的代价（`main.cpp:2976-2978`）：*"The next hole cannot start until this returns … one driven session sat 18 s between a finished render and the next one with the GPU idle."*

**内存化能省哪一块？** 只有 `concatMs` 里的"读 39 MB"——而且实测热读整份只要 **11 ms**。`probe`、`hash`、`flush` 全部照旧（它们要的是一份持久的单文件）。**投入产出比极差。**

### 2.3 拆解实测（隔离基准，2026-10-18 本机）

用 `ffmpeg/ffprobe` 复刻代码里的实际参数，在 39 MB / 120 s / 1280×720 / 24 fps 的真实条目上测（脚本 `F:\tmp\nrbench\bench*.py`）：

| 项 | 实测 | 说明 |
|---|---|---|
| 进程启动下限 | **98 ms** | `ffprobe -version`，每次外部工具调用的固定税 |
| packet 探测（full validation） | **136 ms**（39 MB）/ **190 ms**（116 MB） | ≈ 98 启动 + 逐包扫描 |
| 末帧可解码验证（`-sseof -1`） | **146 ms**（39 MB）/ **172 ms**（116 MB） | `ProbeMedia` 里第二个 ffmpeg 调用，代码里没写明但确实存在 |
| ⇒ `probeMs` 合体 | **282 ms** / **358 ms** | 探测 + 末帧，两个独立进程 |
| concat **61 段** | **489 ms** | 与日志的 380–840 ms 吻合 |
| concat 21 段 / 15 段 / 1 段 | 290 / 263 / **117** ms | 每多一个 part ≈ +6 ms |
| SHA-256 | **31 ms**（39 MB）/ **90 ms**（116 MB） | |
| `FlushFileBuffers` | **40–52 ms** | 116 MB |
| 热读整个文件 | **11 ms**（39 MB）/ **32 ms**（116 MB） | **3.5 GB/s**——IO 不是 concat 的成本 |

**三条关键判读**

1. **concat 的 489 ms 里没有多少 IO**：热读整份只要 11 ms，而 concat 要 489 ms。成本是 **61 次文件打开 + 容器解析 + 2881 个包的重新 mux + 98 ms 启动税**，全是 CPU。所以"内存化省掉读"只能省 **11–32 ms**，即 `totalMs` 的 **1–3%**——这就是内存化在这条链上能拿到的全部。
2. **`probeMs` 是两个进程**：packet 探测 + 末帧解码，各自都要付 98 ms 启动税。它不是"读得慢"，是"起得贵"。
3. **日志里的数偏高 1.5–1.7×**（app 282→~480、489→~600）：因为真实发布时后台还在解码播放、刷脏页、Defender 扫新文件。隔离基准更能反映各项的**相对**权重。

---

## 三、该做的三笔减法

### 3.1 发布不再物理拼接（收益最大：−489 ms concat + −282~590 ms 校验）

让块条目就是"分段集合"：manifest 里记段清单（段数/每段帧数/时间戳 + 每段自己的摘要），播放按段顺序开 decoder（**今天的分段播放路径已经这么做了**，`SynchronizedPlayback` 本来就按 `NeuralSegment` 切换），导出时才按需 concat。

- 省掉 `concatMs` 全部 489 ms（1 段 remux 才 117 ms，其中 98 ms 还是启动税）
- **校验从"验整块"改成"每段落地时验"**：`ProbeMedia` 的两个进程（packet 探测 136–190 + 末帧解码 146–172）挪到 finalize 线程，与 GPU 渲染重叠 → 关键路径上归零。发布时只需断言"各段帧数之和 == 预期"，这是纯算术。
- 摘要从"读整块再哈希"（31–90 ms）改成逐段已算好的摘要链
- 代价：`NeuralCacheEntry.payloadPath` 从"一个文件"变成"一组文件"；eviction / LRU / `Get a copy` / 缩略图这几个消费方要认集合。改动面在 `NeuralCache` + `NeuralSegmentIndex` + 导出路径，不动编码器与渲染线程。

### 3.2 冷命中不再全量 SHA-256

`NeuralCache.cpp:1614-1628`：`Lookup()` 对 payload 做完整哈希。进程内已有 memo（`PublishedPayloadDigest`，`NeuralCache.cpp:855-905`，按 path+size+lastWrite+changeTime+fileId），但**只管本进程刚发布的文件**；跨进程/跨启动全部重读。

- 窗口块条目 39 MB → 20–40 ms，可忽略
- **整片离线条目**（`Generate frames` 渲整部片）可能是 GB 级 → 1 s（NVMe）/ 20 s+（HDD）

**方案**：把 `PayloadStamp` 写进 `manifest.json`，命中时先比 stamp，一致则跳过哈希（与现有进程内 memo 同一套假设）。单文件改动。

### 3.3 分段粒度与刷盘时机（改一行，可 A/B）

- `kLiveSegmentSeconds = 2.0`（`main.cpp:10628`）→ **3 s（上限，见 §八 R3）**：一块 61 段 → 41 段，**实测 concat 526 → 405 ms（−120 ms）**；首段本来就是 0.5 s，不受影响。再往上（4/6 s）会吃掉 `kStartLead = 4 s` / `kResumeLead = 2 s` 的 attach 裕量。
- `FlushToDevice`（`NeuralCache.cpp:797`）对 live 派生条目改异步：条目的持久性由 rename 保证，flush 只影响掉电窗口。**实测 116 MB 的 fsync 是 40–52 ms**，直接移出关键路径。

---

## 四、如果仍要做内存化：三条路与各自的硬约束

### 方案 A：RAM disk 当 cache root（零改码）

- **硬约束**：发布/合并用 `MoveFileExW(..., MOVEFILE_WRITE_THROUGH)`（`NeuralCache.cpp:497,521`），**跨卷必失败（ERROR_NOT_SAME_DEVICE）**。只能整个 cache root 搬过去，不能只搬 live。
- 粒度错了：会把"应当持久的 entry"也搬进易失内存，重启即全部重渲；崩溃即全失。
- 定位：验证性手段。

### 方案 B：共享内存 ring + 自定义 `IMFByteStream`

四个硬约束：① helper 是独立进程（`NeuralWorkerMain.cpp` + 管道协议），共享必须 `CreateFileMapping` 命名段；② `NvencDirectMux::Writer` 用裸 `FILE*`（`NvencDirectMux.h:148-249`），抽 `ISink` 即可，但外部 ffmpeg 那条要 `-f matroska pipe:1` 且不可 seek；③ `VideoDecoder.cpp:1673` 只吃路径，要换 `MFCreateSourceReaderFromByteStream` + 自实现 `IMFByteStream`；④ `NeuralSegment.path` 就是身份，内存段要加变体 + 引用计数，decoder 持引用期间不能覆盖。

**收益上限 = 省掉块内分段的写盘（0.33–2.1 MB/s）**，`concat/probe/hash/flush` 一分不省。**最不划算的一条路。**

### 方案 C：GPU 纹理零拷贝（数量级收益，另一个项目）

live 播放本质不需要"视频流"——神经网络已经在 GPU 上产帧。改 GPU 纹理 ring 直接上屏，可同时省掉 readback（88–185 MB/s）、ffmpeg 管道、NVENC 编码、MF 解码。

代价：呈现抽象重做（`SynchronizedPlayback` 假定"源 + 神经 双解码流"）、与帧生成/超分/时间线耦合、显存预算（1080p fp16 = 8 MB/帧，30 帧 240 MB，尚可）。**真正的数量级收益在这里，独立立项。**

---

## 五、风险清单

- **pagefile-backed section 不等于不落盘**：内存压力下照样换出。
- **崩溃恢复语义**：今天崩溃后分段还在（`SweepLiveSessions` 按 pid 回收），内存化后直接消失。
- **与缓存键无关（安全面）**：live 段不参与 `NeuralCacheIdentity`，任何内存化都不动 key、不退役已有条目。
- **`MOVEFILE_WRITE_THROUGH` 跨卷失败**是方案 A 的静默死穴（表现为"没发布"）。
- **反向收益**：payload ring + 现有 128 MB 帧队列（`kQueuedFrameBytes`）同时压提交，可能触发分页 → 与初衷相反。
- **§3.1 的真实风险**：条目从"单文件"变"文件集合"后，`Evict`/`Clear`/`ReplaceRun`/`Get a copy`/缩略图 5 个消费方都要认集合；必须先把这 5 处列清单再动手。

---

## 六、建议路线

**Phase 0（半天，零风险）**：`GetProcessIoCounters` 分别统计 player / helper / ffmpeg 的读写字节，确认 §2.2 的 4× 放大在真实运行中的占比（日志只有时间，没有字节）。

**Phase 1（低风险，预期 −20% ≈ −190 ms/块）**
1. `kLiveSegmentSeconds` 2 → **3**（§3.3，一行，可 A/B，`concatMs` 实测 526 → 405）
2. live 派生条目异步 flush（§3.3，−40~52 ms）
3. manifest 写 `PayloadStamp`（**可选字段，禁止 bump schema**，见 §八 R5），冷命中跳过全量哈希（§3.2，−31~90 ms）

**Phase 2（中，预期 −70% ≈ −720~1080 ms/块，IO 5×→1×，呈现线程不再掉帧）**：条目改"分段集合"、校验改为抽样、发布不再物理拼接（§3.1 / §八 R4、R6、R7）。**这是整个议题里真正的收益点。**

**Phase 3（大）**：GPU 纹理零拷贝呈现，独立立项（§四-C）。

---

## 七、能优化多少（修订版，已并入 §八 自审的修正）

以 39 MB / 120 s 一块、`totalMs` 基准 **940–1650 ms**、退休段删除 **88–102 ms** 计：

| 阶段 | 今天 | Phase 1 之后 | Phase 2 之后 | 依据 |
|---|---|---|---|---|
| `concatMs` | 380–840 | 260–700（段长 3 s，41 段） | **0** | 实测 61→526 / 41→405 / 21→290 / 1→117 ms |
| `probeMs` | 430–590 | 430–590 | 250–350（只验末段 / 抽样） | 两个进程各 98 ms 启动税；**不是 0**（见 §八 R4） |
| `promoteMs` | 100–415 | 60–320 | 60–320 | 跳哈希 −31~90；异步 flush −40~52 |
| 退休段删除（**呈现线程**） | 88–102 | 88–102 | **0** | 不再有段可删 |
| **关键路径合计** | **1030–1750 ms** | **840–1520（−20%）** | **310–670（−70%）** | |
| 每块 IO 字节（720p / 1080p） | 195 MB / 1.3 GB | 不变 | **39 MB / 255 MB（1×）** | 不再物理拼接，也不再重复读 |

**边界**

- 发布发生在**块边界**，一块 120 s 视频按 1.8× 实时约 67 s 渲染完，所以 1 s 的发布 ≈ **整场吞吐的 1.5–2.5%**。Phase 2 把关键路径压到 0.3–0.7 s，整场吞吐提升 **约 1.5–2%**——不是数量级。
- 真正会咬人的是**方差**：日志里出现过 18 s 的空转（Defender 扫描 / 磁盘争用），那时块间停顿会掉到 20% 吞吐。Phase 2 消掉 concat/probe 两个外部进程后，这条长尾也一起消掉。
- **如果目标是"渲染更快"，这不是瓶颈**（瓶颈是神经 pass 本身，1.8× 实时）。如果目标是"块边界不卡、写放大 5×→1×、少 3/4 的磁盘写与文件数、呈现线程不再掉帧"，那 Phase 1+2 全中。

| 指标 | 今天 | Phase 1 | Phase 2 |
|---|---|---|---|
| 块边界关键路径 | 1030–1750 ms | 840–1520 | **310–670 ms** |
| 每块 IO 字节 | ~195 MB（720p）/ ~1.3 GB（1080p） | 不变 | **39 MB / 255 MB** |
| 呈现线程掉帧 | 每块 3–6 帧 | 同左 | **0** |
| 2 小时会话创建的文件数 | ~3600 | ~2400（段长 3 s） | ~2400 |
| 冷命中 → 首帧（整片条目） | 基准 | −1 s / −20 s | 同左 |
| 峰值提交内存 | ~400 MB | 不变 | 不变 |

**内存化这条路单独量化**：省掉 concat 的"读"= 11–32 ms/块 = 关键路径的 **1–3%**，且 `probe/hash/flush` 一分不动。**这就是它值多少钱。**

---

## 八、方案自审（Review，2026-10-18）

按"能不能落地、会不会翻车"逐条查了代码，发现 5 处需要改写、3 处需要补齐：

### R1（错）写放大是 **5×**，不是 4×
`probeMs` 里的 packet 探测是把 joined **整个再读一遍**。39 MB 内容 → concat 读 39 + 写 39 + probe 读 39 + 哈希读 39 + flush 39 = **195 MB**。已修正 §2.2 / §七。

### R2（漏）还有一笔不在 `publish timing` 里：退休段删除
`ReplaceRun` 退休的 61 个分段在**呈现线程**上删，实测 88–102 ms，播放倒退 103–113 ms、掉 3–6 帧（`main.cpp:10820-10822`）。每块都会发生。**Phase 2 直接消掉它——而且它落在最敏感的线程上，优先级应高于 concat。**

### R3（风险）Phase 1 第 1 项不能把段长拉到 6 s
渲染头那一段**未完成、不可播放**。段长 L 在 1.8× 实时下占 L/1.8 秒墙钟波动：
`kStartLead = 4 s`、`kResumeLead = 2 s`（`LiveSessionPolicy.h`）是 attach 的门槛。

| 段长 | 墙钟波动 | 判断 |
|---|---|---|
| 2 s | 1.1 s | 安全（现状） |
| 3 s | 1.7 s | **安全，建议上限** |
| 4 s | 2.2 s | 顶到 `kResumeLead` |
| 6 s | 3.3 s | **顶穿 `kStartLead`，attach 更易失败 → rebuffer 变多** |

所以 Phase 1 只能到 **3 s**，实测 61→41 段、concat 526→405 ms，**省 ~120 ms（不是我先前写的 200 ms，乐观了 40%）**。

### R4（错）Phase 2 的 probe 节省**不是白拿的**
"每段落地时验"= 61 × (packet 105 ms + 末帧 142 ms) ≈ **15 s CPU/块**，而一块只渲染 67 s——等于把 20% 的 CPU 从神经 pass 挪去起进程。必须改成**抽样**（首段 + 末段 + 每 N 段）或只验末段。修正后 Phase 2 的 probe 是 **250–350 ms 而非 0**，关键路径合计 **310–670 ms 而非 100–300 ms**。

### R5（事故级）`PayloadStamp` 千万别用 schema bump 实现
`IsReusableNeuralCacheManifest` 走 `schema == kSchema` 硬门（`NeuralCache.cpp:324,331`），**bump 会退役全部条目**——schema 5 刚退役过 schema 4。必须沿用既有"可选字段、序列化顺序排最后、缺省兼容"的写法（`NeuralCache.cpp:1239` 注释就是这个模式），并过 `PolicyTests`。

### R6（补）集合条目需要一条摘要链
`Lookup` 验的是 payload 的 `neuralDigest`。段集合要 `neuralDigest = SHA256(segDigest₁ ‖ segDigest₂ ‖ …)`，逐段摘要随段写入并在 manifest 里记段清单，否则认证无处安放。

### R7（补）发布断言要改写，这是 Phase 2 最容易翻车的地方
`PublishedProbeMatches` 要求 `probe.frameCount == frameCount` **精确相等**（`NeuralJobPolicy.h:119`，只有 duration 有容差）。集合条目要断言"**逐段帧数之和 == 预期**"，并且要处理 seam：`NeuralSegmentIndex::Append` 已经在做子帧重叠的算术（`llround(1e7/fps)` 造成的 40 tick 重叠），join 之后帧数可能与求和差 1。**动手前先写一个离线对拍：用现有 61 段 concat 前后的帧数/duration 比对。**

### R8（额外收益，之前漏了）
段集合之后，播放不再需要"最后一段 → joined 条目"的解码器重开，也不再有 `HoldsFile` 的例外分支，**块边界的切换点整个消失**。

### R9（结论不变）
天花板仍是整场吞吐 +1.5~2%。真正的止损点在神经 pass（1.8× 实时）。**若目标是渲染速度，不要在这里投入；** 若目标是块边界平滑、写放大、呈现线程掉帧，Phase 1+2 正好对应，且 Phase 2 的改动面比原估计更可控（编码器与渲染线程不动）。


---

## 九、Phase 1 实施记录与验证（2026-10-18）

### 9.1 已落地

| # | 改动 | 文件 |
|---|---|---|
| 1 | `kLiveSegmentSeconds` 2.0 → **3.0**（一块 61 段 → 41 段） | `src/main.cpp:10638` |
| 2 | manifest 新增**可选** `payloadStamp`（size/lastWrite/change/volume/fileId），promotion 记录、`Lookup` 命中即跳过全量 SHA-256；**schema 仍为 5**，字段缺省时序列化字节与旧版逐字节相同 | `src/NeuralCache.h`、`src/NeuralCache.cpp` |
| 3 | `NeuralSegmentIndex.h` 注释里的 2 s/段示例同步为 3 s | `src/NeuralSegmentIndex.h:343` |

**原 Phase 1 第 2 项（异步 flush）被明确放弃**，两个方案都行不通：

- **发布后再异步刷**：会让 manifest 里记录的 digest 描述"尚未落到设备的字节"。与第 2 项配合后，掉电可能留下"stamp 匹配 + 内容还没落盘"的条目，而它会被直接信任——用一条静默损坏路径换 40 ms，不划算。
- **让 flush 与 hash 重叠**：`Sha256File` 用 `FILE_SHARE_READ` 打开（`NeuralCache.cpp:964`），`FlushToDevice` 需要 `GENERIC_WRITE`，两者不能同时持有同一文件；放宽 hash 的 share 模式又会让它读到别人正在写的文件。
- 实测 fsync 只有 **40–52 ms / 116 MB**，收益远小于风险。

### 9.2 验证证据

| 项 | 方法 | 结果 |
|---|---|---|
| 编译（`/WX /W4`） | 5 个目标 | **全部 BUILD EXIT=0**：DLSSVideoPlayer / NeuralWorker / NeuralPrerenderTests / PolicyTests / NeuralWorkerTests |
| NeuralPrerenderTests（全量） | 含 3 个新用例 | **exit 0** |
| NeuralWorkerTests | 全量 | **exit 0** |
| PolicyTests `--tier=quick` | 1 个失败：`debug_view_popup_contains_all_existing_views_and_selection_test`（菜单项 4≠5） | **基线同样失败**（`git stash` 后重编重跑，一字不差）→ 与本次改动无关 |
| **跨进程跳过哈希**（128 MB 载荷） | 子进程 `publish` → **新进程** `lookup` | **lookup 5.26 ms vs hash 76.5 ms（14.5×）** |
| 对照：去掉 stamp | 手工删 manifest 的 `payloadStamp` 再 lookup | **81.2 ms**（重新哈希）→ 快的来源确实就是 stamp，不是缓存巧合 |
| 向后兼容 | 拿 `run-fg/cache` 里**真实的旧条目**（无 stamp）跑 lookup | 解析通过、按哈希认证（27.7 ms ≈ 26 ms 哈希）→ 旧缓存不会一夜作废 |
| 篡改仍被拦 | 同尺寸改写 + 还原 lastWrite | `LookupRender` 返回 nullopt（change time 变了）→ 既有用例保持绿 |
| 段长 3 s 的 join 收益 | 真实 39 MB / 120 s 条目切段后 concat | **61 段 526 ms → 41 段 405 ms（−121 ms）** |

### 9.3 仍需在本机 GUI 上确认（沙箱里跑不了 live session）

1. `Neural publish timing` 行里 `parts=` 应从 61 变成 **41**，`concatMs` 从 ~500 降到 **~405**。
2. **rebuffer 次数不因段变长而上升**——这是 R3 的唯一真实风险，看 live session 的停顿计数。
3. 冷启动打开已渲染条目时，日志出现 `authenticated by its recorded payload stamp`（不再有 76 ms 级哈希停顿）。

部署：`build-ninja/DLSSVideoPlayer.exe` 与 `build-ninja/Release/neural-runtime/NeuralWorker.exe` 已拷入 `run-fg/`（两者必须同步，否则 helper 与 player 协议不一致会渲染零帧）。

### 9.4 本机 GUI 实跑（22:24–22:26，720p / 24 fps 源）

| 判据 | 改前（今天 5 次发布） | 改后（本次 2 次发布） | 结论 |
|---|---|---|---|
| `parts` | 61 | **41 / 41** | ✅ |
| `concatMs` | 760 / 396 / 487 / 463 / 458，**中位 463** | **289 / 397，中位 343** | ✅ **−120 ms，与隔离基准 −121 吻合** |
| 同尺寸对照 | 51.7 MB → 396 ms；73.2 MB → 487 ms | 51.9 MB → **289 ms**；72.8 MB → **397 ms** | ✅ −107 / −90 ms |
| `totalMs` | 996–1653，中位 1199 | 895 / 1110，中位 **1003** | ✅ **−196 ms（−16%）**，与预估 −190 一致 |
| `probeMs` | 432–478 | 432 / 459 | 未变（如预期），现为最大单项 → Phase 2 目标 |
| 退休段数 | 61 | **41** | ✅ |
| 播放健康 | — | 66 条全 `dropped=0 (total 0)`，23.96–24.00 fps / 23.976 | ✅ **3 s 段未引入 rebuffer，R3 风险排除** |

`stamp` 日志行本次未出现是**预期**：本进程自己发布的条目由进程内 memo 命中，走不到 manifest 那条路径。新条目 manifest 已带 `payloadStamp`（`size` 72790391 / 51937968，与日志 `entryBytes` 逐一相等），冷进程证据见 §9.2。要在 app 里看到该行，需**重启播放器后打开已渲染视频**。

### 9.5 重启后的会话为什么没有 stamp 行（2026-10-18 22:28）

重启后的会话在**渲新区块**，不是命中缓存，日志说得很直白：

```
[22:28:15] Checking neural cache key=811045a5… range=[0,0)        → Neural cache miss
[22:28:22] Checking neural cache key=cdea030f… range=[11678333,1211678333) → miss or invalid entry; starting a new render
```

块起点跟着播放头走，所以每次会话的 key 都是新的（22:23 的 `[31281250,…)`、22:25 的 `[1231230000,…)`、22:26 的 `[2431178749,…)`、22:28 的 `[11678333,…)`）。**stamp 只在命中时被问到，而这次没有命中。**

于是改为**在新进程里直接对真实条目调 `LookupRender`**（与 app 同一条路径、同一个缓存根，只读）：

| 条目 | 载荷 | 全量哈希 | 冷进程 lookup | 判定 |
|---|---|---|---|---|
| `cdea030f…`（22:29 由正在运行的播放器发布） | 48 MB | 32.1 ms | **6.5 ms** | stamp 服务（未哈希） |
| `26c46e1d…`（22:25） | 69 MB | 66.2–74.3 ms | **11.1–12.7 ms** | stamp 服务 |
| `2cce296a…`（22:24） | 49 MB | 32.3–38.6 ms | **6.8–9.8 ms** | stamp 服务 |

三条真实条目全部走 stamp，包括播放器进程刚写下的那条。**app 日志里要看到那一行，只差一次真正的缓存命中**（整片渲染或同一位置的块重复打开）。
