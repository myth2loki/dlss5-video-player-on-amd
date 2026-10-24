# DLSS 5 Video Player AMD 实时字幕与统一 GPU 调度技术方案

## 文档状态

- 状态：研究与设计阶段，尚未实现
- 日期：2026-10-24
- 目标分支：`main-subtitle`
- 记录提交：`88f45295e932f5f5f56711b43499d5faa312231a`
- 目标开发机器：Windows 11 x64，AMD Radeon RX 9070 XT，RDNA 4，`gfx1200` 或 `gfx1201`
- 文档语言：中文
- 文档位置：`docs/REALTIME-SUBTITLE-PLAN-zh.md`
- 本文件用途：作为后续在 Windows 11 + RX 9070 XT 环境继续开发的总设计记录

本文档只记录方案和决策，不表示已有相关功能实现。

## 1. 结论摘要

`Transub-Player` 的实时字幕方案可以作为目标工程的设计参考，但不能直接移植代码。

原因是：

- `Transub-Player` 使用 C#、.NET、WPF 和 `Whisper.net`。
- 目标工程使用 C++20、MSVC、D3D12，并采用静态 CRT `/MT`。
- 目标工程已有 FFmpeg 子进程、音频播放、字幕 BGRA 叠层和 D3D12 合成基础。
- 目标工程当前没有 ASR、Whisper、ONNX、DirectML 或本地翻译链路。

推荐路线：

```text
阶段 0：统一 GPU 资源协调器，只观测，不改变现有行为
阶段 1：CPU ASR + large-v3-turbo，本地文件，先做原文实时字幕
阶段 2：GPU ASR + GPU 字幕翻译，只与神经渲染同时开启
阶段 3：在上述组合上增加超分
阶段 4：插帧与全组合只作为实验模式
阶段 5：根据实测决定是否支持 1080p60、4K、网络媒体和第二 GPU
```

核心产品原则：

```text
神经渲染和正常播放优先
字幕和翻译可以降级
不能因为字幕任务让视频进入无必要的缓冲暂停
```

## 2. 目标与非目标

### 2.1 目标

- 为本地视频提供准实时原文字幕。
- 可选提供字幕翻译。
- 优先支持 Windows 11 和 AMD RX 9070 XT。
- 不允许字幕任务破坏正常播放、神经渲染或音频时钟。
- 通过统一资源协调器控制 GPU、显存和任务优先级。
- 所有模型、运行时和缓存都可验证、可回退、可清理。

### 2.2 非目标

- 第一版不支持逐字级实时转写。
- 第一版不承诺 1080p60、1440p 或 4K 全功能稳定。
- 第一版不支持直播流和网络流字幕。
- 第一版不把字幕烘焙进导出视频。
- 第一版不允许字幕任务抢占神经渲染的播放资源。

### 2.3 “实时”的定义

本方案中的“实时”是准实时、分段增量字幕：

- 首句字幕可能延迟数秒到十余秒。
- 字幕前沿应持续追近播放头。
- 允许字幕略晚于画面。
- 不允许字幕积压无限增长。
- 不允许为了等待字幕而卡住视频播放。

## 3. Transub-Player 的实现分析

### 3.1 主链路

`Transub-Player` 的实际链路为：

```text
播放器打开本地媒体
  -> 按窗口启动无界面 mpv 提取音频
  -> 内嵌 Whisper.net 逐窗口识别
  -> 增量写入原文 SRT
  -> PreviewSubtitleSync 监听并合并字幕
  -> 可选本地 llama-server 翻译
  -> 生成 display.srt
  -> mpv 热重载字幕轨道
```

### 3.2 ASR 分片策略

关键参数：

```text
ChunkSec = 90 秒
ChunkOverlapSec = 2 秒
ChunkStep = 88 秒
```

处理窗口约等于：

```text
0-90 秒
88-178 秒
176-266 秒
...
```

每个后置窗口丢弃与前一窗口重叠的前 2 秒，以减少重复字幕。

音频不是从当前播放器输出抓取，而是另外启动 mpv：

```text
16 kHz
单声道
s16 PCM
WAV
```

