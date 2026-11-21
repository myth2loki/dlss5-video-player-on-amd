# Chromecast 投屏功能 · 详细技术方案

_状态：设计定案，待实施。目标机器：Windows 11 x64 + RX 9070 XT（gfx1201）。目标电视：Sony X9500H（Android TV / Google TV，内置 Chromecast）。_

_本文自包含，可在另一台电脑上照此实施。实施前**必须先做第 2 节的 POC**。_

---

## 1. 结论先行

在播放器里加一个投屏功能：**抓渲染输出帧 → H.264/AAC 编码 → HLS live 切片 → 内置 HTTP 服务 → CastV2 推给 X9500H 的 Default Media Receiver**。电视原生播放、零安装。

选择 Chromecast 而非 DLNA 的理由：X9500H 自带 Chromecast；Cast 对 live 流支持成熟，DLNA 电视实现对"无限实时流"支持参差；Cast 格式宽（H.264/AAC 即可），DLNA 被锁在 MPEG-TS+H264+AC3、≤20Mbps、Level 4.1。代价是 PC 端要自实现 CastV2 协议栈（无官方 Windows 桌面 SDK）。

**大部分设计决策来自一轮 grill，已收敛，无悬空项。第 2 节的 POC 是唯一的开路前置。**

---

## 2. ⚠️ 先做 POC，不要直接写 C++

**这是硬性前置。** 全链路有三处未证实的风险，任何一处不成立都会让后面的 C++ 工作作废。用现成工具在两个晚上内验证完，再动产品代码。

### 2.1 三处待验证风险

| # | 风险 | 为什么可能失败 |
|---|---|---|
| R1 | **X9500H 的 Cast 能否播 HLS live** | Default Media Receiver 支持 HLS，但"无限时长、边生成边播"的 playlist 在电视端是否稳定，无明证 |
| R2 | **CastV2 发送端能否不验证设备证书直连** | 资料称 client 可跳过 device auth，但 X9500H（非独立 dongle）的 TLS 行为需实测 |
| R3 | **H.264 编码吞吐能否跟上实时** | 项目现成编码器只有 HEVC（NVENC/AMF）和 libx264/ffv1，H.264 硬件编码路径要新建；RX 9070 XT 上 h264_amf 的实测吞吐未知 |

### 2.2 POC 步骤（不改产品代码）

工具：一台 Windows PC（目标机器即可）、X9500H、同一局域网、Node.js、`npm i castv2 mdns`、随包 FFmpeg。

**Step 1 — 验证 HLS live 能被 Cast 播放（R1 + R2）**

1. 用 FFmpeg 造一段无限 H264 live 流（测试源即可）：
   ```bat
   ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=30 ^
     -c:v libx264 -preset ultrafast -tune zerolatency -g 60 ^
     -f hls -hls_time 2 -hls_list_size 5 -hls_flags delete_segments ^
     -hls_segment_type mpegts cast_poc\live.m3u8
   ```
2. 起一个静态 HTTP 服务指向 `cast_poc\`（任何工具，`python -m http.server 8080` 即可）。
3. 用 node-castv2 发现 X9500H、连 8009、LAUNCH `CC1AD845`（Default Media Receiver），再在 media channel 发 `LOAD`，`contentId` 填 `http://<PC局域网IP>:8080/live.m3u8`。
4. **判定**：电视开始播放且持续不卡 ≥ 60 秒 → R1/R2 通过。若电视报格式错误或只播首个分段，转 DLNA 方案（保留本文作参考）。

**Step 2 — 验证 H.264 硬件编码吞吐（R3）**

1. 在目标机器上跑 FFmpeg 的 `h264_amf`（RX 9070 XT）和 `h264_nvenc`（若换 N 卡）各一次：
   ```bat
   ffmpeg -f lavfi -i testsrc2=size=1920x1080:rate=30 -t 10 ^
     -c:v h264_amf -quality speed -b:v 8M -f null -
   ```
2. **判定**：1080p30 单帧编码耗时 < 20ms（留余量给抓帧+切片）→ R3 通过。若 AMF 不达标，退化用 `libx264 -preset ultrafast -tune zerolatency`，并实测 CPU 占用；仍不达标则把目标降到 720p30。

**Step 3 — 记录基线延迟**

从按下播放到电视出画面计时。HLS live 固有延迟 = 分段时长 × playlist 长度，典型 6–10 秒。确认这个延迟对使用场景可接受。

**POC 通过标准**：Step 1 持续播放成功 + Step 2 编码达标 + Step 3 延迟可接受。三条全过才进入第 3 节。

