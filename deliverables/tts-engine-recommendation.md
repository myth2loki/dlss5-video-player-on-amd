# TTS 引擎推荐（AMD RX 9070 XT · Windows · 离线）

> 日期：2026-10-27　前提约束：Windows + AMD + 离线 + 可随播放器分发
> 所有数字为本机实测（24 线程 CPU / RX 9070 XT / onnxruntime 1.24.4 DirectML）

---

## 0. 结论

**推荐 Piper（ONNX Runtime）**，理由三句话：

1. **它让"GPU 能不能跑"这个问题变得不重要**——CPU 上 2.88 秒音频只要 **0.086 秒**（RTF 0.03），2 小时片子约 **90 秒**。DirectML 也能跑，但只快 15%。
2. **许可证干净**：代码 MIT；模型 63 MB；不需要 PyTorch、不需要 CUDA、不需要 ROCm。
3. **唯一代价是固定音色，不支持克隆原演员。**

如果"克隆原演员"是硬需求，那么**目前无解**：F5 是唯一效果达标的克隆模型，但它在 CPU 上 RTF **17.5×**（2 小时片 14 小时），且 GPU 路径在 Windows+AMD 上不存在。

---

## 1. 实测数据

| 引擎 | 运行时 | 设备 | 2.88 s 音频耗时 | RTF | 2 h 片（语音占 40%） |
|---|---|---|---|---|---|
| **Piper**（zh_CN-huayan-medium, 63 MB） | ONNX Runtime 1.24.4 | **CPU** | **0.086 s** | **0.03×** | **约 90 秒** |
| Piper | ONNX Runtime + **DirectML** | **9070 XT** | 0.075 s | 0.03× | 约 80 秒（**仅快 15%**） |
| F5-TTS（v1_base, 1.35 GB） | PyTorch 2.14.1 | CPU | 43.0 s | **17.5×** | **约 14 小时** |
| F5-TTS | PyTorch + DirectML | 9070 XT | — | — | **不可用**（四层不兼容） |

**Piper 比 F5 快约 580 倍。** 这不是优化差异，是模型类别差异：F5 是扩散模型（32 步 ODE + 大 transformer），Piper 是小 VITS。

**关键判断**：在这个量级上，**GPU 对 Piper 已经不重要**（15% 提升不值得引入 DirectML 的依赖复杂度）。这反而是 AMD 平台最舒服的结论。

---

## 2. 为什么不是别的

| 候选 | 结论 | 原因 |
|---|---|---|
| **F5-TTS** | 效果最好，但不可交付 | CPU 14 小时；DirectML 四层不兼容（见验证文档 §5.5）；权重许可待核实 |
| **XTTS-v2** | 淘汰 | `speed` 伤音色（人耳判"仓鼠音"）；CPML 非商用 |
| **CosyVoice2** | 装不上 | 文本前端依赖 `pynini`，Windows 无 wheel |
| **Kokoro-82M** | 中文跑不起来 | Apache-2.0、有 8 个普通话音色（zf_\*/zm_\*），但 kokoro-onnx 的 espeak 后端在本机连 `en` 都加载失败 |
| **SeamlessM4T** | 不可用 | CC-BY-NC；2.3B 参数；GPU 路径不存在 |

---

## 3. 代价：固定音色，没有克隆

Piper 的音色是训练时定的（`zh_CN-huayan-medium` 是女声）。**不能克隆原演员**，所以：

| 你的需求 | Piper 能不能满足 |
|---|---|
| 整片中文配音，能听、能交付 | ✅ |
| 音色随角色变化（D2 的"每个角色一个音色"） | ❌（需要 diarization + 克隆，Piper 只提供有限几个音色） |
| 像原演员的声音 | ❌ |

**这是一个产品决策，不是技术决策**：要"能上线的配音"就得接受固定音色；要"像原演员"就得接受 14 小时或换平台。

---

## 4. 换了 TTS 之后，瓶颈换了人

| 环节 | 2 h 片耗时 | 说明 |
|---|---|---|
| whisper 转写 | 约 4 min | 项目自带，GPU |
| 翻译 | 约 3 min | 项目自带，GPU |
| **人声分离（demucs）** | **约 89 min** | **新瓶颈**，PyTorch CPU（0.74× 实时） |
| diarization（ECAPA） | 约 2 min | CPU 即可 |
| **Piper TTS** | **约 1.5 min** | 已经不是问题 |
| 混音 + mux | 约 1 min | ffmpeg |
| **合计** | **约 100 min** | 其中 89 分钟是分离 |