Whisper 返回 segment 后，转成绝对媒体时间并追加到字幕列表。源 SRT 在识别过程中最多约每 4 秒刷新一次，窗口结束后再次强制刷新。

### 3.3 字幕热更新

`Transub-Player` 使用以下机制：

- 源 SRT 作为识别结果中间文件。
- 文件监听加约 450 ms 防抖。
- 合并字幕时按“开始时间 + 原文”保留已有译文。
- 当前显示模式决定生成原文、译文或双语 `display.srt`。
- mpv 使用 `sub-add` 或字幕重载刷新轨道。
- 字幕内容未变化时刻意延迟重载，减少闪烁。

### 3.4 翻译链路

翻译使用本机 `llama-server` 加载专用 GGUF，通过 OpenAI 兼容接口批量处理。

- 默认每批约 8 条字幕。
- 优先翻译播放头附近内容。
- 翻译失败时保留原文。
- ASR 使用 GPU 时，翻译模型可能降级到 CPU。
- ASR 空闲后再尝试把翻译模型升级回 GPU。

### 3.5 缓存与预生成

- ASR、译文和双语字幕写入独立预览目录。
- ASR 完成后写入完成标记。
- 再次打开同一媒体时可直接加载缓存。
- 播放列表可在播放器空闲时预生成后续条目的字幕。

### 3.6 方案局限

- 不是逐 token 或逐字流式识别。
- 90 秒窗口适合吞吐，不适合最低首句延迟。
- 依赖文件写入、监听和重载。
- 网络流和桌面采集不走实时字幕。
- ASR 顺序扫描整部文件，跳播到未生成区域时需要等待或显式从播放头重启。

## 4. 目标工程当前事实

### 4.1 平台与构建

- Windows x64。
- C++20。
- MSVC。
- D3D12。
- 静态 CRT `/MT`。
- 已有严格的运行时文件锁定和校验体系。
- FFmpeg 作为独立子进程使用。

关键文件：

- `CMakeLists.txt`
- `docs/WIN11-DEVELOPMENT-SETUP-zh.md`
- `docs/ARCHITECTURE.md`
- `docs/TROUBLESHOOTING.md`

### 4.2 已有媒体链路

- `AudioPlayer` 通过 FFmpeg 解码音频并交给 WASAPI。
- 支持多音轨选择。
- 支持 AC-3、E-AC-3、DTS passthrough。
- 播放时钟以音频为主。
- `Position()`、暂停、seek、切轨和字幕延迟已有统一入口。

重要事实：ASR 必须使用当前选中的音轨，不能固定提取 `0:a:0`。

### 4.3 已有字幕链路

- `SubtitleOverlay` 通过 FFprobe 探测字幕。
- 文本字幕通过 FFmpeg libass 绘制为透明 BGRA。
- 图片字幕直接转换并缩放到画布。
- `D3D12Renderer::SetSubtitleOverlay` 负责 GPU 上传和最后合成。
- 字幕在神经渲染和超分之后绘制，不进入模型输入或缓存。

当前字幕链路主要面向固定字幕源：

- 整个字幕文件或字幕流一次性读取并渲染。
- 每次更新会重新开始渲染会话。
- 不支持低成本的持续增长字幕文件。

因此不能把“持续覆盖 SRT”直接作为目标工程的实时字幕主通道。

### 4.4 当前 GPU 工作负载

现有主要 GPU 工作：

- Lmxxf 神经渲染。
- FSR 超分。
- FidelityFX 插帧。

这些模块不是统一调度的：

- Lmxxf 自己创建 D3D12 设备、队列、命令列表和 fence。
- FSR 初始化时创建自己的队列。
- FidelityFX 插帧会创建独立 D3D12 设备和队列。
- DXGI 显存采样目前主要是当前进程或 helper 的局部视角。

这意味着后续加入 GPU ASR 和 GPU 翻译时，不能假设现成存在全局资源仲裁。

### 4.5 当前字幕缺陷

如果实时渲染无法覆盖播放头，播放器会进入 live buffering，并暂停音频。

因此后续最大的风险不是“字幕停掉”，而是字幕任务把神经渲染拖入缓冲状态，导致画面和声音一起暂停。

