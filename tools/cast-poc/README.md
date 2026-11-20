# Chromecast POC 工具包

对应方案第 2 节（硬性前置）与第 11 节 grill 定案。**这三处风险任何一处不成立，产品 C++ 工作作废**，所以先用现成工具验证，再动产品代码。

判据全部是可证伪的硬阈值，不靠"看起来不卡"。每条判决都能追到原始 JSONL / ffmpeg 输出行。

## 目录

| 文件 | 验什么 | 产物 |
|---|---|---|
| `r3_encode_bench.py` | R3 编码吞吐 | `POC R3 JUDGE PASS/FAIL` |
| `serve_live.sh` | HLS live 源（H.264 + AAC） | 无限 live 流 + HTTP 8080 |
| `serve_hls.py` | 带 CORS / Range 的静态服务 | 供 CAF receiver 跨域取 m3u8 与分段 |
| `caf-sample.jsonl` | 真实 CAF v3 报文样本 | LOAD + MEDIA_STATUS 序列，写 C++ 报文的对照 |
| `cast_sender.js` | R2 CastV2 协议栈 | `trace.jsonl`（逐条 MEDIA_STATUS） |
| `judge_r1.py` | R1 播放连续性判据 | `POC-R1 JUDGE PASS/FAIL` |
| `mock_receiver.js` | R2 协议链离线自检（无需电视） | 假接收端，主动推进播放头 |
| `run_offline_check.sh` | 双臂自检（健康/冻结） | `OFFLINE-SELFCHECK PASS/FAIL` |

## 零、离线自检（纯 PC，真机前先跑）

```bash
cd tools/cast-poc
bash run_offline_check.sh        # 约 155 s：两臂各 75 s 观察窗
```
**必须看到这一行**：
```
OFFLINE-SELFCHECK PASS verdicts_disagree=ok chain=validated
```
健康臂 mock 每秒推进播放头 → `POC-R1 JUDGE PASS`；冻结臂 `--freeze-after 10` → `POC-R1 JUDGE FAIL reason=playhead_not_advancing`。两臂判决必须相反，否则说明门槛是摆设。这一步验证的是**我们这侧的协议链**（CastV2 信封字段 / LAUNCH→transportId / 心跳 / LOAD 的 LIVE 参数 / MEDIA_STATUS→JSONL→判决），**不验证索尼 CAF 行为**——后者只能在 X9500H 上确认。用 `openssl` 生成一次自签证书缓存在 `.mock-cert/`；不在 PATH 时用 `OPENSSL_BIN` 指定。

## 一、mattebox 播放层验证（纯 PC，无需电视；验 R1 的 HLS/CAF 侧）

跑的是 Google **真实 CAF v3 框架**（不是仿造），因此 HLS/LIVE 处理逻辑与真机同源。方案原文写的 `npx @mattebox/cast-playground` 裸跑会失败——它**必须带 receiver URL 参数**。已验证可用的命令：

```bash
# 终端 A：HLS 源（先起）
bash tools/cast-poc/serve_live.sh        # 打印 http://<PC局域网IP>:8080/live.m3u8

# 终端 B：mattebox（公开 receiver 页，加载真实 CAF v3 框架）
npx --yes @mattebox/cast-playground@0.1.2 https://cast.mattebox.ch/
# 打印 Cast playground http://localhost:8010/ ，浏览器打开它
```

本机实测：`cast.mattebox.ch` 返回 HTTP 200、含 `caf_receiver/v3/cast_receiver_framework.js`；playground 起在 8010、代理在 8011。浏览器用 Edge 即可。在 playground 页面：Content URL 填 `http://<PC局域网IP>:8080/live.m3u8`，streamType 选 **LIVE**，autoplay 开，Send。

**本机实测已通过（2026-11-20）**：CORS 修复后经 sender UI 发 LOAD，Messages 面板拿到完整 `LOAD` + `MEDIA_STATUS` 序列（存于 `caf-sample.jsonl`）；画面 `PLAYING Media session 2`、testsrc2 彩条持续滚动、出现 LIVE 专属的 Go live 按钮；播放头 20 s 窗口从 73.3 s 单调推进到 99.5 s、无 ERROR。LOAD 的 `contentType=application/vnd.apple.mpegurl`、`streamType=LIVE` 与第 11.2/11.6 节定案一致。注意 Messages 面板报文**无逐条毫秒时间戳**，喂不了 `judge_r1.py`（它要 `t_ms` 格式）；该样本用于 C++ 报文字段对照，判决仍靠真机 `cast_sender.js` 的 trace。

若用自动加载，把 receiver URL 传给 playground（**单引号**包住，否则 `&` 被 shell 截断）：
```bash
npx --yes @mattebox/cast-playground@0.1.2 'https://cast.mattebox.ch/?load=http://<PC局域网IP>:8080/live.m3u8&type=application/x-mpegURL&live'
```

**必须看到**：画面持续播放 ≥ 60 秒不卡；Messages 面板能看到 LOAD / MEDIA_STATUS 的 JSON 原文（存一份作 C++ 报文对照样本）。这一步覆盖 R1 的 HLS/CAF 侧大半，但**模拟不了索尼 CAF 厂商差异**，最后仍要真机。

> **不要**直接在浏览器开 `https://cast.mattebox.ch/?load=http://<PC局域网IP>:8080/...`：那是 HTTPS 页面取 HTTP 资源，会被混合内容策略拦截。必须经 playground 的 HTTP 代理（8011）。实测报错是 `ERROR reason=network/NETWORK_FAILED customData.mattebox.fatal=true`，很容易误判成 HLS 源坏了。