**结论**：真正的成本大头变成了**人声分离**。而分离是"背景音保留"（D1）才需要的——
如果接受"纯配音轨 + 原声轨保留"（用户在播放器里切音轨），**整条链路可以压到约 10 分钟**。

这条正好对上方案里早就写下的可推翻项（D1 的退路），现在它从"省 3-5 人天"升级为"省 89 分钟/片"。

---

## 5. 许可证状态（发布前必须核）

| 组件 | 许可 | 状态 |
|---|---|---|
| Piper 代码（`piper-tts`） | **MIT** | ✅ 可分发 |
| `zh_CN-huayan-medium` 音色 | 待核实（rhasspy/piper-voices 各音色许可不同） | ⚠️ **发布前必须查这个音色的 LICENSE** |
| ONNX Runtime | MIT | ✅ |
| F5-TTS 权重 | 待核实（有商用限制） | ❌ 不建议分发 |

**注意**：Piper 的"代码 MIT"不等于"音色可商用"。音色文件的许可要单独确认，这是发布前的一个卡点。

---

## 6. 建议的下一步

1. **先听 `ab_piper_vs_f5.mp3`**（英文原句 → Piper 中文 → F5 中文）。Piper 的品质能不能接受，决定这条路走不走。
2. 能接受 → 按 Piper 重写方案：**范围收敛到"字幕 + 固定音色配音"**，砍掉 diarization 与克隆（-8~12 人天），并决定是否保留人声分离。
3. 不能接受 → 回到"只做字幕"，配音留到有能在 AMD 上跑 GPU 的克隆模型时再做。

**工期修订（按 Piper + 不克隆 + 不做分离）**：约 **4–6 人天**（不含选型验证），因为：
- 分离、diarization、克隆音色、逐句时长拟合（Piper 时长可控）全部可以不做
- 剩下的是：Piper 接入（ONNX 会话 + 音色选择）、翻译字数预算、混音/mux 加第二条音轨、UI 一行控件

---

## 7. 复现方式

```powershell
# 环境（已验证）
pip install onnxruntime-directml    # 提供 DmlExecutionProvider
pip install piper-tts               # 注意：会装回 CPU 版 onnxruntime，之后需 force-reinstall onnxruntime-directml
# 音色
curl -L -o zh_CN-huayan-medium.onnx      https://hf-mirror.com/rhasspy/piper-voices/resolve/main/zh/zh_CN/huayan/medium/zh_CN-huayan-medium.onnx
curl -L -o zh_CN-huayan-medium.onnx.json https://hf-mirror.com/rhasspy/piper-voices/resolve/main/zh/zh_CN/huayan/medium/zh_CN-huayan-medium.onnx.json
```

脚本：`F:\tmp\dub-verify\piper_dml_test.py`（含 DirectML 注入与 CPU/GPU 对比）

---

## 8. 追加实测（2026-10-27 晚）：Kokoro 与 DirectML 的真实边界

用户反馈"Piper 不行"，要求换一个能用 GPU 的。逐个试完，结论比上一版更硬：

| 引擎 | 质量 | 中文 | **DirectML（9070 XT）** | CPU 耗时（2 h 片） | 许可 |
|---|---|---|---|---|---|
| **Piper** | VITS，一般 | ✅ | ✅ **可用**（0.075 s / 2.88 s 音频） | 约 **90 秒** | MIT（音色待核实） |
| **Kokoro-82M** | 明显更好 | ✅ 8 个普通话音色 | ❌ **原生崩溃（segfault）**，第一个会话就挂 | 约 **8 分钟**（RTF 0.172） | **Apache-2.0** |
| **F5-TTS** | 最好，可克隆 | ✅ | ❌ 四层不兼容 | 约 **14 小时** | 待核实 |
| **XTTS-v2** | 好，可克隆 | ✅ | ❌ | 约 7 s/句 | CPML 非商用 |
| **CosyVoice2** | 好，可克隆 | ✅ 母语级 | **未试**（HF 有社区 ONNX 导出） | 未知 | **Apache-2.0** |

**Kokoro 的失败方式特别值得记录**：fp32 和 fp16 版本抛的是 `E_INVALIDARG (80070057)`，
来自 `MLOperatorAuthorImpl.cpp`——DirectML 的算子 author 拒绝了图里的某个算子；
量化版本（q8f16 / quantized / uint8）则**直接段错误**，进程没机会捕获。
在播放器里这意味着**用户的播放器直接崩掉**，比"慢"严重得多。