## 5. 目标工程可复用与不可复用部分

### 5.1 可直接复用

- FFmpeg 工具定位和安全启动模式。
- Job Object 子进程生命周期管理。
- stderr 排空、日志和崩溃转储。
- 音频轨道选择结果。
- 播放时间和字幕时间基准。
- `SubtitleOverlay` 的 BGRA 输出接口。
- `D3D12Renderer::SetSubtitleOverlay` 的上传和合成。
- 现有 runtime lock、打包和第三方声明模式。

### 5.2 必须重新实现

- 原生 C++ ASR 推理。
- 音频切片和 PCM 队列。
- Cue 列表和增量字幕状态机。
- 实时字幕 BGRA 渲染。
- GPU 翻译运行时。
- 模型下载、校验和生命周期。
- 统一 GPU 资源协调器。
- ASR、翻译和字幕的降级策略。

### 5.3 不能直接复用

- C# 或 WPF 代码。
- `Whisper.net` API。
- mpv 字幕热重载。
- `PreviewSubtitleSync` 的文件监听机制。
- Transub 的 Python 或 .NET 运行时。

## 6. 推理后端选择

### 6.1 CPU ASR

候选：

- `whisper.cpp` CPU 后端。

优点：

- 与 GPU 渲染不直接竞争显存。
- 驱动风险最低。
- 开发和打包相对简单。
- 适合第一阶段验证字幕体验。

缺点：

- `large-v3-turbo` 对 CPU 要求较高。
- 会与 FFmpeg、CPU 运动估计和 UI 线程争抢 CPU。
- 实时率不稳定时字幕积压。

建议：

- 使用 `large-v3-turbo`，不优先使用完整 `large-v3`。
- 优先评估量化权重。
- 限制线程数，保留 2 到 4 个逻辑核心给播放器。
- ASR helper 使用低于正常优先级。

### 6.2 GPU ASR

候选：

- `whisper.cpp` Vulkan。
- ONNX Runtime + DirectML。

建议：

- 如果 ASR 和翻译都使用 Vulkan，应共享同一个 Vulkan 设备和调度器。
- 如果 ASR 和翻译都使用 DirectML，应共享 D3D12 设备体系。
- 不建议一个使用 Vulkan、一个使用 DirectML，再叠加 Lmxxf 的 D3D12/HIP 路径。
- GPU 推理必须分片、可取消，并在 chunk 边界释放资源。

### 6.3 GPU 翻译

候选：

- llama.cpp Vulkan。
- ONNX Runtime DirectML。
- 专用小翻译模型。

建议：

- 第一版不要使用 7B 或 8B 通用聊天模型。
- 优先 0.5B 到 3B 的专用翻译模型。
- 使用 4 到 8 条字幕的小批量。
- 关闭思维链和长解释输出。
- 只翻译播放头附近的字幕。

## 7. 阶段路线与能力边界

### 7.1 第一版：CPU ASR，大模型，原文优先

目标：

- 本地文件。
- CPU `large-v3-turbo`。
- 实时或准实时原文字幕。
- 可选小批量译文。
- 暂不支持网络流。

建议参数：

- 第一段 8 到 12 秒，尽快产生首句。
- 后续窗口 20 到 30 秒。
- 窗口重叠 2 秒。
- 先产生原文，再异步翻译。
- 字幕延迟超过阈值时先暂停翻译。

CPU 实时率判断：

```text
RTF = 处理墙钟时间 / 音频时长
```

建议目标：

```text
RTF <= 0.7：有较好余量
0.7 < RTF < 1.0：可用但脆弱
RTF >= 1.0：字幕 backlog 会持续增长
```

如果每 30 秒音频需要超过 28 秒处理，则长期无法跟上播放。

### 7.2 第二版：只开启神经渲染 + GPU ASR + GPU 翻译

这是推荐的第二版主目标。

不开启：

- 超分。
- 插帧。

推荐配置：

- 单 pass 神经渲染。
- GPU `large-v3-turbo` 量化版本。
- GPU 专用小翻译模型。
- 16 GB 显存优先。
- 720p24 到 30 正式支持。
- 1080p24 有条件支持。
- 1080p30 实验性支持。

