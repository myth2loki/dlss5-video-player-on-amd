# 发布后台化真机测试报告（main-async-publish）

> 日期：2026-11-26。GPU：AMD Radeon RX 9070 XT（RDNA4，驱动 32.0.32015.7029）。
> 后端：`amd-lmxxf` / mochizuki，helper `NeuralWorker.exe`（11-22，与 release 同版）。
> 素材：`run-fg/realclip-5min.mkv`（1280x720 @ 23.976，301 s）。
> 窗口：60 s / 21 段每 job（ini 手改，以便一次调用内跑出多个 job）。
> 驱动脚本：`run-fg/async-publish-verify/drive.py`（`ctypes.PostMessageW(WM_COMMAND, IDM_NEURAL_RENDERING=300)`，
> 无焦点、不依赖 SendInput——沙箱安全策略会拦注入按键和 PowerShell 的 Add-Type）。
> 测试目录与生产隔离：新 exe 放 `run-fg/async-publish-verify/`，基线旧 exe 放
> `run-fg/async-publish-verify-baseline/`，两者配齐同样的 AMD dll / helper / ffmpeg。

## 结论先行

1. **收益证实**：块边界 GPU 空转从 **293 ms → 48–56 ms**（−240～−245 ms／块，−83%），
   下一 job 在本 job 的 publish 完成前 ~200 ms 就已启动。这是本次优化的唯一判据，达标。
2. **初版有一个真实崩溃，根因已定位并修复**：`NeuralJobRun` 持有 `NeuralJobInputs&`，
   render worker 的 lambda 销毁后引用悬空（UAF），不是此前推测的 `ReplaceRun` 竞态。
   改为值成员后，**两次独立复现（200 s / 180 s）共 7 个 job，0 新 minidump**。
3. 播放侧正常：receipt 全部 `failure=none frames=1439/1439 verified=1439`，
   播放 `live=1 rendering=1`、`dropped=0`，缓存条目正常落盘（neural.mkv + manifest + receipt）。

## 一、收益 A/B（唯一判据：receipt → 下一 job 启动的间隔）

| Arm | 事件时序（同一次会话，media/窗口/GPU 全同） | receipt→下一 job |
|---|---|---|
| **基线**（串行，旧 exe 11-22） | receipt 14:19:46.131 → publish timing 14:19:46.382（+251 ms）→ 下一 job `rendering [...]` 14:19:46.424 | **293 ms** |
| 基线（第 2 块） | receipt 14:20:34.965 → publish 14:20:35.189（+224 ms）→ 下一 job 14:20:35.231 | **266 ms** |
| 初版新代码（后台，崩溃前） | receipt 14:25:14.223 → 下一 job 14:25:14.271（+48 ms）→ publish timing 14:25:14.473（+250 ms） | **48 ms** |
| 初版新代码（run A） | receipt 14:11:55.594 → 下一 job 14:11:55.645（+51 ms）→ publish 14:11:55.874（+280 ms） | **51 ms** |
| **修复后复现 1**（200 s） | receipt 14:33:30.514 → 下一 job 14:33:30.570（+56 ms）→ publish 14:33:30.776（+270 ms） | **56 ms** |
| **修复后复现 2**（180 s，块 1） | receipt 14:36:46.905 → 下一 job 14:36:46.954（+49 ms）→ publish 14:36:47.159（+254 ms） | **49 ms** |
| **修复后复现 2**（180 s，块 2） | receipt 14:37:38.320 → 下一 job 14:37:38.376（+56 ms）→ publish 14:37:38.546（+226 ms） | **56 ms** |

解读：
- 基线遵守 `Publish() → completion → 下一 job`，所以下一 job 必须等 publish 结束（251/224 ms）。
- 新代码遵守 `Render() → completion → 下一 job`，publish 并发在后台；下一 job 比 publish 完成早约 200 ms。
- publish 自身耗时两 arm 相同（218–280 ms），**证明 concat/probe/promote 没有变快**——省的是空转，不是发布。
- 修复 UAF 后收益数字不变（49–56 ms），说明值成员修复没有影响时序路径。
- 60 s 窗口 publish 更短（21 段 vs 300 s 的 101 段），300 s 生产窗口的空转收益会更大
  （旧日志 publish totalMs 978–2031 ms），但本次未在 300 s 窗口量到。

## 二、初版崩溃：现象与真正的根因

| 项 | 事实 |
|---|---|
| 现象 | `Unhandled exception 0xC0000005 at 0x7ff6787442e0`，异常地址 `0x200006658`（野指针读）；minidump 已写 |
| 复现 | 初版新代码 2/2 出现在**第二个 job** 完成时；只跑 1 个 job 的 run3 无崩溃 |
| 基线 | 同配置 3 个 job 全部完成，无崩溃、无 dump |
| 崩溃点日志序列 | `Neural render receipt` → `Active neural session rendered ... entry=(none)` → `window extending` → `rendering [下一块]` → crash |
| 关键差异 | 基线每个 job 都打 `Live run N now plays from its published entry; 21 segment files retired`；初版新代码常缺失这行 |

