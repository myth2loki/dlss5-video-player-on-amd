# macOS 移植 — 阶段 1-5 + 对比视图 / 音轨 / 字幕

这个目录不是完整的播放器，是一个**逐层验证**的移植：先证明最贵的两个不确定性
（ffmpeg 管道能不能原样用、Metal 能不能干净地接上），再证明音频时钟能不能
撑起音画同步，然后把逻辑层整块搬过来，最后用 Qt6 把这些东西组装成一个能用的窗口。

## 结论

**能移植。** 而且比预想的便宜，因为解码根本不在这份代码里。

`src/VideoDecoder.cpp` 没有链接 libav*：它 spawn `ffmpeg.exe`，把
`-f rawvideo pipe:1` 的裸像素从管道读出来（见它第 1024 行附近）。音频
（`src/AudioPlayer.cpp`）同理。这意味着**换平台换的是进程创建方式和几个
参数名，不是解码器本身**。

## 已经跑通并测过的东西

### 画面（阶段 1）

| 项 | 结果 |
|---|---|
| ffprobe 解析 | width/height/r_frame_rate/duration 全对，`24000/1001` 这种分数帧率也还原正确 |
| ffmpeg 裸流管道 | `-pix_fmt bgra -fps_mode cfr -r <fps> -f rawvideo pipe:1` 在 macOS 上逐字节兼容 |
| 帧数与容器一致 | 640×360/3s/24fps → 精确 72 帧；2118×1046/6s/30fps → 精确 180 帧 |
| seek 精度 | 请求 1.800s → 落在第 43 帧（= 1.800×24），**不是关键帧**，误差 0 |
| 解码吞吐 | 640×360: 675 fps；2118×1046: 227 fps（都是多倍实时） |
| Metal 着色器 | 运行期编译通过，管线状态构建成功 |
| Metal 上屏 | 离屏渲染回读：0% 残留清屏色，40015 次颜色跳变（纹理确实到达着色器） |

### 声音（阶段 2）

| 项 | 结果 |
|---|---|
| 音轨探测 | `aac 48000 Hz 2ch`，无音轨的文件不会报错而是静音播放 |
| ffmpeg 音频管道 | `-map 0:a:0? -vn -sn -dn -ac 2 -ar 48000 -c:a pcm_f32le -f f32le pipe:1` 兼容 |
| AudioQueue 打开 | 按源采样率与声道数开，两端都不重采样 |
| **主时钟成立** | 1.010s 墙钟内音频位置走 0.994s，**误差 -1.6%** |
| 暂停 | 时钟冻结在 0.994s，不跑偏；恢复从同一点继续 |
| seek | 请求 1.500s → 时钟从 1.500s 起算 |
| 停止 | 报告负值，调用方回退到自己的计时器 |
| 音频启动延迟 | 首帧 338 ms（含 ffmpeg 子进程启动 + 首段解码） |

### 逻辑层（阶段 3）

| 项 | 结果 |
|---|---|
| 移植代码量 | **16441 行**（policy 目录，77 个头文件） |
| 其中 policy 文件 | 11770 行 / 50 个 |
| **非 include 的逻辑改动** | **仅 17 行**（只有 RuntimeModulePolicy 的一处类型歧义） |
| 完成未改的文件 | 19 个，逐字节一致 |
| 排除的文件 | 5 个，各有明确理由（见下） |
| 语义验证 | RECT 右上开区间、COLORREF 0x00BBGGRR、MulDiv 四舍五入、`%ld` 往返、UTF-8 无损 |

`ctest` 14 项全绿（含 `compare-views` 的离屏 PNG 合成验证与 `media-lists` 的字幕/音轨验证）。运行：`cmake -S mac -B mac/build -G Ninja && cmake --build mac/build && ctest --test-dir mac/build`

### 界面（阶段 4）

| 项 | 结果 |
|---|---|
| 框架 | Qt 6.12.0（Widgets，非 QML） |
| 编译 | 零警告 |
| 实际运行 | 2118×1046 视频正常播放，工具栏图标全部渲染，时间轴进度跟随 |
| 状态栏 | `2118 × 1046 · 30.00 fps · audio drives the clock` |

### 神经渲染缓存（阶段 5）

