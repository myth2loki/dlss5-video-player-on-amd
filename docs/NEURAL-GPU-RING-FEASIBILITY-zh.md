# C 方案可行性：神经帧 GPU 纹理直出

> 状态：**提案，未实现**。日期：2026-11-24。
> 前置阅读：`docs/PLAN-neural-payload-memory-cache-zh.md` §四方案 C、§五风险清单；
> `docs/CHROMECAST-CAST-PLAN-zh.md` §12.2 / §14；
> `docs/BENCHMARK-METRIC-VALIDITY-zh.md`（指标有效性）、`docs/MOTION-GUIDE-AB-zh.md`（A/B 门禁）。
>
> **一句话结论**：C 不是"把缓存搬进内存"，而是**重新定义神经帧的交付边界**。
> 能拿到数量级收益的只有 (b) 边播边渲，代价是缓存语义重写；
> (a) 保留预渲染只解决掉帧，磁盘流量一点不降。
> 无论走哪条，都**先做发布后台化**（低风险、可独立验收）。

---

## 1. 范围与收益边界

### 1.1 今天的链路

```
helper 进程产帧
  → GPU readback → vector → ffmpeg/NVENC → %TEMP% handoff → remux
  → 3 s 分段 mkv（101 段/块）落盘
  → 发布：concat + probe + promote(hash/flush/rename) → neural.mkv 持久条目
  → MF 从 mkv 解码（源流 + 神经流双流）
  → SynchronizedPlayback 配对 → present shader → 上屏
                                      └→ capture ring(4 slot) → BGRA pipe:0 → 投屏 HLS
```

关键实测与常量（当前代码/日志核过）：

| 项 | 值 | 出处 | 性质 |
|---|---|---|---|
| readback → vector → ffmpeg stdin | 88 MB/s (720p) / 185 MB/s (1080p) | `PLAN-...:12,42` | 实测，**内存带宽，不是磁盘** |
| payload 写盘（按视频时长摊） | 0.19–1.23 MB/s (720p，300 s 块)；2.1 MB/s (1080p CQ16) | `run-fg/DLSSVideoPlayer.log`；`measurements/cache-quality-20260923/REPORT.md:236` | 日志实测 + 规格推算 |
| 发布 total | 366–1065 ms（run-fg 最新 6 条），build-ninja 978–2031 ms | `Neural publish timing` 行，`main.cpp:3434` | 实测 |
| 帧队列 | 128 MiB，720p ≈34 帧 / 1.4 s；回收池 ≥4 帧 | `OfflineNeuralRenderer.cpp:391,451,525` | 代码常量 |
| 分段 / 窗口 | 3 s/段，300 s 窗口 = 101 段/块 | `main.cpp:13591`，`LiveSessionPolicy.h:161` | 代码常量 |
| 会话 lead | `kStartLead=4 s`、`kResumeLead=2 s`、低水位 180 s | `LiveSessionPolicy.h:26,27,181` | 代码常量 |
| 段边界代价 | 掉 5–14 帧 | `SynchronizedPlayback.cpp:600,675,710` 一带 | 实测 |
| 磁盘空闲保护底线 | 20 GB | `CacheEvictionPolicy.h:41` | 代码常量 |

### 1.2 C 省什么、不省什么（两条路线分开算）

| 环节 | (a) 保留预渲染 | (b) 边播边渲 |
|---|---|---|
| readback 88–185 MB/s | **不省**（仍要为落盘 payload 产帧） | 省 |
| ffmpeg / NVENC 编码 | **不省** | 省 |
| 分段 mkv 落盘 0.19–2.1 MB/s | **不省** | 省 |
| concat / probe / promote | **不省**（缓存发布照旧） | 省（无缓存条目可发布） |
| MF 解码（源+神经双流） | 省 | 省 |
| `SynchronizedPlayback` 配对与段边界掉帧 | 省 | 省 |
| capture ring 的 readback | **不省**（投屏必须过 CPU） | 不省 |
| 跨会话缓存命中 | 保留 | **失去** |
| seek / 崩溃恢复 | 保留（盘上帧还在） | **必须重渲**（冷启动 7.3 s + 4 s lead） |

