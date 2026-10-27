"""全片台词精确对齐 + 说话人识别（v3）。

v1 的问题：demucs 的人声轨在音乐段有残留，silencedetect(-35dB) 在整片里
检出一堆"语音"，于是 whisper 的 373 条幻觉又混了进来（66 段里 50 段是幻觉）。

v3 的做法：
  1. whisper 的段落按文本分组，**只取每种文本的第一次出现** —— 幻觉是同一句反复刷屏
  2. **边界完全由人声轨的能量决定**，不再相信 whisper 的时间戳也不再跑 silencedetect：
     在 whisper 粗范围 ±0.8s 内找最长的连续语音段，找不到才依次放宽到 ±2.5s / ±6s。
     whisper 的时间在这部片上实测能差 8 秒（#11 的窗口里全是 -38dB 房间噪声、
     #57 只切到 0.2 秒、#8 丢掉了前半句），这些窗口同时也是说话人嵌入和配时长
     预算的输入，所以时间错会同时表现为"音色不对"和"没地方说话"。
  3. 附近完全没有语音（整窗只有房间噪声）的句子判定为音乐段幻觉，直接丢弃
  4. 说话人：已知台词定参考，其余句与参考质心比余弦；比较余量小的句子在
     lines.json 里标 low confidence，供下游决定是否换参考

输出：lines.json（index/start/end/speaker/text）+ segNN.wav（每句人声切片）
"""
import json
import os
import pathlib
import re
import shutil
import subprocess
import warnings

import numpy as np
import soundfile as sf

warnings.filterwarnings("ignore")

REPO = pathlib.Path(os.environ.get("DUB_REPO", "F:/dlss5-video/dlss5-video-amd"))
ENGINE = pathlib.Path(os.environ.get("DUB_ENGINE", "F:/tmp/s2st"))
FFMPEG = os.environ.get("DUB_FFMPEG", str(REPO / "run-fg/ffmpeg.exe"))
ROOT = pathlib.Path(os.environ.get("DUB_ROOT", "F:/tmp/s2st"))
WORK = ROOT / "work"
WORK.mkdir(parents=True, exist_ok=True)
VOCALS = os.environ.get("DUB_VOCALS", str(ROOT / "stems_out/vocals.wav"))
TRANSCRIPT = os.environ.get("DUB_TRANSCRIPT", "F:/tmp/dub-verify/transcript.json")
# How close two lines have to be before they count as the same voice. Only used when
# the film has no known lines to anchor on -- see the clustering block below.
CLUSTER_THRESHOLD = float(os.environ.get("DUB_CLUSTER_THRESHOLD", "0.55"))

# Speech recognisers emit boilerplate when they meet music: "Transcription by
# CastingWords" is a credit, not a line. Cheap to match, and a dubbed credit is
# unmistakable in the finished file.
BOILERPLATE = re.compile(
    r"transcription by|subtitles? by|amara\.org|thanks for watching"
    r"|please subscribe|www\.|^\s*[*\[(].*[*\])]\s*$", re.I)

_symlink = pathlib.Path.symlink_to


def _copy(self, target, target_is_directories=False):
    try:
        _symlink(self, target, target_is_directories)
    except OSError:
        shutil.copy2(self, target)


pathlib.Path.symlink_to = _copy


def norm(text):
    return re.sub(r"[^a-z0-9 ]", "", text.lower()).strip()


# 16k 单声道人声轨
vox16k = WORK / "vox16k.wav"
subprocess.run([FFMPEG, "-v", "error", "-y", "-i", VOCALS, "-ac", "1", "-ar", "16000",
                "-c:a", "pcm_s16le", str(vox16k)], check=True)
vox, rate = sf.read(str(vox16k), always_2d=True)
vox = vox.mean(axis=1)

from spk import SpeakerEmbedder  # noqa: E402

# Same ECAPA-VoxCeleb weights as before, via ONNX Runtime instead of torch -- the
# graph is the one tools/export_ecapa_onnx.py produced and it is cosine-identical,
# so the thresholds below still mean what they were tuned to mean.
embedder = SpeakerEmbedder(os.environ.get("DUB_SPK_MODEL") or None)


