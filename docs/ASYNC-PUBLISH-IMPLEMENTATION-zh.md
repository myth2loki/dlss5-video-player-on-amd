# 发布后台化实现记录（main-async-publish）

> 分支：`main-async-publish`（从 `main-chromecast` 切出）。日期：2026-11-26。
> 对应设计：`docs/NEURAL-GPU-RING-FEASIBILITY-zh.md` §5「前置条件：发布后台化」。
> 状态：**代码已实现并编译通过；真机收益已证实（空转 −245 ms／块）；初版的 UAF 崩溃已修复，
> 两次独立真机复现共 7 个 job 全部通过门禁，可交付**。详见 `docs/ASYNC-PUBLISH-REALTIME-TEST-zh.md`。

## 1. 做了什么

对 **live job** 把「渲染完成」和「缓存发布」拆成两个交付：

| | 改动前 | 改动后 |
|---|---|---|
| worker 线程 | `Render() → Publish() → post WM_NEURAL_COMPLETE` | `Render() → 释放 lease → post WM_NEURAL_COMPLETE → 结束` |
| 下一 hole | 等 Publish 结束（251–293 ms GPU 空转，真机实测） | completion 一到就启动，Publish 在后台并发 |
| 缓存条目 | 在 completion 的 `neuralPath` 里带回 | 后台线程 post `WM_NEURAL_PUBLISHED` 单独带回 |
| 离线 job | — | **完全不变**（它的 completion 依赖 neuralPath 打开播放） |

## 2. 改动文件

- `src/main.cpp`（+273/−45，含 UAF 修复）
  - 新增 `WM_NEURAL_PUBLISHED`、`NeuralPublishCompletion`、`NeuralJobCompletion::publishPending`。
  - `NeuralJobRun` 改为 `enable_shared_from_this`，`Run()` 在 Render 后 handoff；
    NVIDIA INI 校验前移到 Render 末尾（lease 仍持有时）；Publish 用 `PublishStop()`。
  - `PlayerApp` 新增单线程 FIFO publish queue（共享指针 + 独立 stop_source）、
    `StartNeuralPublishWorker` / `StopNeuralPublishWorker`、`CompleteNeuralPublish` handler。
  - 退出路径（析构 / WM_CLOSE / WM_DESTROY）、`CancelNeuralJob`、`DrainNeuralMessages` 全部接线。
  - **UAF 修复**：`NeuralJobRun::in_` 由 `const NeuralJobInputs&` 改为**值成员**（见 §3.6）。
- `src/NeuralSegmentIndex.h`：新增 `PathsOfRun()`，一次加锁取快照。
- `src/NeuralJobPolicy.h`：`SegmentsOfRun` 改用快照（下一 job 并发 Append 时不再漏段）。

## 3. 关键取舍与原因

1. **不对离线 job 后台化**：离线 completion 的 `neuralPath` 是 `LoadCachedPlayback` 的输入，
   拆开就要重做播放装载，收益却只有一次请求。保持原样。
2. **completion 发副本**：后台 Publish 继续写 `completion_` 的 neuralPath / receipt / verdict，
   注册表拿走的是拷贝，避免后台写已移交所有权的对象。
3. **INI 校验前移**：下一 job 现在能在 Publish 期间写同一个 ReShade.ini；原 Publish 里的重读
   会误判「settings changed」。改为 Render 末尾、lease 仍持有、helper 刚返回时取值。
4. **generation 门控不用于 publish 消息**：publish 完成时下一 job 已 `Begin()` 推进了 generation，
   用 `Accept()` 会永远丢弃 Save 条目。改为只检查 session 还在 + coverage 非空，
   并用 `ExportableEntry` 把条目限定在「单 job 覆盖整片」时才作为 Save 目标。
5. **退出丢队列**：`Pop` 在 stop 时立即返回，不跑完排队 job；
   段已落盘、条目可重建，不让退出等 concat 长尾。
6. **`in_` 必须是值语义（本次修复的核心）**：render worker 的 lambda 按值捕获 `job`
   （`NeuralJobInputs`），线程函数一返回闭包就销毁；而 `NeuralJobRun` 被 `shared_ptr` 移交到
   publish 队列长期存活。初版只存 `const NeuralJobInputs&`，于是 `Publish()` 里所有 `in_.xxx`
   都成了悬空引用：表现为第二个 job 时 `0xC0000005`（读 `0x200006658`），或悬空的 `liveIndex`
   共享指针偶然读成 null 导致 retired 行静默缺失。改为值成员后，run 拥有自己的 inputs 快照，
   生命周期与 publish 线程一致。**不要改回引用**。