## 三步流程

### R3 — 编码吞吐（纯 PC，无需电视，现在就能跑）

```bash
python3 tools/cast-poc/r3_encode_bench.py
```

**必须看到这一行**：
```
POC R3 JUDGE  PASS  encoder=h264_amf frame_ms=<x> threshold_ms=20
```
`frame_ms` 是单帧墙钟耗时，门槛 20 ms。本机实测 `h264_amf` ≈ 2.6 ms/帧（speed 12.7x），`libx264 ultrafast` ≈ 1.9 ms/帧，均达标。`h264_nvenc` 在无 N 卡的机器上记为 `SKIP`，不算失败。

若真机上 AMF 不达标：退化 `libx264 ultrafast`，实测 CPU 占用；仍不达标则把目标降到 720p30。

### R1/R2 — HLS live 能否被 Cast 播放 + CastV2 直连

依赖：Node.js + `npm i castv2@0.1.10`（在 `tools/cast-poc/` 下）。**不要装 `mdns`**——它依赖 node-gyp 原生编译，Node 22 下会失败；POC 阶段直接手工指定电视 IP，排除 mDNS 变量。

终端 A —— 起 HLS 源：
```bash
bash tools/cast-poc/serve_live.sh
```
启动后打印 `Stream URL: http://<PC局域网IP>:8080/live.m3u8`。把这个 IP 填进下一步。

终端 B —— 发送端（换成电视实际 IP）：
```bash
cd tools/cast-poc
node cast_sender.js --host 192.168.1.50 --url http://192.168.1.10:8080/live.m3u8 --seconds 75 --out trace.jsonl
```

`cast_sender.js` 会做：`TLS:8009`（跳过证书验证）→ 平台 channel `CONNECT` → 心跳 PING/5s → `LAUNCH CC1AD845` → 从 `RECEIVER_STATUS` 取 `transportId` → 媒体 channel `CONNECT` → `LOAD`（`contentType=application/x-mpegURL`、`streamType=LIVE`）→ 把每条 `MEDIA_STATUS` 写成 JSONL。

终端 C —— 判决（sender 跑完再执行）：
```bash
python3 tools/cast-poc/judge_r1.py tools/cast-poc/trace.jsonl
```

**必须看到这一行**：
```
POC-R1 JUDGE PASS first_frame_ms=<x> window_s=60.0 net_advance_s=<y> max_stall_s=<z> samples=<n>
```

四道硬阈值（第 11.6 节定案）：

| 判据 | 阈值 | 失败含义 |
|---|---|---|
| 首帧延迟 | ≤ 12000 ms | 电视迟迟不出画面 |
| 观察窗口 | ≥ 55 s | 采样不足，不能判 |
| 播放头净前进 | ≥ 45 s | 只播首个分段 / 卡住 |
| 播放头停滞 | ≤ 3 s | 缓冲耗尽 |
| ERROR 状态 | 无 | 格式错误 / LOAD 失败 |

若电视报格式错误或只播首个分段 → **转 DLNA 方案**（保留方案文档作参考）。

### 判决器自测（确认门槛不是摆设）

```bash
python3 tools/cast-poc/judge_r1.py --selftest
```
必须看到 `SELFTEST PASS verdicts_disagree=ok`——即"正常流"判 PASS、"10 秒后冻结播放头"判 FAIL。一个永远不会 FAIL 的门槛没有意义。

## 常见坑（实现前先记下）

- **`ERROR network/NETWORK_FAILED`（fatal）但 HLS 源是好的**：receiver 页来自 `http://localhost:8011`，取 `http://<PC_IP>:8080/live.m3u8` 是**跨域**。原 `python3 -m http.server` 不发 `Access-Control-Allow-Origin`，浏览器拦掉 m3u8 和分段的 XHR，CAF 统一报成 network fatal（`ready: receiver context started` 出现后才报错，说明框架本身没问题）。已改用 `serve_hls.py`（CORS + Range）。自查命令：
  ```bash
  curl -sS -D - -o /dev/null -H "Origin: http://localhost:8011" \
    http://localhost:8080/live.m3u8 | grep -i access-control-allow-origin
  # 必须看到 Access-Control-Allow-Origin: *
  ```
  live 窗口同时从 5 段（10 s）加到 12 段（24 s），避免 CAF 框架/MediaSource 启动期间最早分段被删。
- **`mdns` 装不上**：真因不是 Node 22 / nan，而是 `mdns@2.7.2` 绑定 Apple Bonjour SDK，编译缺 `dns_sd.h`（且运行期要 `dnssd.dll`）。POC 用手工 IP 绕过；产品化自动发现改 `npm i bonjour-service`（实测可装、`find({type:'googlecast'})` 可用，`svc.txt.id` 即 CastV2 的 destinationId），别修 node-gyp。
- **`h264_nvenc` 显示 0 帧**：AMD 机器上没有 NVENC 设备，是 `SKIP` 不是 `FAIL`。
- **静音视频**：方案原命令只有视频轨；这里加了 440 Hz AAC，因为第 11.2 节定音频为必须项。
- **`streamType` 必须是 `LIVE`**：否则 CAF 把滑动 playlist 当有限 VOD，播完就停。
- **首帧延迟**：HLS live 固有延迟 = 分段时长 × playlist 长度（典型 6–10 s）；12 s 门槛已含余量，超了就是真问题。
- **真机差异**：索尼 CAF 厂商行为模拟不了。R2 用 libcast 模拟能全验，R1 的"索尼认不认 LIVE"必须在 X9500H 上最后确认。
