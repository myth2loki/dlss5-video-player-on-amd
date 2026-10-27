"""Fit every dubbed line into its window without a human in the loop.

Three things changed from the first pass, all of them measured rather than guessed:

1. The window is `next_start - start`, not `end - start`. A line that stops speaking at
   389.71 still owns the silence until 390.50, and that silence is what lets a Chinese
   line that needs 1.0 s fit a 0.41 s utterance. Capped at +1.5 s past the line's own
   end so a long gap later in the film cannot hand a line a huge window.

2. The draft translation is tried first. It is the human-reviewed cut, so it only gets
   replaced when it actually overruns -- a shorter line is not a better line.

3. When it does overrun, the translator is asked for a shorter spoken version under a
   character budget derived from the calibrated pace model, twice at most, then atempo
   as the last resort. A line still over after that is reported, never hidden.
"""

import json
import os
import pathlib
import re
import subprocess
import sys
import time
import urllib.request

import numpy as np
import soundfile as sf

import glossary
import subtitles

ROOT = pathlib.Path(os.environ.get("DUB_ROOT", "F:/tmp/s2st"))
ENGINE = pathlib.Path(os.environ.get("DUB_ENGINE", "F:/tmp/s2st"))
REPO = pathlib.Path(os.environ.get("DUB_REPO", "F:/dlss5-video/dlss5-video-amd"))
WORK = ROOT / "work"
DUB = ROOT / "dub_auto"
DUB.mkdir(exist_ok=True)

BIN = pathlib.Path(os.environ.get("DUB_TTS_BIN",
                                  str(next((ENGINE / "bin").glob("**/llama-tts.exe")))))
TTS_MODEL = pathlib.Path(os.environ.get("DUB_TTS_MODEL",
                                        str(ENGINE / "models/Qwen3-TTS-12Hz-1.7B-Base-Q4_K_M.gguf")))
TTS_MMPROJ = pathlib.Path(os.environ.get("DUB_TTS_MMPROJ",
                                         str(ENGINE / "models/mmproj-Qwen3-TTS-12Hz-1.7B-Base-Q8_0.gguf")))
FFMPEG = os.environ.get("DUB_FFMPEG", str(REPO / "run-fg/ffmpeg.exe"))

TR_SERVER = os.environ.get("DUB_TR_SERVER", str(REPO / "translation/runtime/llama-server.exe"))
TR_MODEL = os.environ.get("DUB_TR_MODEL", str(REPO / "translation/models/translategemma-4b-it-Q4_K_M.gguf"))
TR_PORT = int(os.environ.get("DUB_TR_PORT", "8133"))
TR_BASE = f"http://127.0.0.1:{TR_PORT}"

EXTEND = 1.5          # max seconds a line may borrow past its own end
SAFETY = 0.92         # aim slightly under the window
MAX_ROUNDS = 2
MAX_TEMPO = 1.25
# How far a line may be rushed before compressing it is the better answer. Kept equal
# to MAX_TEMPO on purpose: every overrun the speed-up can absorb is one the translator
# is not asked about, and the translator is what breaks the sentence when asked.
REWRITE_ABOVE = 1.25
MAX_SHIFT = 2.00      # ceiling on starting a line early when it does not fit
# A window under a second is an exclamation ("No!", "Whoa!"), and there the engine's own
# variance is wider than the window: the pace model promised 0.54 s for 不！ and the synth
# produced 0.96 s. Rushing an exclamation is what a person would do and half a second of
# speech carries it, so those get a higher ceiling than a spoken clause does.
SHORT_WINDOW = 1.20
MAX_TEMPO_SHORT = 1.50
# `usable_window` floors at 0.5 s, which for these already spends part of the next line's
# turn. Where that floor is what binds, let the line finish the sentence rather than clip
# it -- 0.25 s of overlap in a barrage of shouting is not what the ear picks out.
OVERLAP = 0.25
COMMAS = "，、"
ENDERS = "。？！…"

# Names come back transliterated inconsistently ("Celia" -> 凯莉 in one pass, 塞莉亚 in
# another). The table now lives in glossary.json so the translator is told the same
# spellings this stage enforces -- it used to be patched here only, which is why the
# subtitle file kept 凯莉 on three lines while the voice said 塞莉亚.
GLOSSARY = glossary.load()

pace = json.load(open(WORK / "pace_model.json", encoding="utf-8"))
PER_CHAR, PER_COMMA, BASE = pace["per_char"], pace["per_comma"], pace["base"]
EFFECTIVE = 1.0 / pace["effective_per_second"]