---

## 3. 架构与数据流

```
┌──────────────────────── 播放器进程 ────────────────────────┐
│                                                               │
│  SynchronizedPlayback ──→ D3D12Renderer(Present)              │
│         │                        │                            │
│         │                        ├── [新增] 呈现帧抓取环        │
│         │                        │      (CapturedVideoFrame)   │
│         │                        ▼                            │
│         │                  [新增] H264/AAC 编码                │
│         │                        ▼                            │
│         │                  [新增] HLS 切片器                   │
│         │                        ▼                            │
│         │                  [新增] HTTP 服务(:8080)             │
│         │                        │                            │
│         │                  http://PC_IP:8080/live.m3u8         │
│         │                        │                            │
│         └── [新增] Position() ───┤                            │
│                                  ▼                            │
│                     [新增] CastV2 发送端                       │
│                     mDNS 发现 ─ TLS:8009 ─ LOAD(URL)          │
└──────────────────────────────────┬────────────────────────────┘
                                   ▼
                    X9500H Default Media Receiver (CC1AD845)
                          电视原生解码播放，零安装
```

**关键设计**：所有新增模块跑在播放器进程内（或作为其子进程），抓的是**渲染/呈现帧**，与三段效果开关解耦——三段开了推效果画面，没开推原始画面。

---

## 4. 现成能力盘点（能复用就别重写）

| 能力 | 现成实现 | 位置 | 复用方式 |
|---|---|---|---|
| 抓帧环 | `D3D12Renderer::Initialize(captureOutput=true)`、`ResolveOldestCapture` / `BeginResolveOldestCapture` / `EndResolveOldestCapture`、`CapturedVideoFrame`、`CaptureSlots=4` | `src/D3D12Renderer.h:150,328,357-425`；`src/D3D12Renderer.cpp:5335,5595,5755` | 播放器端开启 `captureOutput`，从渲染循环 drain。现 ring 是 host-visible 4 帧，4K 占 133MB，注意预算 |
| GPU 转 NV12 | `FrameGenColorConvertShader.h`、`Nv12Convert.h`、`encoder.tip.gpu_convert` | `src/`、`src/Localization.h:564` | 编码器要 NV12 输入时复用现有 GPU 转换 |
| FFmpeg 子进程 | `FindHelper(L"ffmpeg.exe")`、`BuildEncoderArguments`、`media_tools::FindTool` | `src/MediaPipeline.cpp:695,971`、`src/MediaTools.h` | HLS 切片直接复用 FFmpeg 子进程 |
| 播放进度 | `Position()` / `Audio().PositionSeconds()`、片源总时长 | `src/main.cpp:10832` | 对话框进度条直接取，零新增 |
| 菜单/对话框 | `IDM_*` 常量、`HandleCommand`、`RunOwnedModalDialog`、`SettingsDialogChrome` | `src/AppMenu.h:20+`、`src/main.cpp:1048,15956,16005,8055` | 照现有模式加菜单项和对话框 |
| 配置持久化 | `GetPrivateProfileStringW` / `WritePrivateProfileStringW`、`SettingsPath()` | `src/main.cpp:1943,4012` | INI 存设备名/上次选择 |
| 状态行 | `status_chips`、`status_note` | `src/StatusChipPolicy.h`、`src/StatusNotePolicy.h` | 加 Cast 段作辅助提示，不动三块 chip |

**明确没有的（必须新建）**：H.264 编码配置、HLS 切片接线、HTTP 服务、mDNS 发现、CastV2 协议栈、Cast 对话框、菜单项与生命周期管理。

---

## 5. 分模块详细设计

### M1 · 呈现帧抓取

**目标**：从播放器渲染路径持续拿到带当前画面状态的帧，送编码器。

**改动**：
1. 播放器构造 renderer 时把 `captureOutput` 打开（`D3D12Renderer::Initialize(..., captureOutput=true)`）。当前注释明说"player never captures"，要改这个约定。
2. 在渲染循环（ Present 之后或 Present 前，取决于哪一层是最终合成画面）调用 `CaptureEvaluatedFrame` 或 `ResolveOldestCapture` 取帧。
3. 用 `BeginResolveOldestCapture` / `EndResolveOldestCapture` 把 copy 放到工作线程，避免阻塞渲染线程。
4. **背压**：`CaptureSlots=4` 是硬上限。编码器跟不上时，renderer 会在 Acquire 处等待——必须能丢弃最旧帧而不是卡死播放。参考 `NvencDirect` 的 `moreQueued` 丢帧逻辑（`src/NvencDirect.h:133-136`）。
5. **本地停显**：进入投屏后跳过本地 Present 的屏幕输出，但保留渲染调用（否则没帧可抓）。对话框成为主界面。