**Kokoro 是怎么跑起来的**：`kokoro-onnx` 的 espeak 后端在本机连 `en` 都加载失败
（自带 `espeak-ng-data/voices` 目录残缺）。Kokoro 官方前端本来是 **misaki**，所以绕过 espeak：
`misaki.zh.ZHG2P()` 出带声调的音素 → 用 `kokoro_onnx/config.json` 的词表转 token →
直接喂 ONNX（输入只有 `tokens` / `style` / `speed`）。这条路 **CPU 上完全可用**。

### 8.1 现在的真实选择

| 需求 | 能做到的方案 | 代价 |
|---|---|---|
| **GPU + 可交付** | **只有 Piper** | 音质是 VITS 级，你不接受 |
| **质量更好 + 可交付** | **Kokoro（CPU）** | 2 h 片约 8 分钟；不用 GPU |
| **克隆原演员 + GPU** | **目前不存在** | — |
| 克隆 + CPU | F5 | 2 h 片 14 小时，不可交付 |

**建议**：把"GPU"从需求里去掉——Kokoro 在 CPU 上 2 小时片只要 **8 分钟**，
比整条链路里的**人声分离（89 分钟）**便宜一个数量级。GPU 在这里不是瓶颈，
真正的时间大头一直是分离。

如果要继续追 GPU，唯一还没试、且值得试的是 **CosyVoice2 的 ONNX 导出**（Apache-2.0、中文母语、可克隆）。
风险明摆着：Kokoro 在同一条 DirectML 上直接崩，CosyVoice2 的图更复杂，可能同样崩。
建议做成**时间盒 1 天的 spike**，判死标准很简单：**创建 DML 会话时崩或抛 E_INVALIDARG，立即停**。

### 8.2 音质对照

`ab_engines.mp3`：英文原句 → Piper → Kokoro（zf_xiaobei）→ F5。
Kokoro 明显比 Piper 自然，F5 最好但代价是 14 小时。

---

## 9. 定型（2026-10-27）：Kokoro，CPU 跑

用户听过后确认"这个版本听起来挺好的"，引擎就此定下：

| 项 | 选择 |
|---|---|
| 引擎 | **Kokoro-82M**（Apache-2.0，ONNX Runtime） |
| 设备 | **CPU**（DirectML 会在创建会话时段错误，见 §8，不带进产品） |
| 中文前端 | **misaki `ZHG2P`**（不用 espeak：`kokoro-onnx` 自带的数据残缺） |
| 音色 | 8 个普通话（`zf_xiaobei/xiaoni/xiaoxiao/xiaoyi`、`zm_yunjian/yunxi/yunxia/yunyang`） |
| 克隆 | **不支持** —— diarization + 克隆音色整条链路随之取消 |
| 模型体积 | 325 MB（fp32）/ 92 MB（quantized，未在 DML 测通，CPU 可用） |

### 9.1 整段实测（7 句，两个角色两个音色）

| 句 | 角色 | 音色 | 窗口 | 语速 | 时长 | 占窗口 |
|---|---|---|---|---|---|---|
| 1 | Celia | zf_xiaobei | 14.00 s | ×1.00 | 3.35 s | 24% |
| 2 | Tom | zm_yunjian | 3.00 s | ×1.15 | 2.98 s | 99% |
| 3 | Tom | zm_yunjian | 3.00 s | ×1.20 | 3.70 s | **123%** |
| 4 | Celia | zf_xiaobei | 4.00 s | ×1.00 | 3.85 s | 96% |
| 5 | Tom | zm_yunjian | 3.00 s | ×1.00 | 2.80 s | 93% |
| 7 | Tom | zm_yunjian | 3.00 s | ×1.20 | 3.10 s | **103%** |
| 8 | Celia | zf_xiaobei | 8.00 s | ×1.00 | 3.98 s | 50% |

- **Kokoro 的 `speed` 温和且不升调**：同一句 1.00/1.10/1.20 → 4.12/3.95/3.70 s，
  F0 104/107/109 Hz、谱质心 3647/3643/3613 Hz。**没有 XTTS 那种"仓鼠音"风险。**
- **5/7 装进窗口**，剩下 2 句（123%、103%）需要更进一步的译文压缩，或允许 ≤1.25 语速。
- 合成算力：单次约 **0.17× 实时**；带重试的整段实测 **0.48×**。
  2 小时片子按单次算约 **8 分钟**，按带重试算约 **23 分钟**。
- 专名表验证有效：模型把 Celia 重译成"凯莉"，按专名表纠回"塞莉亚"。

### 9.2 产物

- `dubbed_preview_kokoro.mp3` —— 70 秒整段成品（中文铺在分离后的背景上，两个角色两个音色）
- `ab_montage_kokoro.mp3` —— 逐句「英文原句 → Kokoro 中文」

### 9.3 引擎结论表（终版）