仿 Windows 的 `NeuralCoverage` / `SynchronizedPlayback` / `LiveSessionPolicy`，
**不再需要把整部影片转换完才有画面**：

| 项 | 实现 |
|---|---|
| 区间渲染 | `mlxdlss --start-frame N --frames M`，两端向下取整到帧边界 |
| 缓存 | `<cache>/<source-key>/<kind>/segments/` + `coverage.json`；key = 路径 + 大小 + 修改时间 |
| 配对播放 | 原片解码器（借用窗口已有的）+ 片段单例解码器，按时间戳配对，片段帧号平移回源时间轴 |
| 对比切换 | 画面左下角 `DLSS 5` / `ORIGINAL` 两个按钮，也可用 Compare 菜单或 `D` 键；暂停时重绘**同一帧** |
| 活会话 | 一块渲完自动接下一块，块长 10/20/30/60/120 s 可选（默认 30 s），低水位、seek 重定向、开/关、片尾收尾 |
| 缓存管理 | Neural 菜单里可清除当前影片的全部片段、打开缓存目录；渲染结果文件不存在时不记入覆盖 |
| 2x 插帧段 | 片段自带帧率，60 fps 段能正确配对 30 fps 原片 |
| 设置 | 块长、对比视图、音量、最近使用的 feature 存 QSettings |

### 对比视图 / 音轨 / 字幕（阶段 6）

| 项 | 实现 |
|---|---|
| 八种模式 | DLSS 5、Original、Blend、Split、Wipe、Difference、Side by side、2x2。编号沿用 Windows `ComparisonMode`，pane 布局与像素算法在 `policy/CompareCompose.h` |
| CPU 合成 | `CompareCompositor` 只在需要时做一次全帧 Mix / Difference / Spatial，结果按参数缓存；`VideoSurface` 用 QPainter 裁剪 pane，不需要第二个图形 API |
| 交互 | Split/Wipe 分割线可点击拖动，`C`/`Shift+C` 循环模式，`[`/`]` 调 Mix（0–200%），`Shift+[`/`Shift+]` 调 Difference 增益，`X` 交换两侧，2x2 第四格的 Mix 可选；**按住画面 150 ms 窥看原片**（`compare_gesture` 策略），松开即恢复 |
| 保存对比图 | File ▸ Save comparison image（`Ctrl+Shift+S`），导出所见即所得的 PNG；Side by side 输出 2W×H，2x2 输出 2W×2H |
| 音轨 | `AudioPlayer::ProbeTracks` 用一次 ffprobe 枚举全部音轨，按 `AudioTrackPolicy` 选默认（跳过评论轨），Playback ▸ Audio 可在播放中切换，ffmpeg `-map 0:a:N` 跟随 |
| 字幕 | 自动发现同名 `.srt/.vtt/.ass/.ssa` 外挂字幕，支持 UTF-8 与 GB18030、时间偏移、HTML/ASS 标记清理；QPainter 描边绘制，Playback ▸ Subtitles 可关闭/切换/手动加载 |
| 设置 | 模式、Mix、增益、分割线位置、Swap、亮度差、第二 Mix 全部存 QSettings |

**必须说清的一件事**：本机神经渲染实测约 **0.35x 实时**（95 ms/帧 @
2118×1046），Windows 那台是 2.3x。所以渲染永远追不上播放，播放时大部分画面
仍是原片，缓存是在后台持续累积的；回拖、重播或下次打开时才能看到已转换的
区间。状态栏在这种情况会提示"渲染落后于播放"。这不是 bug，是这台机器的
算力上限，调度策略已经按它调整（块从渲染头连续推进，而不是从播放头起跳，
否则每块都会跳过中间那一段）。

**Qt6 是怎么装的（Homebrew 装不上）**：`brew install qt` 在最后一步 `disclaim`
时调用 `sandbox-exec`，而本机策略拒绝它（`sandbox_apply: Operation not
permitted`，与联软终端管控有关），`HOMEBREW_NO_SANDBOX=1` 也绕不过，因为那一步
自己就调 `sandbox-exec`。改用 Qt 官方的 `aqtinstall`：