def embed(samples, sr=16000):
    return embedder.embed(samples, sr)


def rms(samples):
    return float(np.sqrt(np.mean(samples ** 2))) if len(samples) else 0.0


def trim_silence(start, end, frame=0.02, floor=0.10):
    """Shrink a span to where the voice actually is.

    The union of speech spans opens and closes on room tone. Those edges are not
    part of the utterance, and a window padded with them is a window the
    translation is judged against for nothing.
    """
    step = int(frame * rate)
    a, b = int(start * rate), int(end * rate)
    if b - a < 3 * step:
        return start, end
    energies = [rms(vox[i:i + step]) for i in range(a, b - step, step)]
    peak = max(energies) if energies else 0.0
    if peak <= 0:
        return start, end
    first = next((i for i, e in enumerate(energies) if e >= peak * floor), 0)
    last = next((i for i, e in enumerate(reversed(energies)) if e >= peak * floor), 0)
    return round(start + first * frame, 3), round(end - last * frame, 3)


segments = json.load(open(TRANSCRIPT, encoding="utf-8"))["transcription"]

WHISPER = os.environ.get("DUB_WHISPER", str(REPO / "asr/runtime-gpu/whisper-cli.exe"))
WHISPER_MODEL = os.environ.get(
    "DUB_WHISPER_MODEL", str(REPO / "asr/models/ggml-large-v3-turbo-q5_0.bin"))
LANG = os.environ.get("DUB_LANG", "en")
ORIGINAL = os.environ.get("DUB_ORIGINAL", str(ROOT / "full44k.wav"))
RESPLIT_RATIO = float(os.environ.get("DUB_RESPLIT_RATIO", "1.8"))
# How much longer than the line's expected speaking time a union of runs may be before
# it counts as two lines rather than one with a pause in it. 1.8 leaves room for a
# dramatist's beat; 2.5 would start welding adjacent dialogue together.
MERGE_BUDGET = float(os.environ.get("DUB_MERGE_BUDGET", "1.8"))


def expected_seconds(text):
    """How long the line should take at film pace -- about 2.6 words per second."""
    return len(re.findall(r"[A-Za-z']+", text)) / 2.6 + 0.4


def transcribe_span(start, end):
    """Transcribe one slice of the original mix on its own, in absolute seconds."""
    clip = WORK / "refine.wav"
    stem = WORK / "refine"
    rows_path = pathlib.Path(str(stem) + ".json")
    rows_path.unlink(missing_ok=True)
    subprocess.run([FFMPEG, "-v", "error", "-y", "-ss", f"{start:.3f}", "-t", f"{end - start:.3f}",
                    "-i", ORIGINAL, "-ac", "1", "-ar", "16000", "-c:a", "pcm_s16le", str(clip)],
                   check=True, capture_output=True)
    subprocess.run([WHISPER, "-m", WHISPER_MODEL, "-f", str(clip), "-l", LANG, "-oj",
                    "-of", str(stem), "-t", os.environ.get("DUB_THREADS", "8")],
                   capture_output=True, text=True)
    if not rows_path.exists():
        return []
    rows = json.loads(rows_path.read_text(encoding="utf-8")).get("transcription", [])
    rows_path.unlink(missing_ok=True)
    clip.unlink(missing_ok=True)
    out = []
    for row in rows:
        text = row["text"].strip()
        if not text or text.startswith("[_"):
            continue
        out.append({"offsets": {"from": round(start * 1000) + row["offsets"]["from"],
                                "to": round(start * 1000) + row["offsets"]["to"]},
                    "text": text})
    return out


def resplit(segment):
    """Re-transcribe one span on its own so a merged sentence separates.

    whisper's rows are silence-delimited: it will happily return one row for
    "line + 13 s of room tone + line", stamped with only the first line's words.
    Whatever the row swallows is a line the rest of this script never sees -- no
    boundary, no speaker, no Chinese -- and the finished film plays that stretch
    with the score alone, which is heard as "the dub stopped here". Measured at
    5:10: the 303.80-321.48 row hides Tom's "Sorry. Sorry.".

    Cutting exactly that span out and transcribing the 18 s clip on its own gives
    "Sorry, sorry." at +0.0 s and "Look Celia, we have to follow our passions." at
    +12.4 s. Same audio, same model; only the amount of context changed.
    """
    start = segment["offsets"]["from"] / 1000.0
    end = segment["offsets"]["to"] / 1000.0
    return transcribe_span(start, end) or None