建议的验收指标：

- 神经渲染不能持续低于 1.0 倍实时。
- 字幕前沿不能无限落后。
- 翻译 backlog 有上限。
- 显存峰值不能持续接近适配器预算。
- 字幕任务不能让播放器进入 live buffering。

### 7.3 第二版进阶：增加超分

加入超分后形成：

```text
Lmxxf 神经渲染
FSR 超分
GPU Whisper ASR
GPU 字幕翻译
```

推荐分档：

| 场景 | 建议 |
| --- | --- |
| 720p24 到 1080p 输出 | 正式支持目标 |
| 720p24 到 1440p 输出 | 推荐支持目标 |
| 720p30 到 1440p 输出 | 有条件支持 |
| 1080p24 到 1440p 输出 | 实验性支持 |
| 1080p30 到 1440p 输出 | 高风险 |
| 1080p30 到 4K 输出 | 不建议 |
| 1080p60 或 4K 输出 | 暂不支持 |

超分成本与输出像素相关，而不是只与源分辨率相关。

### 7.4 全组合：再加插帧

组合为：

```text
Lmxxf 神经渲染
FSR 超分
FidelityFX 插帧
GPU Whisper ASR
GPU 字幕翻译
```

判断：

- 720p24 到 30：可做实验模式。
- 1080p24 到 30：不承诺稳定。
- 1080p60、1440p、4K：不建议单卡同时运行。
- 第二 GPU 或核显承担 ASR、翻译时可行性最高。

## 8. 当前已测性能基线

以下数据来自现有工程文档，只代表当前已测组合，不包含 ASR 或翻译。

| 项目 | 1280x720 | 1920x1080 |
| --- | ---: | ---: |
| CPU 运动估计 | 0.8 ms/帧 | 1.4 ms/帧 |
| Lmxxf submit p50 | 10.3 ms/帧 | 21.4 ms/帧 |
| 网络本身 GPU 时间 | 4.6 ms/帧 | 8.5 ms/帧 |
| 24 fps 单帧预算 | 41.7 ms | 41.7 ms |
| 神经渲染相对实时能力 | 约 4.0 倍 | 约 1.9 倍 |

实时窗口实测：

- 1280x720、23.976 fps：约 2.62 到 2.71 倍实时。
- 1920x966、25 fps：约 1.51 到 1.52 倍实时。
- 冷启动约 5.6 到 10.0 秒。
- 1080p 神经渲染 helper 的进程局部显存约为 1.3 到 2.5 GiB。
- 适配器预算约为 15.4 GiB。

尚未测得：

- FSR 与 ASR 组合。
- 插帧与 ASR 组合。
- GPU ASR 的显存和时间。
- GPU 翻译的显存和时间。
- 三个 AI 模型同时加载时的峰值。
- 4K 输出全组合。

因此任何“1080p 加超分加 GPU 字幕加翻译稳定”的结论都必须先经过真机测试。

## 9. 推荐目标架构

```text
当前音轨
  -> FFmpeg 音频切片
  -> ASR Worker
  -> CueStore
  -> LiveSubtitleController
  -> DirectWrite/GDI 生成 BGRA
  -> D3D12Renderer::SetSubtitleOverlay

CueStore
  -> Translation Worker
  -> CueStore 更新译文 revision

统一 GPU 资源协调器
  -> 观测
  -> 准入
  -> 租约
  -> 降级
```

### 9.1 音频切片

独立 FFmpeg 子进程建议参数：

```text
-map 0:a:<selected-track>
-ac 1
-ar 16000
-sample_fmt s16
```

注意：

- 必须跟随当前播放器选择的音轨。
- passthrough 输出不是 ASR 可用 PCM。
- seek 后必须取消旧切片。
- 本地文件使用现有 local protocol whitelist。
- 网络媒体第一阶段不实现。

### 9.2 CueStore

建议字段：

```text
id
start
end
sourceText
translatedText
revision
asrState
translationState
sourceLanguage
targetLanguage
```

所有 ASR 和翻译结果都带 generation，旧 generation 的结果必须丢弃。

### 9.3 实时字幕渲染

不建议把持续增长的 SRT 作为实时主通道。