### 根因（已定位，非推测）

`NeuralJobRun::in_` 是 `const NeuralJobInputs&`，指向 render worker lambda 的按值捕获成员 `job`。
live job 在 `Run()` 末尾 `handoff(shared_from_this())` 后返回，线程函数结束 → lambda 闭包连同
`job` 销毁；但 `run` 已由 `shared_ptr` 移交给 publish 队列，继续在后台线程执行 `Publish()`。
于是 `Publish()` 中每一个 `in_.xxx` 都是对已释放对象的访问：

- `in_.liveIndex` 读到垃圾/偶然为 null → `ReplaceRun` 不执行 → retired 行缺失；
- 其它成员（如 `liveRunId`、`cacheRoot`、registry 指针）读到无效值 → 野指针读 `0x200006658` → `0xC0000005`。

基线下 `Publish()` 在 `Run()` 内同步调用，`job` 仍存活，所以完全没有这个问题。

**为什么此前误判为 `ReplaceRun` 竞态**：retired 行缺失看起来像「publish 线程在 ReplaceRun 处崩了」，
而当时又知道下一 job 会并发 Append 到同一 index，两条线索指向竞态。但 `NeuralSegmentIndex`
每个方法都加锁，且 `PathsOfRun` 已是单次加锁快照；真正跨线程失效的不是 index，而是持有 inputs
的那个引用。按误判方向「把 ReplaceRun 移回 UI 线程」并不能修复——publish 一开始访问 `in_` 就已经悬空。

### 修复

`src/main.cpp`：`NeuralJobRun::in_` 由 `const NeuralJobInputs& in_` 改为 `NeuralJobInputs in_`
（构造时拷贝一份快照），并在构造函数注释中写明原因。`cache_` 改从 `in_.cacheRoot` 初始化，
避免成员初始化顺序陷阱。lambda 与 handoff 结构不变。

修复后门禁：**连续 ≥3 job 无 dump、retired 行按 job 出现**（见 §三），全部通过。

## 三、门禁核对

| 门禁 | 修复后结果 |
|---|---|
| `Neural render receipt:` | ✅ 每 job 出现，`failure=none`，帧数完整 |
| `Neural publish timing: ... totalMs=` | ✅ 出现，218–270 ms（21 段） |
| `Live run N now plays from its published entry; ... retired` | ✅ 复现 1：run 1 / run 3；复现 2：run 1 / run 2 / run 3 各一行 |
| `... published its cache entry in the background; save=0` | ✅ 出现；窗口会话 `save=0` 是预期 |
| 发布失败降级 `published no cache entry` | 本次未触发，未验证 |
| 块边界不再 GPU 空转 | ✅ 293→49–56 ms |
| **连续 ≥3 job 无新 minidump** | ✅ 两次独立复现共 7 job，dump 计数保持 2（均为修复前旧文件） |
| 播放连续性 dropped=0 / 音画 | ✅ `Playback health` dropped=0、live=1 |
| 运行期无崩溃 | ✅ 修复前 2/2 崩，修复后 2/2 完整跑完 |
| `Get a copy` / Save | 窗口会话 save=0，未测导出按钮 |

复现 1 中 run 2 在 206 ms 内被 window 逻辑取消（范围由 `[64.7,124.7)` 扩到 `[64.7,184.7)`），
所以没有独立 retired 行——这是正常的 job 取代，不是 publish 失败；复现 2 三个 job 全部完成，
retired 行一一对应。

## 四、产物与复现

- 日志归档：`run-fg/async-publish-verify/{async-run-final,crash-run-33228,async-run3-nocrash,fixed-run1,fixed-run2}.log`
- 基线日志：`run-fg/async-publish-verify-baseline/baseline-run.log`
- 崩溃转储（**修复前**旧文件）：`run-fg/async-publish-verify/DLSSVideoPlayer-crash-20261126-141{409,616}-*.dmp`
- 缓存条目：`run-fg/async-publish-verify/cache/v1/renders/<key>/{neural.mkv,manifest.json,receipt.json}`
- 复现命令：`python run-fg/async-publish-verify/drive.py 180`（基线目录把脚本里 D 换成 baseline 目录）
- 环境已隔离，未改动生产 exe/ini；驱动脚本与测试目录均为本次新增，未提交。

## 五、未决

- 崩溃已修复，且不再需要符号化（根因由所有权分析确定，值成员修复后两次复现均未再出现）。
- 300 s 生产窗口的收益未在真机量到（本次用 60 s 窗口）；按 publish 时长推算空转收益更大，
  但需在真实配置下复测。
- 未验证：发布失败降级路径、Save 导出、AMD 之外的后端。