```bash
python3 -m venv /tmp/aqtenv && /tmp/aqtenv/bin/pip install aqtinstall
/tmp/aqtenv/bin/aqt install-qt mac desktop 6.12.0 clang_64 --outputdir "$PWD/.qt"
cmake -S mac -B mac/build -G Ninja -DCMAKE_PREFIX_PATH="$PWD/.qt/6.12.0/macos"
```

`-m qttools` 会因 ftp.jaist.ac.jp 超时失败，但 `qtbase` 已经装好了 —— 播放器只要
Widgets，不需要 tools。`.qt/` 211MB，已在 `.gitignore` 里。

## 这个原型里有什么

```
mac/src/FfmpegPipeSource.{h,cpp}    ffmpeg 子进程 + 管道 + 有界队列 + seek   330 行
mac/src/MetalVideoPresenter.{h,mm}  BGRA 上传 + 单 quad + letterbox 呈现      185 行
mac/src/CoreAudioSink.{h,mm}        AudioQueue 输出 + 播放帧计数 + 淡出停     ~430 行
mac/src/AudioPlayer.{h,cpp}         ffmpeg 音频子进程 + 主时钟 + 设备变化      ~600 行
mac/src/AudioClockPolicy.h          原项目同名文件，**零修改复制**            159 行
mac/src/PlaybackClock.h             时钟（音频优先，计时器兜底）              105 行
mac/src/main.mm                     窗口 + display link + 按键               441 行
mac/src/UiTypes.h                   Win32 词汇层的兼容实现                    513 行
mac/src/UiTheme.h/.cpp              调色板 + 图标码位 + 按钮状态规则           213 行
mac/src/PlatformCompat.h            移植文件的统一入口                        21 行
mac/policy/                         移植过来的 77 个头文件                  16441 行
mac/src/VideoSurface.{h,cpp}        Qt 视频控件（QImage 直接包 BGRA）        ~180 行
mac/src/TimelineWidget.{h,cpp}      Qt 时间轴（三车道，缩略图未做）           ~200 行
mac/src/ToolbarWidget.{h,cpp}       Qt 工具栏（坐标/配色全部来自 policy）     ~330 行
mac/src/PlayerWindow.{h,cpp}        主窗口：组装 + 播放循环                   ~300 行
mac/src/main_qt.cpp                 Qt 入口                                    45 行
```

### Qt 层复用了什么，重写了什么

这是整个移植最值得看的一张表。**坐标和颜色一行都没重写**：

| 项 | 来源 |
|---|---|
| 工具栏每项的矩形 | `LayoutToolbar()`（policy，原样） |
| 点击命中哪个按钮 | `HitTestToolbar()`（policy，原样） |
| 按钮该是什么颜色 | `ResolveButtonVisual()`（policy，原样） |
| 某个动作现在能不能用 | `IsToolbarActionEnabled()`（policy，原样） |
| DPI 缩放 | `MulDiv()`（兼容层，与 Win32 同语义） |
| 图标码位 | `GlyphForIcon()`（policy，原样） |
| 视频帧格式 | BGRA → `QImage::Format_RGB32`，**零转换** |

真正新写的只有：`QPainter` 画圆角矩形和文字、鼠标/键盘事件、以及主窗口的组装。
这就是为什么 Qt 这条路值：它让"重写 UI"退化成"重写绘制"。

### 排除的 5 个文件，以及为什么

移植不是"全搬"。这 5 个留在 Windows，因为它们要的不是词汇而是平台能力：

| 文件 | 理由 |
|---|---|
| `AudioEndpointPolicy.h` | `IMMNotificationClient` 是 COM 回调接口 |
| `AudioPassthroughPolicy.h` | IEC 61937 走 `KSDATAFORMAT` GUID，macOS 无 SPDIF 直通 API |
| `DarkModePolicy.h` | `UAHMENU`/`WM_UAHDRAWMENU` 是未文档化的菜单绘制协议 |
| `NvencDirectPolicy.h` | NVENC 编码器，macOS 没有 |
| `RendererRecoveryPolicy.h` | 接收 `HWND` 并调用窗口自己的设备丢失恢复 |

## 四个设计决定，以及为什么