## 4. 构建与测试（本次实跑）

| 项 | 结果 |
|---|---|
| `DLSSVideoPlayer` Release 编译 | EXIT 0（VS 内置 cmake，`build-ci`） |
| `NeuralPrerenderTests` | 通过（200 pairs, buffer fills original=2 neural=2） |
| `PolicyTests --tier=quick` | 5 failures（toolbar 相关）— **A/B 与基线一致**（stash 后重编同结果） |
| `PlayerUiRegressionTests` | 66 cases / 3171 assertions / **22 failures** — **A/B 与基线一致** |
| A/B 方法 | `git stash` 三个改动文件 → 重编 → 跑 → `git stash pop` → 重编；两次数字逐字相同 |

结论：**没有引入可测回归**。真实时序由下述真机 A/B 覆盖。

## 5. 真机验证门禁（必须看到哪一行）

在真实 live 会话（开会话渲染）中，日志里必须同时出现：

1. `Neural render receipt:` — job N 的渲染结果（原有行）。
2. `Neural publish timing: parts=21 concatMs=... probeMs=... promoteMs=... totalMs=...`
   — **可在下一个 job 的 `Neural render receipt:` 之后才出现**，这正是后台化的证据。
3. `Live run <N> now plays from its published entry; <M> segment files retired. entryBytes=...`
   — 每个**真正完成 publish 的 run** 都要有这一行（被下一 job 取消的 run 除外）。
4. `Active neural session job <G> published its cache entry in the background; save=... entry=...`
   — 新的 publish 完成行。整片单 job 时 `save=1`；窗口会话 `save=0` 是正确行为。
5. 若没有第 3 行而有 `published no cache entry: ...`，说明发布失败；
   此时**播放仍应在分段上继续**，不应黑屏或回到原片。

必须验证的行为：

- 块边界不再有 GPU 空转：`receipt → 下一 job` 的间隔接近纯渲染启动时间（实测 49–56 ms），
  不再包含 publish 的 218–280 ms。这是本次优化的**唯一收益指标**。
- **稳定性（UAF 回归门禁）**：连续 ≥3 个 job 无新 minidump；retired 行按 job 出现。
- 播放连续性：`Segment boundary:` 日志的掉帧数不比改动前差；音画同步不变。
- 一键导出：整片单 job 渲染后 `Save converted video` 仍可用（对应日志 `save=1`）。
- 退出：在 Publish 进行中关窗口，进程应正常退出、无崩溃、无残留 helper。

## 6. 已知限制 / 未决风险

- **收益已实测**：293 ms → 48–56 ms／块（−83%），见真机报告。300 s 生产窗口的收益按 publish
  时长推算更大（旧日志 publish totalMs 978–2031 ms），但本次用 60 s 窗口测，未在 300 s 上量。
- **publish 失败降级**：发布失败不再让 completion 的 `result.ok=false`（因为它已经发出）。
  行为上是「分段照常播放、没有 Save 条目、日志说明原因」，这是有意设计，
  但需要在真机上确认用户不会因此丢失已渲染的画面。
- **`m_neuralPath` 时序**：多 job 窗口会话里，job N 的 publish 消息可能被 job N+1 的
  completion 抢先；`ExportableEntry` 会把旧条目排除在 Save 之外（保守，不误导出错片）。
- **reaper（已澄清）**：`ReplaceRun` 留在 publish 线程是安全的——`NeuralSegmentIndex` 自带
  mutex，`PathsOfRun` 是一次加锁快照，reaper 仍在 UI 线程串行删除。初版崩溃**不是**竞态，
  而是 §3.6 的 UAF；此前「把 ReplaceRun 移回 UI 线程」的修复方向是误判，未采纳。
- **helper 独占**：Publish 不碰 `residentHelper`/`preflightLatch`，下一 job 可以安全用它们（已核代码）。

## 7. 未提交

按用户长期规则，改动只留在工作区，**未 git add / commit / push**。