rows = json.load(open(WORK / "lines.json", encoding="utf-8"))
translations = {row["index"]: row for row in json.load(open(WORK / "zh_final.json", encoding="utf-8"))}
# Hand corrections, keyed by line number. Re-translating rewrites zh_final.json, so a fix
# made there is lost on the next pass of the translator; keeping it here means it survives
# every rerun and is visible as a deliberate edit rather than a lucky sample.
OVERRIDES = WORK / "zh_overrides.json"
overrides = json.load(open(OVERRIDES, encoding="utf-8")) if OVERRIDES.exists() else {}
refs = json.load(open(WORK / "refs_auto.json", encoding="utf-8"))


def features(text):
    chars = len(re.sub(f"[{COMMAS}{ENDERS}]", "", text))
    commas = sum(text.count(c) for c in COMMAS)
    return chars, commas


def predict(text):
    chars, commas = features(text)
    return PER_CHAR * chars + PER_COMMA * commas + BASE


def budget_chars(window):
    """Characters *including* punctuation that fit the window.

    The per-character and per-comma costs are folded into one effective rate because the
    translator controls both at once and cannot be asked to balance them.
    """
    chars = (window * SAFETY - BASE) / EFFECTIVE
    return max(4, int(chars))


def text_len(text):
    return len(re.sub(r"\s", "", text))


def gloss(text):
    return glossary.fix(text, GLOSSARY)


def rewrite_strict(source, window, limit):
    """Ask for a line under the limit, and insist when the model overshoots.

    The model treats the budget as advice: given "at most 4 characters" it produced 7,
    longer than the draft it was meant to shorten. It also truncates under a tight budget
    ("塞莉亚，我们该追" with no ending), so a result without sentence-final punctuation is
    treated as broken rather than used -- a cut-off line reads as a lost word to the ear.

    Returns the shortest valid rewrite, or an empty string when every attempt was broken.
    """
    best = rewrite(source, window, limit)
    if not (best and best.rstrip()[-1] in ENDERS):
        best = ""
    for _ in range(2):
        if best and text_len(best) <= limit:
            break
        limit = max(3, int(limit * 0.78))
        again = rewrite(source, window, limit)
        if again and again.rstrip()[-1] in ENDERS:
            if not best or text_len(again) < text_len(best):
                best = again
    return best


def dropped(position):
    """Whether this line will not be spoken at all."""
    index = rows[position]["index"]
    entry = translations.get(index, {})
    if entry.get("skip") or not (entry.get("zh") or "").strip():
        return True
    if str(index) in overrides:
        value = overrides[str(index)]
        return value is None or not str(value).strip()
    return False


def usable_window(position):
    # The room runs to the next line that is actually spoken, not to the next
    # row: dropping a line has to lengthen its neighbour's window, or the
    # neighbour stays squeezed against a sentence nobody says.
    row = rows[position]
    base = row["end"] - row["start"]
    span = base + 2.0
    for later in range(position + 1, len(rows)):
        if not dropped(later):
            span = rows[later]["start"] - row["start"]
            break
    return max(0.5, min(span - 0.05, base + EXTEND))


def header():
    return ("You are a professional English (en) to Chinese (zh-Hans) translator. Your goal is to "
            "accurately convey the meaning and nuances of the original English text while adhering to "
            "Chinese grammar, vocabulary, and cultural sensitivities.\n")


def budget_prompt(text, window, limit):
    return header() + (
        f"This line is for dubbing: the Chinese has to be spoken in {window:.1f} seconds, so it may "
        f"be at most {limit} characters including punctuation. Write it as spoken Chinese an actor "
        f"can read in one breath, keeping the meaning and the tone. Output only the line, without "
        f"quotes or commentary.\n\n\n{text}")


def clean(raw):
    cut = raw.find("<end_of_turn>")
    if cut >= 0:
        raw = raw[:cut]
    raw = raw.strip()
    while len(raw) >= 2 and raw[0] == '"' and raw[-1] == '"':
        raw = raw[1:-1].strip()
    raw = re.sub(r"^\d+[\.、]\s*", "", raw)
    return raw.strip()