建议：

- 在内存中维护当前有效字幕。
- 每次 revision 只重新生成受影响区域。
- 使用 DirectWrite 或 GDI 绘制到透明 BGRA。
- 继续调用现有 `SetSubtitleOverlay`。
- SRT 只用于缓存和导出。

字幕时间必须使用媒体时间轴。ASR 推理延迟不应通过全局字幕延迟掩盖，界面应单独显示“字幕落后多少秒”。

### 9.4 翻译优先级

翻译顺序：

1. 当前播放头附近。
2. 当前窗口。
3. 后续窗口。
4. 历史未翻译内容。

翻译不阻塞原文显示。

## 10. 统一 GPU 资源协调器

详细实现方案见同目录文档：

- `GPU-SCHEDULER-DESIGN-zh.md`

### 10.1 定位

调度器是控制面，不是数据面。

它负责：

- 资源观测。
- 预算。
- 准入。
- 租约。
- 优先级。
- 降级。
- 指标。

它不负责：

- 代替 Lmxxf 执行推理。
- 代替 FSR 执行超分。
- 代替 Whisper 执行 ASR。
- 代替翻译模型执行解码。
- 跨 API 创建通用 GPU 命令。

### 10.2 建议优先级

```text
PlaybackCritical   神经渲染
PlaybackSupport    超分、插帧
Interactive        当前请求的 ASR
Background         翻译、缓存预生成
```

### 10.3 工作负载描述

建议最少包含：

```text
kind
adapterLuid
priority
estimatedGpuMs
estimatedVramBytes
canCancel
canTimeSlice
deadline
reason
```

### 10.4 执行阶段

第一阶段：影子模式

- 只记录。
- 不改变现有执行路径。
- 输出资源压力。
- 建立真机基线。

第二阶段：准入控制

- 预留下限。
- 拒绝明显会 OOM 的任务。
- 不让后台任务启动。

第三阶段：运行时控制

- 根据帧时间暂停低优先级任务。
- 根据显存暂停模型加载。
- 根据神经渲染前置距离调整 ASR 并发。

第四阶段：自动降级

- 翻译先降级。
- ASR 再降级。
- 超分再降级。
- 神经渲染最后降级。

### 10.5 调度器收益

- 统一 GPU LUID 和显存视角。
- 保护神经渲染和音频播放。
- 防止字幕任务造成 live buffering。
- 统一降级行为。
- 统一日志和性能回执。
- 为未来 ASR、翻译、OCR 等任务提供接入边界。
- 让性能测试和回归测试有稳定接口。

### 10.6 调度器限制

- 不能提高硬件性能。
- 不能凭空减少模型大小。
- 不能跨进程共享 D3D12 fence。
- 不能抢占已经提交的细粒度 GPU 命令。
- 不能证明未实测组合一定稳定。
- 不能修复驱动崩溃。

## 11. 超载时的表现与保护

### 11.1 轻度超载

- 字幕开始落后。
- 译文晚于原文。
- 当前字幕停留时间过长。
- 翻译 backlog 增长。

### 11.2 中度超载

- 帧时间 p95 上升。
- 偶发掉帧。
- 神经渲染前置窗口缩小。
- 字幕和翻译更容易积压。

### 11.3 严重超载

- 神经渲染无法覆盖播放头。
- 播放器进入 live buffering。
- 音频暂停。
- 等待渲染恢复后继续播放。
- 多次失败后可能切回原始画面。

### 11.4 显存或驱动异常

- 模型分配失败。
- ASR 或翻译 worker 崩溃。
- 神经渲染 worker 崩溃。
- device removed。
- 渲染器重建或回退原画。

### 11.5 必须避免的表现

不允许因为字幕任务导致：

- 视频和音频无必要的缓冲暂停。
- 插帧节奏持续抖动。
- 神经渲染被强制关闭。
- 显存 OOM 跨越到渲染进程。
- 旧字幕在 seek 后重新出现。

### 11.6 降级顺序

```text
1. 减少翻译批次
2. 只翻译播放头附近
3. 暂停翻译，只显示原文
4. 降低 ASR 模型
5. ASR 改为 CPU
6. 降低超分质量或关闭超分
7. 最后才降低神经渲染设置
```