**验收**：连续抓 1000 帧无丢序、无内存增长；本地停显时渲染仍在进行（日志可证）。

### M2 · H.264 / AAC 编码 + HLS 切片

**关键事实**：项目现成编码器只有 HEVC（NVENC/AMF）、libx264、ffv1（见 `src/NeuralJobPolicy.h:88`、`src/MediaPipeline.cpp:733-787`）。**没有 H.264 硬件编码配置**。

**方案（按优先级）**：
1. **首选**：在 `BuildEncoderArguments` 新增 `H264Amf`（A 卡）/ `H264Nvenc`（N 卡）分支，复用现有 FFmpeg 子进程框架。POC 已测过吞吐才采用。
2. **回退**：`libx264 -preset ultrafast -tune zerolatency -g <fps*2>`（CPU，POC 测过达标才用）。
3. **不采用** `NvencDirect` 直编码——它只支持 HEVC（`NvencDirectPolicy.h:60` 明确拒绝非 `HevcNvenc`），改成 H.264 要重配整套 NVENC 参数，不值得。

**AAC 音频**：从 `AudioPlayer` 的 PCM 输出编码 AAC，`-c:a aac -b:a 128k`。Cast Default Receiver 通用支持。若音频路径难接，可先做无声版本（HLS 只有视频轨），POC 阶段验证无声能否播放。

**HLS 切片**：FFmpeg 现成能力，接在编码器输出后：
```
-f hls -hls_time 2 -hls_list_size 5 -hls_flags delete_segments -hls_segment_type mpegts live.m3u8
```
- 分段时长 2s、playlist 5 段 → ~10s 缓冲，抗抖动；
- `-re` 不需要（帧由渲染节奏供给，是实时的）；
- **constant framerate 必须保证**——索尼/Cast 对 VFR 敏感（Serviio Wiki 有前科），`-r 30 -fps_mode cfr`。

**验收**：HLS 目录持续产出、playlist 滚动更新、电视端持续播放不卡。

### M3 · HTTP 服务

**目标**：把 HLS 目录暴露成局域网可达 URL。

**要求**：
- 绑定 `0.0.0.0`（不是 localhost，否则电视访问不到）；
- 支持 `Range` 请求（播放器拖拽会发）；
- 正确 MIME：`.m3u8` → `application/vnd.apple.mpegurl`、`.ts` → `video/mp2t`；
- CORS 头（Cast receiver 跨域拉取）；
- 端口可配（默认 8080），冲突时报错。

**实现**：Windows 上可用 `winhttp`/`IHttpServer` 自写轻量服务，或起 FFmpeg/Python 子进程（发布时不宜依赖 Python，建议 C++ 自写，几百行）。

**验收**：电视能 `GET` 到 playlist 和分段；拖进度时 Range 正常。

### M4 · mDNS 设备发现

**目标**：发现局域网内所有 `_googlecast._tcp` 设备（X9500H 及可能的其他 Cast 设备）。

**实现**：
- UDP 5353 发 mDNS 查询 `_googlecast._tcp.local`，解析 SRV/A/PTR 记录拿 IP + 端口（通常 8009）+ 设备名；
- 或用现成库（项目无网络库依赖，需评估引入；也可自写简化版 mDNS 解析）；
- 扫描窗口 ~3 秒，结果去重；
- 记住上次选择的设备名到 INI，下次优先连接。

**动态子菜单**：File > Cast to TV > [发现的设备列表] + [刷新扫描]。和现有 `Recent videos` 菜单动态生成同套路（`src/AppMenu.cpp:66-68`）。

**验收**：X9500H 出现在列表；关机后消失；刷新能重新扫到。

### M5 · CastV2 协议栈（核心新增）

**目标**：作为 Cast sender 控制电视加载并播放 HLS URL。