**1. 音频是主时钟，不是"顺便也播声音"。**
Windows 版的播放闸门（presentation gate）按音频位置决定该显示哪一帧。
自己计时会让画面相对声音漂移——声卡晶振和系统时钟的差是每几分钟一帧，
十分钟内就听得出来。用设备"已消费的帧数"驱动画面，漂移从结构上消失，
因为只有一个时钟。`PlaybackClock::Position()` 就是这个规则的落点。

**2. `AudioClockPolicy.h` 原样搬，不重写。**
里面的 `Usable` / `Present` 解决的是"音频位置突然不动了怎么办"——
ffmpeg 子进程结束、缓冲欠载、设备掉线，都会让位置冻结，而播放闸门会
因此把画面永久停住。它的解法是：冻结超过 0.5s 就把时钟按墙钟速度往前带，
音频恢复时以不超过 5% 的速度斜率追回来，差超过 1s 才一次性跳。这是纯算术，
和平台无关，所以一行没改。

**3. AudioQueue 而不是 AudioUnit 图。**
拿到的是无帧边界的字节流，AudioQueue 的 buffer 回调是 1:1 映射；
换 AUHAL 还要自己拥有渲染线程和时钟，而这两样 AudioQueue 已经给了。

**4. 兼容层用 `using` 而不是重写，把改动从 16441 行压到 17 行。**
policy 层写的是 `RECT`、`COLORREF`、`UINT`、`%ld`——这是**词汇**，不是平台。
两条路：改写 11770 行换新名字并维护两份逻辑，或者在 `UiTypes.h` 里把名字
定义一次、只留一份。选了后者。关键是要精确匹配语义：`RECT` 右/下开区间、
`COLORREF` 是 0x00BBGGRR、`MulDiv` 半远离零舍入——任一处走样，整个 UI 就会
差一个像素或颜色互补，而且照样能画出来，看不出来是 bug。

## 踩到的坑（都修了，记下来省得再踩）

1. `Metal.h` 会拉 Foundation（Objective-C），**不能**在 `.cpp` 里 include。
   解决：`MetalVideoPresenter.h` 用 pimpl，layer 以 `void*` 传递，
   实现在 `.mm` 里 `(__bridge CAMetalLayer*)`。
2. `posix_spawn` 不走 shell。用 shell 引号拼命令再按空格切分，引号会
   带进参数，ffmpeg 找不到文件——表现为解码 **0 帧**。必须直接构造 argv。
   （ffprobe 走 popen，有 shell，可以用引号。）
3. **`Write` 的无界等待会死锁。** 设备不再要数据时队列回调不再触发，
   读取线程永久阻塞，`Stop()` 的 join 就永远等下去（实测被 SIGTERM）。
   解决：等待改成 20ms 超时轮询 + 检查 `stop`/`paused` 标志。
4. **暂停后音频会自己重启。** 写入线程正卡在 `Write` 里，暂停时
   `FadeOutAndStop` 停了队列，但那个线程拿到缓冲区后又 `AudioQueueStart`。
   解决：`Write` 接受 abort 标志，且在往队列里放缓冲区**之前**检查。
5. `ReaderLoop` 曾在子线程里读 `m_path`/`m_track`，而 `Seek()` 在主线程
   重写它们——真实的数据竞争。解决：参数按值传入子线程。
6. **`LONG` 必须是 `long`，不是 `int32_t`。** Windows 上两者都是 4 字节，
   LP64 上不是。`WindowPlacementPolicy` 用 `%ld` 读写 `LONG`，而 varargs
   格式必须精确匹配宽度，否则读的时候会从 8 字节槽里取 4 字节，后面全是
   垃圾。让宽度跟着格式走，而不是跟着 Windows 的结构体走。
7. **`UINT_MAX` 不能自己定义。** `<climits>` 把它定义成宏，宏会赢过任何
   声明，于是 `constexpr UINT UINT_MAX` 展开成 `constexpr UINT 4294967295u`
   直接语法错误。同理 `LONG_MIN`/`LONG_MAX`。
8. **`mbsrtowcs` 不能用来做 UTF-8 转换。** 它服从 C locale，而 GUI 进程
   启动时是 `"C"`，所有 0x80 以上的字节都会失败。从 Finder 启动的播放器
   永远不会调 `setlocale`，于是只有文件名带中文的用户会遇到——对中文播放器
   来说就是绝大多数用户。改成手写 UTF-8 编解码。