def word_overlap(source, piece):
    """Share of the original's words that survive in the re-transcribed row."""
    words = set(re.findall(r"[a-z']+", source.lower()))
    if not words:
        return 0.0
    return len(words & set(re.findall(r"[a-z']+", piece.lower()))) / len(words)


MERGED_SPANS = []
kept = []
for segment in segments:
    span = (segment["offsets"]["to"] - segment["offsets"]["from"]) / 1000.0
    if span > max(3.0, expected_seconds(segment["text"]) * RESPLIT_RATIO):
        rows = resplit(segment)
        # Only a split that actually separates the span is an improvement -- one row
        # back means the model read the clip the same way it read the film, and
        # replacing the original with it would just move the text around. On this film
        # that is exactly what the 30 s spans do: "I don't know." comes back as
        # "We'll be right back.", which is a music-section hallucination, not a line.
        if rows and len(rows) >= 2:
            # "And action!" + "Action!" -- a row swallowed by a longer one is the same
            # line counted twice, and dubbing it twice is audible. Identical rows are
            # the same problem in a different shape.
            texts = [row["text"].strip().lower() for row in rows]
            unique = []
            for row, text in zip(rows, texts):
                if any(text != other and text in other for other in texts):
                    continue
                if any(text == kept_row["text"].strip().lower() for kept_row in unique):
                    continue
                unique.append(row)
            best = max((word_overlap(segment["text"], row["text"]) for row in unique), default=0.0)
            if unique and best >= 0.5:
                print(f"  [resplit {span:4.1f}s -> {len(unique)} rows] {segment['text'].strip()[:44]}")
                for row in unique:
                    row["from_resplit"] = True
                    print(f"      {row['offsets']['from'] / 1000:7.2f}-{row['offsets']['to'] / 1000:7.2f}"
                          f"  {row['text'][:50]}")
                MERGED_SPANS.append(segment["text"].strip()[:40])
                kept += unique
                continue
    kept.append(segment)
segments = kept

# Kill the hallucination storm without killing genuine repeats.
#
# whisper, on a stretch with no speech in it, emits the *same sentence* over and over --
# measured on this film: "i dont know why youre in there" 50 times at 0.5-1.7 s spacing,
# "im sorry" 25 times at 2-3 s. So a repeated text used to be kept once and dropped after.
#
# That rule also ate the film's real repeats, which are 4-5 minutes apart: Tom reciting
# "Look, Celia, we have to follow our passions" again at 303 s (first said at 24 s), and
# three more lines with it. The dub went silent for ~35 s at 5:10, because every line in
# that scene had been deleted as a duplicate.
#
# The two cases separate cleanly on spacing: hallucinations pile up inside 1-35 s, real
# repeats are 276-317 s apart. Keeping the first occurrence per *time cluster* preserves
# both, and the speech-floor test below still removes clusters that land on silence.
REPEAT_GAP = 60.0
first = {}
last_seen = {}
for segment in segments:
    key = norm(segment["text"])
    if not key:
        continue
    at = segment["offsets"]["from"] / 1000.0
    if key not in last_seen or at - last_seen[key] > REPEAT_GAP:
        first[f"{key}#{at:.0f}"] = segment
        last_seen[key] = at
        continue
    last_seen[key] = at
print(f"unique first occurrences: {len(first)}")

# 已知说话人（沿用 diarize2.py 的 TRUTH）
TRUTH = [
    ("We have main engine start", "robot"),
    ("You're a jerk, Tom", "celia"),
    ("Look Celia", "tom"),
    ("You have your robotics", "tom"),
    ("Why don't you just admit", "celia"),
    ("I'm not freaked out by", "tom"),
    ("All right, fine", "tom"),
    ("I'm having nightmares", "tom"),
    ("Oh, whatever", "celia"),
    ("Robot's memory synced", "robot"),
    ("This is pretty freaky", "unknown"),
]