| 引擎 | GPU | 质量 | 克隆 | 许可 | 结论 |
|---|---|---|---|---|---|
| **Kokoro** | ❌（DML 崩） | ✅ 好 | ❌ | Apache-2.0 | **选定** |
| Piper | ✅ | 一般 | ❌ | MIT | 质量不达标 |
| F5-TTS | ❌ | 最好 | ✅ | 待核实 | 14 小时，不可交付 |
| XTTS-v2 | ❌ | 好 | ✅ | CPML | 淘汰 |
| CosyVoice2 | 未试 | 好 | ✅ | Apache-2.0 | 唯一可追的 GPU+克隆候选，时间盒 1 天 |

---

## 10. XTTS-v2 重测（2026-10-27 晚）：克隆与语速第一次同时成立

上一轮 XTTS 被判"淘汰"的理由是**语速**（`speed=1.15` 人耳判"仓鼠音"）。但那次的失败是三件事叠加的，
现在三条都修掉了：

| 当时的问题 | 现在怎么做 |
|---|---|
| 用 `speed` 提速 → 走 `length_scale` 重排发音 → 音色变紧 | **合成恒 `speed=1.0`**，溢出用外部 `atempo`（保发音，只压时间） |
| 译文没压字数 → 长句必然溢出 | 先用**字数预算**压译文 |
| 默认采样 → 同一句两次时长不同 | `do_sample=False` + 固定种子 |

### 10.1 整段实测（7 句，两个角色各自克隆）

| 句 | 角色 | 窗口 | 自然时长 | 外部 atempo | 结果 |
|---|---|---|---|---|---|
| 1 | Celia | 14.00 s | 2.31 s | ×1.00 | 17% |
| 2 | Tom | 3.00 s | 2.04 s | ×1.00 | 68% |
| 3 | Tom | 3.00 s | 2.97 s | ×1.00 | 99% |
| 4 | Celia | 4.00 s | 1.94 s | ×1.00 | 49% |
| 5 | Tom | 3.00 s | 1.58 s | ×1.00 | 53% |
| 7 | Tom | 3.00 s | 3.24 s | **×1.08** | 100% |
| 8 | Celia | 8.00 s | 3.16 s | ×1.00 | 39% |

**7/7 装进窗口，6 句连时间拉伸都不需要。**

### 10.2 克隆指标没有退化

| 指标 | 数值 |
|---|---|
| 同人 cosine（生成中文 vs 本人参考） | **+0.30** |
| 异人 cosine（vs 另一位） | +0.03 |
| 分离度 | **+0.27** |
| 判到错人 | **0 / 7** |

和上一轮（0.32 / 0.05）一致——**说明语速是外部处理的，没有伤到音色**。

### 10.3 成本

| 指标 | 实测 |
|---|---|
| CPU RTF | **1.87×**（5.7 s 算力 / 2.97 s 音频） |
| 2 小时片（语音 40%） | **约 90 分钟** |
| 对比 F5 | 14 小时（慢 9 倍） |
| 对比 Kokoro | 8–23 分钟（快 4–10 倍，但不克隆） |

### 10.4 选型定版（按"要不要像原演员"分）

| 引擎 | 克隆 | 2 h 片 | 许可 | 适用 |
|---|---|---|---|---|
| **XTTS-v2** | ✅ 0.30 | **90 分钟** | **CPML（非商用）** | 要"像原演员"且能接受非商用条款 |
| Kokoro | ❌ | 8–23 分钟 | Apache-2.0 | 要可商用、接受换人 |
| Piper | ❌ | 90 秒 | MIT（音色待核实） | 只要快 |
| F5-TTS | ✅ | 14 小时 | 待核实 | 只适合短片段 |

**唯一剩下的门禁是许可**：XTTS-v2 的权重是 **CPML（Coqui Public Model License）**，
**禁止商业用途**。这个播放器是 MIT 开源、免费分发，非商用一般成立，但"分发带 CPML 权重的附件"
需要按 CPML 条款附许可证并保留归属。**这一条要你来拍**，因为涉及发布方式。

### 10.5 产物

- `dubbed_preview_xtts2.mp3` —— 整段成品（克隆音色、铺在分离后的背景上）
- `ab_montage_xtts2.mp3` —— 逐句「英文原句 → 克隆出的中文」

---

## 11. 定案（2026-10-27）：XTTS-v2，克隆

用户确认 §10 的整段成品可用。**引擎选定 XTTS-v2**，替代 §9 的 Kokoro。
差别只有一条：**Kokoro 换人配音，XTTS 克隆原演员**——这是用户不接受 Kokoro 的全部原因。