**协议要点**（来源：`thibauts/node-castv2` 的协议文档、MediaX `QtCastSender`、Open Screen `libcast`）：
1. mDNS 发现后，TCP 连电视 **8009** 端口，TLS 握手（**跳过设备证书验证**，POC 验证 R2）；
2. 消息格式：4 字节大端长度前缀 + protobuf `CastMessage`（`.proto` 在 Chromium 源码树，字段见 POC 参考）；
3. 建立虚拟连接：`urn:x-cast:com.google.cast.tp.connection` 发 `{"type":"CONNECT"}`；
4. 心跳：`urn:x-cast:com.google.cast.tp.heartbeat` 每 5s `PING`，等 `PONG`；
5. LAUNCH：`urn:x-cast:com.google.cast.receiver` 发 `{"type":"LAUNCH","appId":"CC1AD845","requestId":N}`；
6. 从 `RECEIVER_STATUS` 响应拿 `transportId`（如 `web-5`）和 `sessionId`；
7. 对 transportId 建立 CONNECT，然后在 `urn:x-cast:com.google.cast.media` 发 `LOAD`：
   ```json
   {"type":"LOAD","requestId":N,"media":{
     "contentId":"http://PC_IP:8080/live.m3u8",
     "contentType":"application/vnd.apple.mpegurl",
     "streamType":"LIVE"
   },"autoplay":true}
   ```
8. 停止：`{"type":"STOP","sessionId":"..."}` 或 receiver 层 STOP。

**参考实现**（照着写，别从零摸索）：
- `https://github.com/thibauts/node-castv2` —— 协议文档最全，client 无设备认证限制；
- `https://astute-systems.github.io/MediaX/user-guide/cast` —— C++/Qt 原生实现，架构与本方案几乎一致（mDNS + CastV2 + HLS live），**注意它标注 Windows 不支持**（OpenSSL 依赖），但协议逻辑可参考；
- `https://chromium.googlesource.com/openscreen/+/.../cast` —— Google 官方 Cast 协议实现（libcast），最权威但最重。

**依赖**：TLS 用 OpenSSL（项目已有 FFmpeg，可能已带；换电脑时需确认 `libssl/libcrypto` 可用）；protobuf 只需一个 `CastMessage` 结构，可用 `protobuf-c` 或手写最小化编解码。

**验收**：能发现、连接、LAUNCH、LOAD、播放、STOP；心跳维持不断线。

### M6 · 菜单、对话框、状态

**菜单**：`src/AppMenu.h` 加 `IDM_CAST_TO_TV` 及设备子菜单 command 段；`src/AppMenu.cpp` File 菜单块加 `Cast to TV` 动态子菜单；`src/main.cpp:16005 HandleCommand` 加分支。菜单项在"已加载片源"时可用，否则灰掉。

**Cast 对话框**（推流期间主界面，照 `RunOwnedModalDialog` / `SettingsDialogChrome` 模式新建）：
- 目标设备名 + 连接状态（扫描中/连接中/推流中/失败）；
- 实时指标：推流帧率、码率、已推时长；
- 播放进度条（当前/总时长）——用现成 `Position()`，**只读不可拖**（避免 seek 联动复杂度）；
- 按钮：停止推流 / 切换设备 / 刷新扫描；
- 错误区：连不上/编码失败/切片失败原因。

**状态行**：`status_chips` 三块不动；在状态文字行加 `Cast → 客厅电视` / `Casting` / `Cast 失败` 段（照 `status_note` 机制）。

**验收**：菜单勾选进入推流、对话框实时更新、状态行提示正确。

### M7 · 生命周期与边界（grill 已定案）

| 事件 | 行为 |
|---|---|
| 三段开关 | 与推流解耦，随时可推（推的就是当前渲染画面） |
| 本地画面 | 进入投屏后停显，后台继续渲染供抓帧 |
| 本地快捷键 | 全部保留，推流跟随播放状态 |
| 暂停 / seek | 推流端持续推相同内容的分段（画面定格但电视仍在播，HLS live 无"暂停"状态）；seek 重新锚定分片 |
| 片尾 | 自动停止推流，对话框提示"播放结束" |
| 编码失败 / 电视断开 | 对话框报错、停推流，**后台渲染继续**；修好可重推 |
| 切换片源 | 停止推流，正常打开新片 |
| 关闭对话框 | 停止推流，恢复本地显示 |
| 推流中数据源切换 | 不发生——数据源唯一（渲染帧） |

---

## 6. 配置（INI）

`SettingsPath()` 指向的 INI，新增 `[Cast]` 段：

```ini
[Cast]
LastDevice=Sony X9500H        ; 上次选择的设备名
HttpPort=8080                  ; HLS HTTP 服务端口
SegmentSeconds=2               ; HLS 分段时长
PlaylistSize=5                 ; playlist 保留段数
VideoBitrate=8000000           ; 视频码率
AudioBitrate=128000            ; 音频码率
CaptureWidth=1920              ; 抓帧输出宽（0=跟随源）
CaptureHeight=1080             ; 抓帧输出高（0=跟随源）
CastAppId=CC1AD845             ; Default Media Receiver
```