def truth_of(text):
    low = text.lower()
    for prefix, label in TRUTH:
        if low.startswith(prefix.lower()):
            return label
    return None


# --- where the voice actually is ---------------------------------------------------
# whisper's segment times are not trustworthy on this material. Measured on the film:
# #11's window holds -38 dB of room tone (no speech at all), #57 collapses to 0.2 s, #8
# starts after the first half of the sentence. Those windows are also what the speaker
# embedding and the fit budget are measured on, so a wrong window surfaces as the wrong
# voice and a line with no room to speak. The times are therefore re-derived from the
# vocal stem: take the speech run nearest whisper's estimate, widening the search only
# when the tight window has no speech in it.
FRAME = 0.02
# Only two steps of widening. A third step (±6 s) let a line claim the speech of the
# line next to it: measured, #52 and #53 both ended up on 367.1-368.4 while #55/#56 and
# #58/#59 landed on identical spans. Past ±2.5 s the honest answer is "whisper put this
# text where nobody is speaking", and the mix stage restores that stretch from the
# original vocal stem instead.
WIDEN = (0.8, 2.5)


def frame_energies(start, end, claimed=()):
    step = int(FRAME * rate)
    a, b = int(start * rate), int(end * rate)
    if b - a < step:
        return np.zeros(0), start
    count = (b - a) // step
    block = vox[a:a + count * step].reshape(count, step)
    energies = np.sqrt(np.mean(block ** 2, axis=1))
    # Speech already given to an earlier line is off limits: dialogue runs together and
    # the longest run in two overlapping windows is the same run.
    for lo, hi in claimed:
        i0 = max(0, int((lo - start) / FRAME))
        i1 = min(count, int(np.ceil((hi - start) / FRAME)))
        if i1 > i0:
            energies[i0:i1] = 0.0
    return energies, start


def speech_runs(start, end, floor, claimed=(), gap=0.35, relax=0.12):
    """Every unclaimed voiced run inside [start, end], breaths allowed inside a run."""
    energies, t0 = frame_energies(start, end, claimed)
    if not len(energies) or float(energies.max()) < floor:
        return []
    threshold = max(float(energies.max()) * relax, floor * 0.6)
    voiced = energies >= threshold
    max_gap = int(gap / FRAME)
    runs, i = [], 0
    while i < len(voiced):
        if not voiced[i]:
            i += 1
            continue
        j = last = i
        while j < len(voiced):
            if voiced[j]:
                last = j
            elif j - last > max_gap:
                break
            j += 1
        runs.append((t0 + i * FRAME, t0 + (last + 1) * FRAME))
        i = j
    return runs


def loud_enough(span, floor):
    """A run that is mostly a click or a scream tail is not a line."""
    return rms(vox[int(span[0] * rate):int(span[1] * rate)]) >= floor * 0.6


def locate(rough_start, rough_end, floor, claimed, budget=None):
    """The speech belonging to this line, or None when there is none within reach.

    One line can be two runs with a pause in the middle -- "Look, Celia." then, a beat
    later, "We have to follow our passions." -- and taking only the longer one truncated
    the line: the dub covered half of it and the rest played in English. Budget is how
    long the line should take to say, so a union that fits is one line and a union that
    does not is two; that keeps this from welding neighbouring lines together.
    """
    for pad in WIDEN:
        runs = [run for run in speech_runs(max(0.0, rough_start - pad), rough_end + pad, floor, claimed)
                if run[1] - run[0] >= 0.30]
        if not runs:
            continue
        if budget:
            union = (min(run[0] for run in runs), max(run[1] for run in runs))
            if union[1] - union[0] <= budget and loud_enough(union, floor):
                return union, pad
        best = max(runs, key=lambda run: run[1] - run[0])
        if loud_enough(best, floor):
            return best, pad
    return None, None


candidates = []
for key, segment in first.items():
    text = segment["text"].strip()
    candidates.append({"text": text,
                       "truth": truth_of(text),
                       "from_resplit": bool(segment.get("from_resplit")),
                       "rough_start": round(segment["offsets"]["from"] / 1000.0, 3),
                       "rough_end": round(segment["offsets"]["to"] / 1000.0, 3)})