**这条表是本文档的核心**：(a) 的"GPU 直出"只把呈现端换掉，payload 生产链原样；
(b) 才真正消除落盘，代价是缓存语义没了。不要用 (a) 的数字去论证 (b) 的收益。

### 1.3 与前面几轮落盘讨论的关系

- 发布后台化：**省时间**（每块约 −600 ms GPU 空转），不省字节。是 C 的前置条件。
- 缓存挪到 G 盘：**省磨损与重渲**，不改架构。与 C 正交，可立即做。
- 方案 B（共享内存 ring + 自定义 `IMFByteStream`）：动四层只省 0.19–1.23 MB/s，已判不划算，本文档不再展开。
  四条硬约束仍在：`NvencDirectMux.h:159/228/249` 裸 `FILE*`；`VideoDecoder.cpp:1673` 只吃路径；
  `NeuralSegmentIndex.h:16` 用 `path` 作身份；`NeuralCache.cpp:497,522` `MOVEFILE_WRITE_THROUGH` 跨卷必失败。

---

## 2. 显存账

### 2.1 单帧字节：先定格式，再谈预算

`PLAN-...:136` 的估算是 **1080p fp16 = 8 MB/帧，30 帧 = 240 MB**。
这个口径**必须复核**：1920×1080 × 8 B/px（RGBA fp16）= 16.6 MB/帧；
8.3 MB/帧对应双通道 fp16 或单通道 fp32。
**做预算前先确认 ring 里存的是哪种纹理**——差一倍会直接决定 ring 深度和是否撞上模型权重预算。

以下统一按文档口径 8 MB/帧做保守下限，括号内给出 16.6 MB/帧的上限。

### 2.2 短时 ring

| 深度 | 8 MB/帧 | 16.6 MB/帧 | 备注 |
|---|---|---|---|
| 30 帧（≈1.25 s @24 fps） | 240 MB | 498 MB | 与今天 128 MiB 队列同"秒级"量级，只是从系统内存搬到显存 |
| 24 帧（1 s） | 192 MB | 398 MB | (b) 的最小可行起点 |
| 2–3 帧（100 ms） | 16–24 MB | 33–50 MB | 只够抹平单帧抖动，扛不住 pass 超时 |

注意：今天 `kQueuedFrameBytes=128 MiB` 在**系统内存**，C 把它搬到显存后，
这部分不再是"白用的内存"，而是与模型权重、FSR4/FG 中间纹理抢同一块 VRAM。
`AMD-BACKEND-PITFALLS-zh.md` 里已有显存紧张时的 fallback 记录，ring 必须做**可降级深度**（见 §4-A7）。

### 2.3 180 s 预渲染缓冲放不进显存——这是两条路线的分岔点

今天播放头前方约 180 s 已渲染视频（`LiveSessionPolicy.h:181`），
块边界那 0.4–2 s 的 concat/probe 停顿全靠它吸收：

```
180 s × 24 fps × 8 MB/帧 ≈ 34.6 GB
（按 16.6 MB/帧则 ≈ 71.8 GB）
```

**显存装不下。** 所以只有两种设计，没有第三条：

- 想要这层缓冲 → 它必须留在盘上 → readback/NVENC/落盘省不掉 → 就是 (a)。
- 想要零落盘 → 只能把预缓冲砍到秒级 → 失去块边界的损耗吸收与跨会话复用 → 就是 (b)。

### 2.4 显存预算接口（实现前要接的）

- `D3D12Renderer` 的输出 rung / `UpscaleHeight=1080`：明确 ring 帧尺寸跟随哪个 rung（记忆里"SR 输出 = rung 盒子 fit 源，不是固定 2×"）。
- 与 FG 生成帧的显存共存：FG 在输出 rung 之后插值，ring 不能挤掉 FG 的中间纹理。
- "能改画面的设置必须同时进三处"：restart 条件、实时保留段落 key、渲染缓存 key。新增 ring 深度若影响画面，三处都要登记。

---

## 3. 两条路线对比