### 11.7 硬保护阈值

建议作为策略起点，不作为最终固定值：

- 显存使用达到预算 80% 到 85%：不再启动新模型任务。
- 字幕落后超过 15 到 20 秒：暂停翻译。
- 神经渲染前置不足 3 到 5 秒：暂停 ASR 和翻译。
- p95 帧时间超过源帧预算：降低低优先级任务。
- 字幕任务连续两次造成缓冲风险：关闭本轮 GPU 字幕，回退 CPU 或原文。

## 12. 模型与打包

### 12.1 模型体积

参考值：

- Whisper large-v3 FP16 模型约 1.6 GB。
- 量化 Whisper 模型通常更小，但仍有运行时内存和显存开销。
- 小翻译模型通常为数百 MB 到数 GB，取决于参数量和量化级别。

本工程不提交模型权重。

### 12.2 建议发布形态

建议将模型与主程序分离：

```text
DLSSVideoPlayer.exe
ffmpeg.exe
ffprobe.exe
neural-runtime/
amd-neural-runtime/
subtitle-runtime/       未来可选
  subtitle-worker.exe
  models/
  licenses/
  SHA256SUMS
```

`subtitle-runtime/` 为建议名称，最终实现前需要再次确认目录和升级策略。

### 12.3 安全与可验证性

- 模型下载必须使用 HTTPS。
- 文件和压缩包必须有固定哈希。
- 运行时文件必须走现有 lock 和校验模式。
- 不通过当前目录或 PATH 加载未验证模型。
- helper 必须进入 Kill-on-close Job Object。
- helper 崩溃不能拖垮播放器。
- 模型损坏时必须明确禁用并提示，而不是静默失败。

### 12.4 静态 CRT

目标工程使用 `/MT`。如果新增原生库或 helper，必须确认：

- 编译配置一致。
- 不跨 DLL 边界传递 CRT 对象。
- 动态库的运行时版本匹配。
- 测试 Release 和 Debug 配置。

独立 helper 进程通常更容易隔离 ABI 和崩溃风险，但无法和播放器直接共享 D3D12 队列。

## 13. Windows 11 + RX 9070 XT 开发准备

### 13.1 系统与工具

- Windows 11 x64。
- Visual Studio 2022 或 Build Tools。
- MSVC C++20。
- CMake。
- Git。
- PowerShell。
- Windows SDK。

### 13.2 工程依赖

根据现有文档准备：

- `amd-neural-runtime/`。
- `external/ffmpeg/bin/ffmpeg.exe`。
- `external/ffmpeg/bin/ffprobe.exe`。
- FidelityFX loader、upscaler 和 frame generation DLL。
- 构建所需的 NVIDIA/DLSS SDK 检出。

构建步骤以以下文件为准：

- `README.md`
- `docs/WIN11-DEVELOPMENT-SETUP-zh.md`
- `docs/BUILDING.md`

### 13.3 建议初始验证

在 Windows 11 + 9070 XT 上依次验证：

1. 未修改工程可以构建。
2. 可以播放本地视频。
3. Lmxxf 神经渲染可用。
4. FSR 超分可用。
5. 插帧可用。
6. 记录每种组合的功耗、帧时间、掉帧和显存。
7. 确认显卡报告为 `gfx1200` 或 `gfx1201`。

### 13.4 ASR 性能预实验

在接播放器之前，先独立测试：

```text
30 秒、16 kHz、单声道 PCM
large-v3-turbo CPU
不同线程数
不同量化版本
```

记录：

- 模型加载时间。
- 30 秒音频处理时间。
- RTF。
- 峰值内存。
- 首句延迟。
- 字幕段数量。

CPU 推荐判定：

```text
RTF <= 0.7：可以进入实时准星
0.7 < RTF < 1.0：可以实现但必须使用预缓冲和降级
RTF >= 1.0：不建议作为实时默认方案
```

### 13.5 GPU 性能预实验

分别测试：