# Speech floor: the median energy of a line window, 12 dB down. A window that is only
# room tone -- which is what a music-section hallucination usually gets -- sits below it.
rough_energies = [rms(vox[int(row["rough_start"] * rate):int(row["rough_end"] * rate)])
                  for row in candidates]
positive = [e for e in rough_energies if e > 0] or [0.0]
floor = float(np.median(positive)) * 0.25
print(f"speech floor: {floor:.4f} (median line {20 * np.log10(np.median(positive) + 1e-9):.1f} dBFS)")

# Locate in reading order so the earlier line of a run-together pair gets first pick.
candidates.sort(key=lambda row: row["rough_start"])
kept, claimed = [], []
for row in candidates:
    if BOILERPLATE.search(row["text"]):
        print(f"  [drop:credit] {row['text'][:44]}")
        continue
    budget = max(2.5, expected_seconds(row["text"]) * MERGE_BUDGET)
    span, pad = locate(row["rough_start"], row["rough_end"], floor, claimed, budget)
    if span is None:
        # A row that came out of a re-split has already been confirmed as speech once,
        # on a clip that contained nothing else. "Sorry, sorry." is why this branch
        # exists: the apology sits 10 dB under the score, demucs leaves almost none of
        # it in the vocal stem, so the energy test cannot see it -- and without this the
        # film plays 303.8-316.0 with no voice at all.
        if row.get("from_resplit"):
            start, end = row["rough_start"], row["rough_end"]
            claimed.append((start, end))
            row.update(start=round(start, 3), end=round(end, 3), quality="resplit")
            kept.append(row)
            print(f"  [resplit-kept] {row['text'][:44]}  "
                  f"({start:.2f}-{end:.2f}, voice below the stem's floor)")
            continue
        # whisper put this text where nobody is speaking. Dubbing it would put a Chinese
        # voice over a quiet scene; the mix restores the original voice for this stretch.
        print(f"  [drop:no-speech] {row['text'][:44]}")
        continue
    start, end = trim_silence(span[0], span[1])
    if end - start < 0.25:
        start, end = span
    quality = "span" if pad <= WIDEN[0] else f"moved{pad:g}s"
    if quality != "span":
        print(f"  [retimed:{pad:g}s] {row['text'][:40]}")
    claimed.append((start, end))
    row.update(start=round(start, 3), end=round(end, 3), quality=quality)
    kept.append(row)

candidates = sorted(kept, key=lambda row: row["start"])

vectors = {}
for position, row in enumerate(candidates):
    a, b = int(row["start"] * rate), int(row["end"] * rate)
    vectors[position] = embed(vox[a:b])

# 参考向量 = 已知句**逐句嵌入的质心**，不是"把这些句子拼成一条长音频再嵌入"。
# 拼接会把 10 秒的混合语句当成一个不同的发音人：实测这样算出来的参考与任何单句的
# 余弦只有 0.05，于是所有句子在两个人之间都分不开（余量 0.02 的句子有 24 条）。
# 逐句嵌入取质心后，同人余弦 0.5~0.8、异人 <0.16，阈值和这里才重新有意义。
ref_vectors = {}
for label in ("tom", "celia"):
    picks = [(position, row) for position, row in enumerate(candidates)
             if row["truth"] == label]
    stack = np.stack([vectors[p] for p, _ in picks if vectors.get(p) is not None])
    if not len(stack):
        continue
    centroid = stack.mean(axis=0)
    ref_vectors[label] = centroid / (np.linalg.norm(centroid) + 1e-9)
    chunks = [vox[int(row["start"] * rate):int(row["end"] * rate)] for _, row in picks]
    merged = np.concatenate(chunks)
    sf.write(WORK / f"ref_{label}.wav", merged, rate)
    print(f"ref {label}: {len(stack)} lines, {len(merged)/rate:.1f}s, "
          f"self-cos {float(np.mean([vectors[p] @ ref_vectors[label] for p, _ in picks])):.2f}")