| 维度 | (a) 保留预渲染 + GPU 直出 | (b) 边播边渲 |
|---|---|---|
| 帧存哪 | 仍落盘（payload 链不动），呈现端另开 GPU ring | 只在 GPU ring，不落盘 |
| ring 深度 | 30 帧 / 240 MB 下限 | 24 帧起 / 192 MB 下限 |
| 落盘收益 | 0 | 全部（0.19–2.1 MB/s + 瞬时发布 IO） |
| 内存带宽收益 | 0（readback 仍为产 payload 发生） | 88–185 MB/s |
| 抗抖动 | 强：180 s 盘上缓冲还在 | 薄：神经 pass 文档按 1.8× 实时估，`LiveSessionPolicy.h:477` 只按 `ratio ≥ 0.98` 判 keepsUp，余量很窄 |
| seek / 重开 | 命中缓存，不重渲 | 必须重渲，冷启动 7.3 s + 4 s lead |
| 崩溃恢复 | 盘上分段还在，`SweepLiveSessions` 按 pid 回收 | 全丢 |
| 改动面 | 呈现端 + 同步层（中） | 呈现层 + 编码器 + 解码器 + 缓存 + 投屏（大） |
| 缓存条目 | 照旧发布、照旧消费 | 不再产生；5 个消费方要重定义 |
| 判断 | **低风险，解决掉帧，但不解决磁盘** | **数量级收益，但是重写项目** |

**推荐顺序**：
1. 发布后台化（独立验收，−600 ms/块，也是 (a) 吸收块边界的前提）。
2. (a) 作为第一阶段：验证 GPU ring 直出能消掉段边界掉帧，同时缓存语义不变。
3. 若动机仍是"消除落盘"，再单独立项 (b)，先做离线实验（§6.1）拿 dropped/rebuffer 数；**这个数没测出来之前，(b) 的收益与可行性都是纸面推算**。

---

## 4. 锚点清单（动手前逐项定案）

### A1 帧号 / 时间戳来源
- 现状：神经帧的 `frameNumber` / `timestamp100ns` 来自解码的 mkv；dump 帧号与解码帧号有全局整数偏移，每次用 `--calibrate` 量。
- C 之后：帧从未落盘，这个锚点消失。必须从神经 pass 自身序号重建，并与源文件音频（`Audio().Start(source)`，`main.cpp:10311`）对齐。
- 风险：错了不会报错，表现为投屏回放旧帧 + 音画不同步。
- 完成判据：电视端实拍音画同步 + 帧号单调性日志（新增，待定义）。

### A2 双流同步与段边界
- 现状：`SynchronizedPlayback` 做时间戳容差 1 帧、4 个回收 buffer、段边界 prefetch、OutOfSync 重同步；段边界掉 5–14 帧（`:600,675,710`）。
- C 之后：(b) 整条配对逻辑消失（最大简化，也是最大重写）；(a) 仍需保留源流音频配对，只去掉神经侧的段边界。
- 完成判据：`Segment boundary:` 日志在 (b) 中不再出现；(a) 中段边界掉帧数归零且无新的 rebuffer。

### A3 缓存条目的消费方
- 现状三个真实消费方：跨会话命中 `AnswerFromCache()`（`main.cpp:3081`）、live 源切换 `ReplaceRun()`（`:3459`）、`Get a copy` / 导出（`:13497`）。
- 另有 `docs/PLAN-...:147` 列的 5 处（含 `Evict` / `Clear` / 缩略图）必须先列清单。
- (a) 全部保留；(b) 全部要重定义或删除，且崩溃恢复语义一并改。
- 完成判据：现有消费方逐个跑通或明确标注"不再支持"。

### A4 段容器身份
- 现状：`NeuralSegment.path` 就是身份（`NeuralSegmentIndex.h:16`），`ReplaceRun` 用路径 retire 分段（`:209-248`）。
- (a) present ring 不参与索引，不动；(b) 无段可索引，`NeuralSegmentIndex` 要加内存变体 + 引用计数，或整体退役。