1. 仅 Lmxxf。
2. Lmxxf + GPU ASR。
3. Lmxxf + GPU ASR + GPU 翻译。
4. Lmxxf + FSR。
5. Lmxxf + FSR + GPU ASR。
6. Lmxxf + FSR + GPU ASR + GPU 翻译。
7. 再加入插帧作为实验组。

每组至少记录：

- p50 帧时间。
- p95 帧时间。
- 掉帧数。
- 神经渲染前置距离。
- GPU 占空。
- 峰值显存。
- ASR RTF。
- 字幕 backlog。
- 翻译 backlog。
- 是否进入 live buffering。

## 14. 实施阶段

### 阶段 0：统一 GPU 资源协调器

先不添加 ASR 或翻译。

范围：

- 按 GPU LUID 汇总现有资源。
- 定义 workload 和 lease。
- 观测 Lmxxf、FSR、插帧。
- 输出压力指标。
- 只做影子模式。
- 纯策略单元测试。

验收：

- 不改变现有输出。
- 不降低可测量帧率。
- 能解释当前 GPU 资源使用。
- 能识别资源压力窗口。
- 帧路径额外开销低于测量噪声。

### 阶段 1：CPU ASR 原文

范围：

- 本地文件。
- 当前音轨。
- 16 kHz 单声道。
- 20 到 30 秒窗口。
- 2 秒重叠。
- 内存 CueStore。
- 实时字幕叠层。
- SRT 缓存。

不做：

- 网络流。
- 导出烧录。
- GPU ASR。
- GPU 翻译。

### 阶段 2：CPU 翻译

范围：

- 小批量。
- 播放头优先。
- 翻译失败回退原文。
- 不阻塞原文。

### 阶段 3：GPU ASR

条件：

- 阶段 0 调度器已提供真实准入和降级。
- ASR 性能基线已测得。
- 音频轨选择已接入。

### 阶段 4：GPU 翻译

条件：

- GPU ASR 与神经渲染组合稳定。
- 显存预算可控。
- 翻译模型足够小。

### 阶段 5：增加超分

条件：

- 720p 组合已稳定。
- 1080p24 已实测。
- 可以通过 QoS 暂停翻译或 ASR。

### 阶段 6：插帧

只在以下条件进入：

- 已有真实调度器。
- 已有跨模块 GPU 压力测试。
- 已明确实验模式。
- 或可用第二 GPU 承担字幕模型。

## 15. 测试矩阵

### 15.1 媒体

- 720p24。
- 720p30。
- 1080p24。
- 1080p30。
- 1080p60。
- 4K 输入，仅用于确认降级行为。

### 15.2 功能组合

- 原画。
- 原画 + 原文实时字幕。
- 原画 + 原文和译文。
- 神经渲染。
- 神经渲染 + 原文。
- 神经渲染 + 原文和译文。
- 神经渲染 + 超分 + 原文和译文。
- 再增加插帧。

### 15.3 操作

- 正常播放。
- 暂停和恢复。
- 前后跳转。
- 连续拖动时间轴。
- 切换音轨。
- 切换字幕开关。
- 切换翻译目标。
- 切换模型。
- 最小化窗口。
- 切换全屏。
- 断开或重建音频设备。
- 关闭媒体。

### 15.4 资源

- 模型缺失。
- 模型损坏。
- 显存不足。
- GPU 任务取消。
- worker 崩溃。
- 网络媒体不可用。
- 显存碎片。

### 15.5 验收

- 无旧字幕跨 seek 回写。
- 无音轨错配。
- 无字幕导致的播放暂停。
- 翻译不会阻塞原文。
- 模型失败可回退。
- 退出时所有 helper、fence 和 GPU 资源释放。
- 重新打开媒体使用缓存时不会重复推理。

## 16. 风险清单

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| 多套 GPU 运行时无序竞争 | 掉帧、缓冲暂停 | 先做统一调度和 shadow 度量 |
| 显存不足 | 模型或渲染失败 | 准入控制、量化模型、16 GB 优先 |
| ASR 使用错误音轨 | 字幕内容错误 | 跟随 `selectedTrack`，切轨时清空 |
| seek 后旧结果回写 | 字幕错位 | generation、取消和 stale 丢弃 |
| SRT 热更新过重 | 闪烁、CPU/IO 抖动 | 内存 CueStore + BGRA 叠层 |
| 模型体积过大 | 安装包和磁盘压力 | 独立可选运行包 |
| helper 崩溃 | 字幕中断 | 独立进程和自动重启 |
| 驱动 device removed | 播放中断 | 隔离、恢复、回退原画 |
| 1080p 性能未经实测 | 过度承诺 | 分层支持矩阵和真机回归 |
| 翻译质量不足 | 可读性差 | 专用小模型、后处理、原文回退 |