# A film this pipeline has never seen carries no truth table, so labelling cannot
# start from "these lines are Tom". Greedy cosine clustering gives the distinct
# voices instead: take each line in order, attach it to the nearest running centroid
# if it is close enough, otherwise call it a new one. Order-dependent by nature, but
# 20 lines with a 0.55 threshold is far below the regime where that matters, and the
# alternative (k-means with an assumed count) needs the one number nobody can supply.
cluster_labels = {}
if not ref_vectors:
    centroids = []                       # [(sum of members, count, name)]
    for position in range(len(candidates)):
        vector = vectors.get(position)
        if vector is None:
            cluster_labels[position] = "unknown"
            continue
        best, best_score = None, 0.0
        for slot, (total, count, _) in enumerate(centroids):
            centroid = total / max(count, 1)
            score = float(vector @ (centroid / (np.linalg.norm(centroid) + 1e-9)))
            if score > best_score:
                best, best_score = slot, score
        if best is not None and best_score >= CLUSTER_THRESHOLD:
            total, count, name = centroids[best]
            centroids[best] = (total + vector, count + 1, name)
            cluster_labels[position] = name
        else:
            name = f"voice{len(centroids) + 1}"
            centroids.append((vector.copy(), 1, name))
            cluster_labels[position] = name
    # 只有一两句的簇没有足够素材做参考音频，并到台词最多的那个声线
    counts = {}
    for name in cluster_labels.values():
        counts[name] = counts.get(name, 0) + 1
    lead = max(counts, key=counts.get)
    for position, name in list(cluster_labels.items()):
        if counts.get(name, 0) <= 1 and name != "unknown":
            cluster_labels[position] = lead
    print("无真值 -> 聚类得到声线: " + ", ".join(
        f"{name}×{sum(1 for v in cluster_labels.values() if v == name)}"
        for name in sorted(set(cluster_labels.values()))))

lines = []
for index, row in enumerate(candidates):
    vector = vectors.get(index)
    label = row["truth"] or cluster_labels.get(index)
    scores = {}
    if vector is not None and ref_vectors:
        scores = {name: round(float(vector @ ref), 3) for name, ref in ref_vectors.items()}
    if label in (None, "unknown") and scores:
        label = max(scores, key=scores.get)
    label = label or "unknown"
    margin = (max(scores.values()) - min(scores.values())) if scores else 0.0
    confidence = "low" if scores and margin < 0.15 else "ok"
    lines.append({"index": index, "start": row["start"], "end": row["end"], "speaker": label,
                  "text": row["text"], "scores": scores, "margin": round(margin, 3),
                  "confidence": confidence, "quality": row.get("quality", "span"),
                  "rough_start": row.get("rough_start", row["start"]),
                  "rough_end": row.get("rough_end", row["end"])})
    print(f"  #{index:2d} {row['start']:8.2f}-{row['end']:8.2f} {label:7s} {confidence:3s} "
          f"m={margin:.2f} {row['text'][:44]}")

# 余弦在短句上没有分辨力：0.4~1.0 秒的感叹句绝对余弦只有 0.03~0.17，谁赢取决于噪声。
# 实测整场戏（232-248 秒）被拆成了 celia / unknown 两类，成片里就是"男人说话用女声"。
# 基频能否决掉这种情况：一整场里 165 Hz 以上完全没有周期性，就不可能是女声。
# 只在有真值表（知道每个标签是谁）时启用——没有真值时标签是聚类出来的，
# 无法知道哪一个是女声，此时乱改只会把两个声线搅在一起。
if ref_vectors:
    import voice

    changes = voice.repair_turns(lines, vox, rate, voices=tuple(ref_vectors))
    print(f"  基频否决：{len(changes)} 句改了声线"
          + ("" if not changes else "，见上面的 turn 行"))

json.dump(lines, open(WORK / "lines.json", "w", encoding="utf-8"), ensure_ascii=False, indent=1)

# 导出每句切片（44.1k，供 Granite / TTS 用）
full, full_rate = sf.read(VOCALS, always_2d=True)
full = full.mean(axis=1)
for index, row in enumerate(lines):
    a, b = int(row["start"] * full_rate), int(row["end"] * full_rate)
    sf.write(WORK / f"seg{index:02d}.wav", full[a:b], full_rate)
print(f"\nwrote {len(lines)} segments + lines.json")