def rewrite(source, window, limit):
    body = json.dumps({
        "messages": [{"role": "user", "content": budget_prompt(source, window, limit)}],
        "temperature": 0.3, "max_tokens": 160, "stream": False,
    }).encode()
    request = urllib.request.Request(TR_BASE + "/v1/chat/completions", body,
                                     {"Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=120) as response:
        payload = json.load(response)
    return clean(payload["choices"][0]["message"]["content"])


def trim_silence(path):
    """Cut the dead air the engine pads a line with, and say what is left.

    The duration it returns is what the fit budgets against, so padding is not a
    cosmetic problem: 哇！ came back 1.68 s long for one syllable, and the fit
    then spent the whole window rushing silence -- which the mixer answered by
    cutting the line short instead. A quarter of a second of room, measured at
    -48 dBFS, is kept at both ends so no consonant is clipped.
    """
    audio, rate = sf.read(str(path), always_2d=True)
    frame = max(1, int(0.01 * rate))
    count = len(audio) // frame
    if not count:
        return len(audio) / rate
    mono = audio.mean(axis=1)
    energy = np.sqrt((mono[:count * frame].reshape(count, frame).astype(np.float64) ** 2).mean(axis=1))
    loud = np.nonzero(20 * np.log10(energy + 1e-9) > -48.0)[0]
    if not len(loud):
        return len(audio) / rate
    margin = int(0.04 * rate)
    start = max(0, loud[0] * frame - margin)
    end = min(len(audio), (loud[-1] + 1) * frame + margin)
    if end - start < len(audio):
        audio = audio[start:end]
        sf.write(str(path), audio, rate)
    return len(audio) / rate


def synth(text, ref, path):
    result = subprocess.run(
        [str(BIN), "-m", str(TTS_MODEL), "--mmproj", str(TTS_MMPROJ), "-ngl", "99",
         "--tts-lang", "zh", "--tts-speaker-file", ref, "-p", text,
         "--output", str(path), "-n", "900", "--temp", "0.7"],
        capture_output=True, text=True, encoding="utf-8", errors="replace")
    if result.returncode != 0 or not path.exists():
        return None
    return trim_silence(path)


def atempo(src, dst, ratio):
    subprocess.run([FFMPEG, "-v", "error", "-y", "-i", str(src),
                    "-filter:a", f"atempo={ratio:.3f}", str(dst)], check=True)


def main():
    server = subprocess.Popen(
        [TR_SERVER, "-m", TR_MODEL, "-c", "2048", "-ngl", "99", "--port", str(TR_PORT),
         "--no-jinja", "--chat-template", "gemma"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    out = []
    try:
        deadline = time.time() + 240
        ready = False
        while time.time() < deadline:
            try:
                with urllib.request.urlopen(TR_BASE + "/health", timeout=2) as response:
                    if response.status == 200:
                        ready = True
                        break
            except Exception:
                time.sleep(1.0)
        if not ready:
            print("FAIL: translator never answered /health")
            sys.exit(1)

        for position, row in enumerate(rows):
            index = row["index"]
            entry = translations.get(index, {})
            if entry.get("skip") or not (entry.get("zh") or "").strip():
                continue
            if str(index) in overrides:
                replacement = overrides[str(index)]
                # A null in the override file means "leave this line unsaid".
                # The transcriber put two sentences on one burst of speech at
                # 377.3 s, and dubbing both is what cuts the longer one in half
                # to make room for its own echo.
                if replacement is None or not str(replacement).strip():
                    continue
                entry = dict(entry, zh=replacement)
            speaker = row["speaker"]
            ref = refs.get(speaker, {}).get("path")
            if not ref:
                print(f"  #{index:2d} no reference for {speaker}, skipped")
                continue

            window = usable_window(position)
            source = entry.get("en_whisper") or row["text"]
            draft = entry["zh"].strip()
            text, stage = gloss(draft), "draft"

            path = DUB / f"zh{index:02d}.wav"

            # What this line could reach if it fought for every second: its window, the
            # silence the previous speaker left, and the tempo ceiling. The pace model is
            # only trusted in the hopeless direction -- its mean error is 0.236 s, so
            # "3.0 s into a 0.50 s window" is a fact while "1.1 s into a 1.0 s window" is
            # a guess. #17 used to be synthesised in full, measured, shortened, and
            # synthesised again for a line the model could already see would not fit.
            taken = [rows[position - 1]["end"]] if position else []
            if out and out[-1].get("seconds"):
                taken.append(out[-1]["anchor"] + out[-1]["seconds"])
            room = max(0.0, row["start"] - max(taken, default=0.0) - 0.15)
            ceiling = MAX_TEMPO_SHORT if window < SHORT_WINDOW else MAX_TEMPO
            reachable = (window + min(room, MAX_SHIFT)) * ceiling
            predicted = predict(text)
            rounds = 0
            if predicted > reachable + pace["mae"]:
                chars, _ = features(text)
                scaled = max(4, int(chars * (window / predicted) * 0.90))
                candidate = rewrite_strict(source, window, scaled)
                if candidate and text_len(candidate) < text_len(text):
                    text, stage = gloss(candidate), "precheck"
                    rounds = 1
                    print(f"   #{index:2d} predicted {predicted:.2f}s > reachable "
                          f"{reachable:.2f}s, rewritten before synthesis")

            seconds = synth(text, ref, path)

            # Rushing before rewriting, in that order. A line that overruns its window
            # by a fifth can simply be said a fifth faster; asking the translator to
            # compress it costs meaning, and on this film it turned "follow our passions"
            # into 我们必须追随心 -- shorter, and no longer a sentence. Rewriting is for
            # what rushing cannot reach.
            while seconds and seconds > window * REWRITE_ABOVE and rounds < MAX_ROUNDS:
                chars, _ = features(text)
                scaled = max(4, int(chars * (window / seconds) * 0.90))
                candidate = rewrite_strict(source, window, scaled)
                if not candidate or text_len(candidate) >= text_len(text):
                    break
                text, stage = gloss(candidate), f"budget-{rounds + 1}"
                seconds = synth(text, ref, path)
                rounds += 1

            # Borrow the silence first, speed the voice second. The order used to be the
            # other way round, which contradicted its own reasoning: a line nudged into the
            # gap the previous speaker left is inaudible, another 1.2x on the voice is not.
            # Whisper's timestamps run late as well, so the silence in front is usually the
            # room the original sentence actually had.
            anchor = row["start"]
            shifted = 0.0
            if seconds and seconds > window:
                # `room` and `ceiling` were computed before synthesis for the pre-check;
                # they are the same quantities, so they are not measured twice.
                shift = min(seconds - window, room, MAX_SHIFT)
                if shift > 0.01:
                    anchor -= shift
                    shifted = shift

            tempo = 1.0
            if seconds and seconds > window + shifted:
                tempo = min(seconds / (window + shifted), ceiling)
                if tempo > 1.02:
                    stretched = DUB / f"zh{index:02d}_t.wav"
                    atempo(path, stretched, tempo)
                    stretched.replace(path)
                    seconds = seconds / tempo

            room_total = window + shifted + (OVERLAP if window <= 0.55 else 0.0)
            ratio = (seconds / room_total) if seconds else float("nan")
            out.append({"index": index, "speaker": speaker, "window": round(window, 2),
                        "anchor": round(anchor, 3), "shift": round(shifted, 3),
                        "start": row["start"], "end": row["end"], "source": source,
                        "draft": draft, "text": text, "stage": stage,
                        "seconds": round(seconds, 2) if seconds else None,
                        "tempo": round(tempo, 3), "ratio": round(ratio, 3),
                        "needs_human": bool(seconds and seconds > room_total * 1.02)})
            mark = "!" if seconds and seconds > room_total else " "
            print(f"{mark}#{index:2d} [{speaker:6s}] 窗口{window:5.2f}s {stage:9s} "
                  f"合成{seconds or 0:5.2f}s x{tempo:.2f} 移{shifted:.2f} ratio{ratio:5.2f}  {text[:26]}")
            json.dump(out, open(WORK / "fit_auto.json", "w", encoding="utf-8"),
                      ensure_ascii=False, indent=1)
    finally:
        server.terminate()
        try:
            server.wait(timeout=10)
        except subprocess.TimeoutExpired:
            server.kill()

    kept = sum(1 for r in out if r["stage"] == "draft")
    squeezed = sum(1 for r in out if r["stage"].startswith("budget"))
    stretched = sum(1 for r in out if r["tempo"] > 1.02)
    over = [r for r in out if r["needs_human"]]
    print(f"\n=== {len(out)} 句 ===")
    print(f"  初稿直接用 {kept}  重写压缩 {squeezed}  额外 atempo {stretched}")
    print(f"  仍超窗 {len(over)}" + (": " + ", ".join(
        f"#{r['index']}({r['ratio']:.2f})" for r in over) if over else ""))
    if out:
        ratios = [r["ratio"] for r in out if r["seconds"]]
        print(f"  ratio 均值 {np.mean(ratios):.2f} 最大 {np.max(ratios):.2f}")

    # The subtitles are written here, from these rows and from nothing else: same
    # `text` the engine spoke, same `anchor` it was moved to, same `seconds` left
    # after any tempo change. The subtitle file and the voice cannot disagree
    # because there is only one file to disagree from.
    video = os.environ.get("DUB_INPUT")
    width, height = (subtitles.probe_size(video, pathlib.Path(FFMPEG).with_name("ffprobe.exe"))
                     if video and pathlib.Path(video).exists() else (1920, 1080))
    target = ROOT / "subtitles.ass"
    built = subtitles.write(out, target, width, height)
    squeezed = sum(1 for start, end, _ in built if end - start < subtitles.MIN_DUR)
    overlong = sum(1 for _, _, text in built for row in subtitles.wrap(text)
                   if subtitles._units(row) > subtitles.LINE_UNITS)
    print(f"\n=== 字幕 {target.name} ===")
    print(f"  {len(built)} 条 · {width}x{height} · 字号 {max(12, round(height * 0.046))}")
    print(f"  短于 {subtitles.MIN_DUR:.1f}s {squeezed} 条（被下一句挤压）· "
          f"超宽行 {overlong} 行（超过 {subtitles.LINE_UNITS} 单位会折行）")
    if len(built) != len(out):
        print(f"  !!! {len(out) - len(built)} 句没有对应字幕，检查 seconds 是否为空")


if __name__ == "__main__":
    main()