读写用 `GetPrivateProfileStringW` / `WritePrivateProfileStringW`，与现有设置一致。

---

## 7. 换电脑实施前置（Windows 11 + RX 9070 XT）

来源：`docs/WIN11-DEVELOPMENT-SETUP-zh.md`、`docs/BUILDING.md`。

1. VS 2022 Build Tools + MSVC v143 14.44、Windows 11 SDK 10.0.26100、CMake ≥3.24、Git、PowerShell、Python；
2. FFmpeg/FFprobe 放到 `external/ffmpeg/bin`（HLS 切片、H264 编码都靠它）；
3. AMD runtime 资产：`LmxxfNrRuntime.dll` + shader + 权重 + gfx1201 HSACO + SHA256SUMS；
4. 驱动提供 `amdhip64_7.dll`（`Test-Path "$env:windir\System32\amdhip64_7.dll"`）；
5. **新增依赖**：OpenSSL（`libssl`/`libcrypto`，CastV2 的 TLS）——确认目标机器/构建环境可用，这是 POC 阶段就要查的；
6. 磁盘 ≥30GB，内存 16GB 最低 / 32GB 推荐。

**注意**：`NvencDirect` 及其 HEVC direct 路径只在 N 卡可用；RX 9070 XT 上投屏编码走 `h264_amf` 或 `libx264`（见 M2）。

---

## 8. 实施顺序

1. **POC（第 2 节）** —— 三处风险全过才继续；
2. M3 HTTP 服务 + M2 编码/HLS（先不接抓帧，用 testsrc 造流，端到端能播）；
3. M5 CastV2（先命令行工具验证，再 C++ 化）；
4. M4 mDNS 发现 + 设备选择；
5. M1 抓帧接入 + 本地停显；
6. M6 菜单/对话框/状态；
7. M7 边界行为补齐；
8. 全链路回归 + 长时间稳定性（≥1 小时播放）。

---

## 9. 风险与未决项

| 风险 | 影响 | 缓解 |
|---|---|---|
| **X9500H 不播 HLS live** | 整个方案作废 | POC Step 1；失败则转 DLNA（TS+H264+AC3） |
| **Cast 设备证书验证绕过失败** | 连不上电视 | POC Step 2；失败则研究 X9500H 的 DIAL/认证流程 |
| **H264 编码吞吐不足** | 掉帧、卡顿 | POC Step 3；AMF→libx264→降 720p |
| **CastV2 协议细节踩坑** | 工期不确定 | 有 node-castv2 / MediaX / libcast 三份参考；heartbeat/重连要健壮 |
| **HLS 固有延迟 6–10s** | 体验受限 | 调小 `hls_time`/`hls_list_size`；接受这个量级（看片够用） |
| **抓帧环 4 槽背压** | 编码慢会卡渲染 | 丢最旧帧而非等待；参考 `NvencDirect` 的 moreQueued 逻辑 |
| **OpenSSL 依赖** | 换电脑构建失败 | POC 阶段确认可用；必要时静态链接 |

**无悬空设计决策**——所有产品行为在 grill 中已定案（见第 5、7 节）。唯一的前置是 POC 验证三项技术风险。

---

## 10. 验证清单

- [ ] POC Step 1：X9500H 持续播放 HLS live ≥60s
- [ ] POC Step 2：H264 编码 1080p30 单帧 <20ms
- [ ] POC Step 3：端到端延迟可接受
- [ ] mDNS 扫到 X9500H，动态菜单可选、记住上次
- [ ] 抓帧 → 编码 → HLS → HTTP → Cast 全链路打通
- [ ] 三段开关任意组合下推流画面正确
- [ ] 暂停定格、seek 重锚、片尾自动停
- [ ] 失败报错但后台渲染继续，可重推
- [ ] 切片源停推、关对话框恢复本地显示
- [ ] 1 小时稳定性无内存增长、无卡死

---

## 11. 产品化实际状态更正（2026-11-21）

最终落地走的是 **DLNA**（非本文档的 Chromecast 路径）。第 10 节的
「抓帧 → 编码 → HLS → HTTP → Cast 全链路打通」**尚未完成**：

- 分层验证已证明数据面 OK：产品 ffmpeg 参数（h264_amf 1080p30/8000k）
  编码的切片，经产品 `CastHttpServer` 服务，电视全程 PLAYING
  （`tools/verification/verify_product_hls_on_tv.py --generate` = PASS）。