9. `to_wstring(timeError ? 0 : count())` 在 LP64 上有歧义：三元表达式两个
   分支类型不同，结果是 `long`，`to_wstring` 的 `long`/`long long` 两个
   重载都不匹配。这是唯一一处需要改逻辑的地方（17 行里的全部）。

## 还没做的

阶段 6 之后，播放、渲染缓存、双帧配对、八种对比、音轨、外挂字幕都是可用的。
剩下的是**需要另一条呈现路径或大量 UI** 的能力，不是播放逻辑：

- **对比的进阶部分**：空间蒙版（手绘遮罩）、Zoom / Loupe、
  线性光下的 Difference（当前在 sRGB 字节上算，是近似）、Compare bar 工具条。
  `CompareMaskPolicy.h` / `CompareViewPolicy.h` / `CompareBarPolicy.h` 的判断逻辑
  都已在 policy 层，缺的是绘制与手势。
- **实时插帧**：播放中把 swapchain 速率翻倍的 `PlaybackFrameGeneration`。
  离线 2x 导出是有的，也能作为片段播放，但播放仍按源帧率走。
- **内嵌字幕 / 位图字幕 / 语音识别与翻译**：当前只解析同名外挂文本文件；
  `SubtitleOverlay.cpp` 的 libass 通道、PGS/VobSub 与整条翻译链未移植。
- **起始页**：最近播放、示例视频、能力面板（`StartScreenPolicy.h` 已在 policy 层）。
- **时间轴缩略图**（`TimelinePolicy.h` 的 `ThumbnailArguments` 已在 policy 层）。
- **完整缓存管理器**：Windows `NeuralCacheManager` 的 SHA-256 内容校验、
  staging/promote、quarantine、容量淘汰；mac 目前只校验文件存在与帧数。
- **HDR 呈现 / NV12 协商 / 超分（RTX VSR、DLSS SR）**：需要 Metal 呈现路径。

### 建议的下一步

1. **Compare bar**：`CompareBarPolicy.h` 的分档布局已移植，把模式/Mix/增益/交换
   从菜单挪到控制条上方，比每次开菜单直观得多。
2. **Loupe**：`CompareViewPolicy.h` 的 `compare_loupe` 是纯函数，接进
   `VideoSurface` 的鼠标事件即可（按住窥看已经接好）。
3. **起始页**：`IdleSurfaceLayout`（标题、副标题、两个动作、最近瓷砖）画出来
   就是一个像样的落地页。
4. **时间轴缩略图**：现有 ffmpeg 管道加一路低分辨率 seek 解码即可。

### 一个值得考虑的工程问题

现在 `dlss5-qt` 直接 `#include` 了 `policy/UiLayout.cpp` 和 `mac/policy/` 的头文件。
这是对的 —— 一份实现。但如果上游 `src/` 更新，`mac/policy/` 需要同步，而现在
同步全靠人工 `diff`。

值得加一个脚本：对每个移植文件，断言它与上游的差异只有 include 行。这能把
"17 行改动"这个约束变成 CI 可验证的事实，而不是一次性的观察。

## 明确不要做的事

- **不要用 AVPlayer 偷懒。** 原项目的价值在渲染缓存、seek 后原始/渲染
  混播、compare views——这些都建立在自己的帧管线上，AVPlayer 接不进去。
- **不要试图在 macOS 上重写神经渲染模型。** 本移植走的是同仓库的
  `mlxdlss` CLI（离线按区间产出片段），不是把 `LmxxfNrRuntime.dll` /
  `MochizukiNrRuntime.dll` 的 D3D12/HIP/Vulkan 后端搬过来。要实时推理得有
  Metal/CoreML 后端，那是另一个项目；这里的价值在于缓存、配对、对比这条
  播放链路与后端无关。
- **不要为了"跨平台"改上游的 `src/`。** `mac/policy/` 是副本，`src/` 是
  上游。一旦开始两边同时改，就再也说不清哪边是对的。上游的修复照原样
  cherry-pick 到 `mac/policy/`，并保持"只改 include"这个约束——它现在能被
  验证（17 行），一旦破了就验证不了。