| 项 | 决定 |
|---|---|
| 引擎 | **XTTS-v2**（`coqui/XTTS-v2`，1.87 GB，PyTorch CPU） |
| 克隆方式 | 零样本：每个角色 8–10 秒参考音频 + 参考文本，无需训练 |
| 合成参数 | `speed=1.0` 恒定、`do_sample=False`、固定种子 |
| 语速处理 | 溢出只用**外部 `atempo`**，上限 1.20（内部 `speed` 会伤音色） |
| 分角色 | 需要 diarization 或人工指定，每角色一段参考音频 |
| 成本 | CPU RTF **1.87×** → 2 小时片约 **90 分钟** |
| **未决** | **CPML 非商用许可**——发布方式需用户拍板 |

### 11.1 关于"音频转音频"（用户 2026-10-27 提问）

| 类型 | 现有实现 | 9070 XT 可用性 |
|---|---|---|
| **真 S2ST**（音频→翻译后音频） | SeamlessM4T v2 / SeamlessExpressive | ❌ **华为镜像与 HF 上均无 ONNX 导出**（实测搜索为空）；PyTorch 2.3B → CPU 不可用；许可 CC-BY-NC |
| **语音转换 VC**（换音色，不翻译） | RVC 系列，**有 ONNX**（`ozada/onnx_rvc`、`ohnoitsaninja/rvc-base-onnx` 等） | ⚠️ 理论上可跑 DirectML，但**每个音色要单独训练**（10 分钟素材 + 数十分钟训练），不是零样本 |
| **零样本克隆 TTS** | XTTS-v2 / F5 / GPT-SoVITS | ✅ XTTS 已验证（本机 CPU，90 分钟/片） |

**结论**：不存在"给一段音频就输出翻译后音频"且能在 Windows+AMD 上跑的实现。
XTTS-v2 的"10 秒参考音频 → 克隆中文配音"是当前最接近的形态，已定为本项目引擎。
真正的 S2ST 需要等 ONNX 移植，或等 Windows+AMD 有可用的 PyTorch GPU 后端——那是独立工程。

---

## 12. 许可决定与落地义务（2026-10-27：保持开源）

用户决定：**项目保持免费开源**。据此 CPML 的"非商用"条件成立，XTTS-v2 可以进产品。
原文已取回并入库：`THIRD_PARTY_LICENSES/coqui-XTTS-CPML.txt`（4014 字节，CPML 1.0.0）。

### 12.1 三条必须履行的义务（引自原文，不是概括）

| # | 义务 | 原文关键句 |
|---|---|---|
| 1 | **只做非商用**，模型与**其输出**都算 | "This license allows only **non-commercial use** of a machine learning model **and its outputs**." |
| 2 | **拿到模型或输出的人，必须同时拿到条款或 URL** | "You must ensure that anyone who gets a copy of any part of the model, or any modification of the model, **or their output**, from you also gets a copy of these terms **or the URL**." |
| 3 | 不得再授权、专利主张即失效 | "do not allow you to sublicense or transfer" / "Patent Defense" |

### 12.2 落地动作（进发布清单）

1. `THIRD_PARTY_LICENSES/coqui-XTTS-CPML.txt` 随 runtime 附件一起分发（已入库）。
2. 配音引擎的 opt-in 附件里带同一份文件 + `https://coqui.ai/cpml.txt`。
3. **输出也要带**：这是容易被忽略的一条——配音产出的音视频文件属于"model output"，
   所以 mux 时写入 `-metadata comment="Dubbed with XTTS-v2 (CPML 1.0.0, non-commercial) - https://coqui.ai/cpml.txt"`，
   让拿到文件的人能追溯到条款。
4. UI 提示一行：**配音输出不得用于商业用途**（对话框 tip + `README.txt`）。
5. 版本说明里写明：**若将来转商业发行，必须换引擎**——这条要写进项目的已知约束，避免以后踩。

### 12.3 这条决定的长期影响

| 场景 | 是否允许 |
|---|---|
| 个人使用、开源免费分发播放器 | ✅ 允许 |
| 把配音文件发到视频网站（无收益） | ✅ 允许（属非商用输出） |
| 用配音成果接商单、带货、付费课程 | ❌ **违反 CPML** |
| 把播放器做成付费版/商业发行 | ❌ **必须换引擎**（候选：Kokoro Apache-2.0，但换人配音） |

**给用户的实话**：这个许可不是"开源就行"，它管到**输出的用途**。个人玩没问题，
但如果将来有人拿这个功能接活赚钱，那是他们违反许可，而我们必须在文档和 UI 上明确告知——
否则出事时我们说不清。