### A5 投屏 capture：enum 两档 + 内部三档，别混为一谈
- 当前 enum **只有两档**：`CaptureSource { NeuralOutput, PresentOutput }`（`D3D12Renderer.h:138`）。
- `PresentOutput` 内部按**三档纹理**选源（历史文档 `CHROMECAST-CAST-PLAN-zh.md:416-419` 为准，§12.2 已被 §14 覆盖）：
  NGX 在-service → descriptor 1；`UpscalePresentActive()` → `UpscaleSRV`；都没有 → descriptor 4（`m_dlssColor` 解码源）。
- `CaptureSlots=4`（`D3D12Renderer.h:373`）；投屏喂 `pipe:0` raw BGRA（`CastEncoder.cpp:150-151`），**capture readback 必须保留**。
- C 之后：三档要改成"从新 GPU ring 的哪一档取"；`drainBase` 硬闸依赖的帧号见 A1；几何跟随 renderer（`main.cpp:12703`），不能写死 1920×1080。
- 完成判据：见 §6.3 三条日志 + 电视端出画。

### A6 回退链与设置三处同步
- 硬不变量：FSR4 → FSR1 → CAS 不得破坏；AMD 新支路一律 `if (request.backend == NeuralBackend::Amd)` 早退（`main.cpp:2431,2965,3143,3274`）。
- ring 输出接到 present rung 的哪一级、`UpscaleHeight=1080` 还作不作数，按"三处同步"规矩登记。
- 完成判据：A/B 同一份测试代码跑回退链，`restart 条件 / 实时保留 key / 渲染缓存 key` 三处逐项核对。

### A7 GPU 忙时的背压
- 现状：队列满是 `space_.wait` 阻塞写线程（`OfflineNeuralRenderer.cpp:451-454`）。
- ring 满时：阻塞 GPU pass 提交会打乱调度；丢帧又回到 rebuffer。
- **策略必须先定再写码**：建议按显存余量动态降深度（§2.2），而不是固定 30 帧。

### A8 崩溃恢复 / 会话清理
- 现状：崩溃后分段还在，`SweepLiveSessions` 按 pid 回收；退休段删除在后台 `BackgroundFileReaper`（`main.cpp:13809`），已不在呈现线程。
- (b) 后这些语义全部要重写；`main.cpp:13794` 和 PLAN 文档里"掉 3–6 帧"是**过时注释**（2026-09-26 已修），别照抄。

### A9 编码器 sink（(b) 专属）
- 裸 `FILE*`：`NvencDirectMux.h:159/228/249`；解码器只吃路径：`VideoDecoder.cpp:1673`。
- (b) 要彻底去落盘，必须抽 `ISink` 并把外部 ffmpeg 改 `-f matroska pipe:1`（不可 seek）；这是四层改动里最重的一层。
- 与方案 B 的区别：B 改这四层只换 0.19–1.23 MB/s；B 的改造是 (b) 的子集，不单独做。

---

## 5. 前置条件：发布后台化

现状严格串行（`main.cpp:2848`）：

```cpp
if (ResolveSource() && ... && Render()) Publish();
lease_.reset();   // 发布不结束，下一个 job 起不来
in_.completions->RegisterAndPost(...)
```

`Publish()` 做 concat + probe + promote + hash + flush + rename，当前实测 366–1065 ms。
这段时间 helper 已渲完、runtime lease 还握着、**GPU 空闲**（文档注释记过一次 18 s 的 GPU 空转）。

改造目标：`Render()` 一返回就释放 lease、注册完成、启下一个 job；发布在后台线程做**同样的事、写同样的条目**。
收益是时间（约 −600 ms/块），不是字节。
它同时是 (a) 的前提：块边界停顿不能卡渲染线程，180 s 盘上缓冲才吸收得住。

**验收**：渲染线程不再出现 publish 分段耗时；同一份素材 A/B，GPU 空转窗口下降，缓存条目字节与哈希不变。

---

## 6. 验证门禁（写"必须看到哪一行"，不写"跑通即可"）

### 6.1 先做离线实验（(b) 立项门槛）
- 量"边播边渲 + 100 ms / 1 s ring 深度"在真实素材上的 dropped / rebuffer 数。
- 规则先写死再跑，不为凑结论调阈值。
- 判据：至少 2/3 镜头同号且每段差异 > 0.05 dB；整片均值不算数；repeat 零信息量，方差在镜头之间。
- **这个数没测出来之前，(b) 只有纸面收益。**