- 断点在**抓帧→喂编码器**：`main.cpp` 调 `D3D12Renderer::Initialize(...)`
  没传第 10 参 `captureOutput=true`（第 9 参是 `preserveSource`，易混），
  抓帧环从未创建，`SubmitFrame()` 从未被调用，编码器 stdin 无帧 →
  HLS 目录空 → 电视拿 404「当前内容不可播放」。
- 且现成抓帧环为神经渲染评估设计：抓 NGX `m_dlssOutput`（descriptor 1）、
  受 `m_lastDLSSUsed` 门控；AMD 上 DLSS 不可用，最终呈现帧是
  `m_upscaleSharp`/`m_dlssColor`。**不能只补 `true`**，要给投屏接一条
  抓「当前呈现帧」的独立路径。
- 详细证据/工具/命令见 `.workbuddy/memory/2026-11-21.md` 顶部 CORRECTION 与分层定位两节。

---

## 12. 抓帧接入方案（2026-11-21 定案，实现前）

### 12.1 勘察结论：现成机制比"补一个 true"更合适

RenderFrame 返回时 `m_upscaleSharp`（FP16、`m_outputW×m_outputH` = 输出 rung）
已填好本帧 FSR 4 结果——`DumpOutputFrame()` 紧跟 RenderFrame 调用就是证据。
AMD 路径 present 最终采样 `SRVGPU(UpscaleSRV)`（D3D12Renderer.cpp:3952-3954），
即 `m_upscaleSharp`，**尺寸正好是投屏编码器要的 1920×1080**。

现成 `EnqueueEvaluatedFrameCapture()` 的 4 槽异步环 + `m_psoCacheCapture`
（present shader 把 FP16 SRV 转 BGRA8 写 readback）**格式完全兼容** UpscaleSRV：
两个源都是 FP16、都是 m_outputW×m_outputH，present shader 不关心是哪张。
所以不必另造抓帧通路，只需给现有环加"抓哪张"的选择 + 放行门控。

被否决的两条路（避免再试）：
- 复用 `EnqueueComposedViewCapture`（VSR 导出那条）：它抓窗口 backbuffer 尺寸
  （`CurrentPresentTarget()` = 窗口大小，非 1920×1080），且要求 `target.scaled`
  和 `DebugView::Final`，与播放器任意窗口尺寸/调试视图不兼容。
- 只补 `Initialize(..., true, true)`：环开了但 `m_lastDLSSUsed` 在 AMD 恒 false，
  `EnqueueEvaluatedFrameCapture` 第 2 道闸仍 return，且抓 descriptor 1（NGX 输出，
  AMD 上无内容）→ 仍然无帧。

### 12.2 改动点（三处，全部默认行为不变）

1. **`D3D12Renderer.h/.cpp`**：`EnqueueEvaluatedFrameCapture` 加可选参数
   `CaptureSource source = CaptureSource::NeuralOutput`，枚举两值：
   - `NeuralOutput`（默认）：抓 descriptor 1，保留 `m_lastDLSSUsed` 门控，
     **评估/离线路径逐字不变**。
   - `PresentOutput`：抓 `UpscaleSRV`，门控改为 `UpscalePresentActive()`
     （AMD FSR4/FSR1/CAS 在-service），不碰 m_lastDLSSUsed。
   源切换只改 `SRVGPU(captured)` 的 descriptor 号（1 → UpscaleSRV）与门控，
   captured 仍是 FP16 FP16→BGRA8 的同一 present shader。
   若 `UpscalePresentActive()` 为 false（DLSS 真在用的 NVIDIA 机器），
   PresentOutput 回退抓 descriptor 1——那正是该类机器的最终呈现帧。

2. **`main.cpp`**：
   - `Initialize(..., m_activeQuality, true, true)`：第 10 参补 `captureOutput=true`
     （第 9 参 `preserveSource=true` 保持不变）。同时修正那句写错的注释。
   - `RenderVideoFrame` 的送帧段：投屏时用 `EnqueueEvaluatedFrameCapture(
     CaptureSource::PresentOutput)`；非投屏时用默认参数（评估语义不变）。
     drain/SubmitFrame 路径不变（已有 drainBase 硬闸）。

3. **`CMakeLists.txt`**：无新增源文件，不改。

### 12.3 不变量 / 回归门禁

- **回退链 FSR4 → FSR1 → CAS 不得破坏**：PresentOutput 只是读 UpscaleSRV，
  不写任何 present 状态、不改任何 PSO/资源；且 AMD 新支路统一
  `if (request.backend == Amd)` 语义（此处以 `UpscalePresentActive()` 判定）。