## 17. 已确定的决策

- 不从 C# 移植，只复用架构思路。
- 先实现统一 GPU 资源协调器，不先接 ASR。
- 调度器第一版只做观测和策略，不接管 GPU 命令。
- 字幕主通道使用内存 CueStore，不依赖 SRT 热更新。
- ASR 必须跟随当前音轨。
- 第一版优先本地文件。
- 第一版原文优先，翻译可降级。
- 字幕任务不能让神经渲染进入无必要的 live buffering。
- 全组合不作为默认稳定能力。
- Windows 11 + RX 9070 XT 是首要验证平台。

## 18. 待确认问题

- GPU ASR 和翻译最终选 DirectML 还是 Vulkan。
- ASR 和翻译是独立 helper 还是同进程统一设备。
- 目标 GPU 实际可用显存和驱动版本。
- 9070 XT 上 large-v3-turbo 的具体 RTF。
- 小翻译模型的目标质量和体积。
- 字幕是否需要双语混排。
- 是否支持网络媒体。
- 模型下载是否由播放器内完成。
- 多 GPU 或核显回退是否进入第二版。
- 调度器是否允许配置覆盖。

注意：由于当前工程使用静态 CRT，若选择 DirectML 同进程共享 D3D12 设备，需要额外确认运行时库、COM 和异常边界；若选择独立 helper，则必须接受跨进程无法直接共享 D3D12 队列和 fence 的限制。

## 19. 参考资料

目标工程：

- `README.md`
- `docs/ARCHITECTURE.md`
- `docs/USAGE.md`
- `docs/TROUBLESHOOTING.md`
- `docs/WIN11-DEVELOPMENT-SETUP-zh.md`
- `docs/AMD-LMXXF-INTEGRATION-PLAN-REV2-zh.md`
- `docs/AMD-TEMPORAL-ACCUMULATION-zh.md`
- `src/AudioPlayer.cpp`
- `src/SubtitleOverlay.cpp`
- `src/SubtitlePolicy.h`
- `src/D3D12Renderer.cpp`
- `src/AmdLmxxfEvaluator.cpp`
- `src/FsrUpscaleBackend.cpp`
- `src/FsrFrameGenBackend.cpp`

Transub-Player 参考位置：

- `Transub-Player/src/TransubPlayer/Services/AsrPipeline.cs`
- `Transub-Player/src/TransubPlayer/Services/AsrAudioExtract.cs`
- `Transub-Player/src/TransubPlayer/Services/PreviewController.cs`
- `Transub-Player/src/TransubPlayer/Services/PreviewSubtitleSync.cs`
- `Transub-Player/src/TransubPlayer/Services/SubtitleFile.cs`
- `Transub-Player/src/TransubPlayer/Services/TranslateClient.cs`
- `Transub-Player/src/TransubPlayer/Services/LlamaServerProcess.cs`
- `Transub-Player/src/TransubPlayer/Services/PreviewPaths.cs`

## 20. 最终判断

这套方案在 RX 9070 XT 上有现实可行的落地路径，但必须按能力和分辨率分阶段交付。

最稳妥的顺序是：

```text
先建立统一 GPU 调度和观测
再做本地 CPU 实时字幕
再做 GPU ASR 与神经渲染组合
再做 GPU 翻译
再做超分
最后评估插帧和全组合
```

第二版最有把握的范围是：

```text
本地文件
单 pass 神经渲染
720p24 到 30
GPU 量化 ASR
小型 GPU 翻译模型
有界字幕与翻译队列
严格降级策略
```

1080p24 可以列为条件支持，1080p30 和全组合必须等待 Windows 11 + RX 9070 XT 真机实测。