### 6.2 呈现 / 掉帧
- 必须看到 `Segment boundary:` 日志按预期消失或掉帧数归零（`SynchronizedPlayback.cpp:551`）。
- 必须看到 `Neural publish timing: ... totalMs=...` 的 `totalMs` 不再计入下一个 job 的启动间隔（后台化验收）。
  注意：**现有日志行没有线程标识**，要证明"发布真的在后台"必须新增一行带线程名/顺序号的日志（实现时定名），
  不能拿 `totalMs` 变小代替——那只能证明 concat 快了。
- 必须看到 `Live run ... now plays from its published entry; ... segment files retired`（`main.cpp:3460`）——(a) 必须保留；(b) 应消失。
- 检查无 GPU 重置、无 `ffmpeg: Error reinitializing filters!`（帧号跳变会让时域指标失效，只能检出补跑，不能放宽门禁）。

### 6.3 投屏（历史 20 次 404 的回归面）
- `Cast capture diag: captureOutput=... output=WxH upscaleActive=...`（`D3D12Renderer.cpp:5381`）——确认源档位选对、门控没把 capture 停掉。
- `Cast capture size from renderer: WxH`（`main.cpp:12703`）——确认几何跟随新 rung，不是写死尺寸。
- **电视端实际出画 + 音画同步**——这是帧号映射唯一的真实判据，日志看不出来。
- 自检报告把这三行列为必看项；缺任一行判失败。

### 6.4 回退链 / 设置
- FSR4 → FSR1 → CAS 三档逐档出图，与基线 A/B 一致；AMD 支路早退不被 ring 绕过。
- 同一份测试代码 A/B 比 assertion / failure 数，不拿旧基线数字当参照。
- 改画面的设置三处登记齐全：restart 条件、实时保留段落 key、渲染缓存 key。

### 6.5 落盘字节（(b) 才算）
- 用进程 IO 计数器（`GetProcessIoCounters` / `ReadTransferCount` / `WriteTransferCount`）做字节级验证：
  现状文档里的"写放大 4–5×"**从未做过字节级实测**。
- (b) 的验收目标：神经渲染期间进程写入字节接近 0（只剩日志），而不是"看起来没生成 mkv"。
- 写满 12 帧 ≠ 12 帧可用；`reset≠0` / 单一 `historyGeneration` 被打破 / 帧号跳变的数据不能用于判决。

---

## 7. 未决问题与数据缺口

| # | 缺口 | 影响 |
|---|---|---|
| 1 | ring 纹理格式（8 还是 16.6 MB/帧）未定 | 显存预算差一倍，可能在模型权重处撞墙 |
| 2 | (b) 的 dropped / rebuffer 数从未实测 | 收益与可行性目前是推算 |
| 3 | 真实写盘字节从未用 IO 计数器量过 | 写放大 4–5× 只是算术模型 |
| 4 | 神经 pass 实时倍率口径不一致（1.8× vs `ratio ≥ 0.98`） | 抗抖动余量无法定量评估 |
| 5 | 显存紧张时的 ring 降级策略未定 | 可能导致 fallback 链失效或整段重渲 |
| 6 | 投屏三档纹理在新 rung 下的映射未设计 | 历史上同类改动引出过 20 次 404 |

---

## 8. 明确不做的事

- 不为省磨损做方案 B（四层改动换 0.19–1.23 MB/s）。
- 不把 RAM disk 当缓存根：`MOVEFILE_WRITE_THROUGH` 跨卷必失败，且持久条目变易失。
- 不改回退链 FSR4 → FSR1 → CAS；AMD 支路保持早退。
- 不拿 `cuda.is_available()`、峰值 dB、短片 ECAPA cos、head-line PSNR 等已证伪的指标做判决。
- 不在没有 A/B 对照的情况下根据单次日志差异宣布回归（冷启动 `encoder back pressure` 会造成额外 `history reset`）。