- **评估抓帧逐字不变**：默认参数走原路径，离线导出 A/B 不受影响。
- **UI 回归**：66 cases / 3160 assertions / **22 failures** 与基线逐项一致
  （同一份测试代码 A/B）。
- **真机验收（必须看到哪一行）**：投屏开始后 `%TEMP%\dlss5-cast-hls`
  的 `live.m3u8` 存在、`GET /live.m3u8` 返回 **200**（不是 404）、
  切片 `seg-*.ts` 持续增多、电视 PLAYING 且用户肉眼看到画面。
  **netstat 连接数不算数**（404 也建立连接，11-21 已误判一次）。

### 12.4 风险与降级

- 若 `captureOutput=true` 的常驻 readback 环内存（~40 MiB@1080p）在意，
  可后续改成投屏时才分配——本轮不做，先保证通路正确。
- 若 PresentOutput 抓到的是上一帧（时序问题）：DumpOutputFrame 已证明
  RenderFrame 返回时 m_upscaleSharp 是本帧的，风险低；真机以画面为准。

### 12.5 实现更正（2026-11-21 15:44，实测推翻 12.1 的假设）

**12.1 的致命假设错了**：实测诊断行
`Cast capture diag: captureOutput=1 output=1280x720 upscaleActive=0 dlssUsed=0
dlssEnabled=0 upscaleOffered=0 upscaleStretch=0` —— 投屏时 SR **默认没开**
（`m_upscalingRequested` 取决于 ini，AMD 上 `SetDLSS(false)` 关闭了 NGX 开关），
renderer 输出就是源尺寸 1280×720，`UpscaleSRV` 根本不存在（`upscaleStretch=0`）。
按 12.2 的 `UpscalePresentActive()` 门控会直接 return，仍然无帧（实测 20 次 404）。
而且即使抓到，源是 1280×720，编码器写死 1920×1080 = stride 不匹配。

**实际实现**（偏离 12.2，以本节为准）：
1. `CaptureSource` 保留，但 `PresentOutput` 的门控改为「只要 capture ring 在就抓」，
   无「SR 必须开」条件。
2. 抓帧源按 present 主路径的三档同款选择：NGX 在-service → descriptor 1；
   `UpscalePresentActive()` → `UpscaleSRV`；都没有 → descriptor 4（`m_dlssColor`
   解码源），并做 `GuideReadState ↔ PIXEL_SHADER_RESOURCE` 的成对 barrier。
   三档都是 FP16，同一 present shader 转 BGRA8，源小于输出时按 output viewport 放大。
3. `SelectCastDevice` 在 Start 前用 `m_renderer->OutputW()/OutputH()` 设置
   `m_cast.Config_().width/height`，让 ffmpeg `-video_size` 匹配抓帧输出。
   这是 12.2 漏掉的关键联动——编码器尺寸必须跟随 renderer，不能写死。
4. 主 renderer 的 3 处 `Initialize` 都补第 10 参 `captureOutput=true`
   （含 GPU 恢复重建路径，否则重建后投屏静默断流）。
5. 诊断行做成一次性 LOG（cast 路径专属），从此「没帧」能直接从日志判根因，
   不用再靠 netstat 猜。

**验证结果**：`verify_cast_capture.py --hold 30` → 15 次探测 14 次 OK，
m3u8 200、`seg-*.ts` 从 1 段增长到 12 段每段 2.17 MB，日志
`Cast encoder started: h264_amf 1280x720@30`；自检 27+18+21 全绿，
UI 回归 66/3160/22 与基线一致。电视真实画面待用户肉眼确认。

### 12.6 第二根因：URL 推送早于首片落盘（"内容无法播放"）
抓帧打通后 HLS 200 + 非空切片、电视来拉，但电视报"当前内容无法播放"。
隔离实验：把同一批产品切片用"等就绪再推 URL"的验证器推给电视 →
`PLAYING 20/20` + 肉眼出画面，证明切片本身没问题，差异在时序。

- 根因：`Start()` 里 encoder 刚启动就推 URL，电视立即 GET `/live.m3u8`
  撞上首片未落盘的 404，判定不可播放且不重试。
- 修法：`CastHttpServer::ServeOne`（独立线程）对 playlist GET 等
  `HlsReady(dir)`（m3u8 + 非空 seg，≤15s，stop-aware）。
  **不能在 `Start()` 里等**——它是 UI 线程，sleep 会停掉渲染送帧而自锁
  （实测 12s 全 404 + WinError 10061）。`HlsReady` 是纯函数，+5 单测。
- 验证：`verify_cast_capture.py --hold 30` → probe 00 即 200（无 404 竞态），
  01–14 全 OK；编译 EXIT=0，自检 32+18+21，UI 回归 66/3160/22 与基线一致。
- 详细证据见 `.workbuddy/memory/2026-11-21.md` 16:05 追加节。

### 12.7 功能开关重建 renderer 的抓帧环隐患（2026-11-21 下午）

**症状**：播放中开超分再投屏，电视拿不到画面。日志铁证：
```
不开超分  Cast capture diag: captureOutput=1 output=1280x720  upscaleActive=0   → HlsReady=1
开超分    Cast capture diag: captureOutput=0 output=2560x1440 upscaleActive=1   → HlsReady=0（HLS 空）
```

- **根因**：开超分走 `EnableUpscaling()` → 建候选 renderer → `m_renderer=std::move(candidate)`
  替换活动 renderer。候选的 `Initialize` 只传 9 参，**第 10 参 `captureOutput` 默认 false**
  → 新活动 renderer 的抓帧环没开 → `EnqueueEvaluatedFrameCapture` 首行 `if(!m_captureOutput)return false`
  → 无帧喂编码器。这是「能选但什么也不做」的又一例：参数存在但没接线。
- **同类隐患**：YouTube 切分辨率的候选 `Initialize` 也缺第 10 参。

**修复**（`src/main.cpp` 两处，各补第 10 参 `true`）：
- `EnableUpscaling()` 候选：`Initialize(..., MaxQuality, true, true)`（第 9 参 preserveSource 原值不动）
- `CreateRendererCandidate()`（YouTube）：`Initialize(..., quality, false, true)`

不能用 `m_cast.Casting()` 做门控——用户是「先开超分再投屏」，那一刻 `Casting()` 还是 false，
会漏掉这个场景。修后 5 处 renderer `Initialize`（10306/10495/10812/14959/15280）全部带
`captureOutput=true`。

**为什么不能只补参数就想当然**：抓帧选源三档是 NGX descriptor 1 / UpscaleSRV / 源 descriptor 4，
与 present switch（2311–2315）顺序一致。AMD 上 NGX 不可用（`m_lastDLSSUsed`/`DLSSEnabled` 恒 false），
实际走 UpscaleSRV（FSR4 超分 rung）——投屏抓的就是观众看到的那一档，所以补参后选源天然正确，
无需改抓帧逻辑。

**扩展排查：神经渲染与插帧是否也破坏投屏 → 都没有（真机实测 PASS）**：
- 静态：这两个开关**都不重建 renderer**（`ToggleNeuralRendering`→`StartLiveNeuralSession`
  在现有 renderer 上 attach；`TogglePlaybackFrameGeneration`→`SetPlaybackFrameGeneration` 只改标志），
  不会重现候选漏参 bug。
- 工具 `tools/verification/verify_cast_features.py`（`--feature neural|framegen`）：ini 预置功能开
  （避免触发时机问题）；功能必须在日志实证激活（`neural=live` / `framegen=on`）否则 INCONCLUSIVE；
  判据 = `captureOutput=1` + `HlsReady=1` + 切片增长 + DMR PLAYING + 无 start failure。
- 结果：插帧（与超分同时开）`framegen=on` 全程、切片 2→11、TV 15/15 PLAYING；
  神经渲染（live session）`neural=live` 全程、切片 1→13、TV 15/15 PLAYING。均 ALL=PASS。
- 两次方法论教训：① 功能类验证必须先确认功能真激活——`verify_cast_with_upscale.py` 首轮
  `upscaleActive=0` 说明 WM_COMMAND 发了但功能没开，那轮验的不是超分态；② DMR 状态单次采样会撞
  瞬时缓冲态（神经渲染首跑一次 `PAUSED_PLAYBACK`，恰逢 `re-anchoring`，重跑 15/15 PLAYING），
  状态判据要多次采样；③ 环境偶发（探活 `no-response`、瞬时缓冲）判 INCONCLUSIVE 而非 FAIL。

**验证总账**：编译 EXIT=0；cast 自检 32+18+21 全绿；端到端 `verify_cast_capture.py` ALL=PASS；
UI 停止 `verify_cast_stop.py` ALL=PASS（DMR 自报 PLAYING→STOPPED）；暂停 `verify_cast_pause.py`
修复前后切片 `6,6,6,6,6,6` → `7,8,9,10,11,12`。

