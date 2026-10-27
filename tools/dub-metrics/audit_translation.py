"""Check a translated line against its English source without asking a human.

Three kinds of error are checkable by machine and are the ones a viewer notices
first: a number that changed, a negation that vanished, a name that drifted. All
three survive any amount of "does it read well", so they belong in a gate rather
than in a review.

Operates on what was actually spoken (fit_auto.json.text) and on what the subtitle
file shows. Those used to come from different files and disagreed on 13 of 71
lines; the fit stage now writes the subtitle from the rows it spoke, so the check
is a guard against that wiring coming apart rather than a measurement of it.
"""
import json
import os
import re
from pathlib import Path

JOB = Path(os.environ.get("DUB_JOB", "F:/tmp/s2st/jobs/t5"))
WORK = JOB / "work"
load = lambda n: json.load(open(WORK / n, encoding="utf-8"))

lines = {r["index"]: r for r in load("lines.json")}
fit = {r["index"]: r for r in load("fit_auto.json")}

CN_DIGIT = {"0": "零", "1": "一", "2": "二", "3": "三", "4": "四", "5": "五",
            "6": "六", "7": "七", "8": "八", "9": "九", "10": "十",
            "11": "十一", "12": "十二", "20": "二十", "30": "三十",
            "100": "一百", "1000": "一千"}
NEG_EN = re.compile(r"\b(not|no|never|nobody|nothing|none|nor|neither|without)\b|n't\b", re.I)
NEG_CN = re.compile(r"[不没别未无非莫勿甭缺乏]|不要|没有")
Q_EN = re.compile(r"\?\s*$")
Q_CN = re.compile(r"[吗呢嘛？]|什么|怎么|为什么|哪|谁|多少|是不是|要不要")

# Names the film actually uses; the mapping is the glossary the chain lacks.
# People only: "robot" would fire on 机械手 and 机器, both of which are fine.
NAMES = {"tom": "汤姆", "celia": "塞莉亚", "barley": "巴利"}

problems = []


def note(index, kind, detail):
    problems.append((index, kind, detail))


for index, f in sorted(fit.items()):
    en = (lines[index]["text"] or "").strip()
    zh = (f.get("text") or "").strip()
    if not zh:
        continue

    # 1. numbers
    for token in re.findall(r"\b\d{1,4}\b", en):
        want = CN_DIGIT.get(token, token)
        if token in zh or want in zh:
            continue
        # "90%" is as often 九成 as 百分之九十 in spoken Chinese, and only the first
        # one fits a two-second window. A round percentage written as tenths is the
        # same number, not a lost one.
        if "%" in en and len(token) == 2 and token.endswith("0"):
            tenths = CN_DIGIT.get(str(int(token) // 10), "")
            if tenths and f"{tenths}成" in zh:
                continue
        note(index, "number", f"源「{token}」→ 译「{zh}」")

    # 2. negation
    if NEG_EN.search(en) and not NEG_CN.search(zh):
        note(index, "negation", f"源「{en}」→ 译「{zh}」")

    # 3. question
    if Q_EN.search(en) and not Q_CN.search(zh):
        note(index, "question", f"源「{en}」→ 译「{zh}」")

    # 4. name drift: a known name in the source must land on its agreed form
    for latin, chinese in NAMES.items():
        if re.search(rf"\b{latin}\b", en, re.I):
            others = [v for k, v in NAMES.items() if k != latin and v in zh]
            if chinese not in zh and not others:
                note(index, "name-missing", f"{latin} 未落到「{chinese}」→「{zh}」")
            elif others:
                note(index, "name-wrong", f"{latin} 应作「{chinese}」，译作「{others[0]}」")

    # 5. how much of the source survived: characters against English words
    words = len(re.findall(r"[A-Za-z']+", en))
    if words >= 4 and len(zh) < words * 0.6:
        note(index, "short", f"{words} 词 → {len(zh)} 字「{zh}」")

kinds = {}
for _, kind, _ in problems:
    kinds[kind] = kinds.get(kind, 0) + 1
print(f"lines audited: {len(fit)}")
print(f"problems     : {len(problems)}  {kinds}")
print()
for kind in ("name-wrong", "name-missing", "number", "negation", "question", "short"):
    hits = [p for p in problems if p[1] == kind]
    if not hits:
        continue
    print(f"--- {kind} ({len(hits)})")
    for index, _, detail in hits[:8]:
        print(f"  #{index:<3} {detail[:110]}")

print()
# The two files are now derived from one another - the fit stage writes the
# subtitle from the rows it spoke - so this is a guard against that wiring
# coming apart, not a measurement of it. It used to compare the voice against
# `zh_final.json` and find 13 disagreements, none of which mattered to a viewer
# until the subtitle file existed.
subtitle_file = JOB / "subtitles.ass"
shown = []
if subtitle_file.exists():
    for line in subtitle_file.read_text(encoding="utf-8").splitlines():
        if line.startswith("Dialogue:"):
            shown.append(line.split(",", 9)[9].strip().replace("\\N", ""))
spoken = [(i, (fit[i].get("text") or "").strip()) for i in sorted(fit)
          if (fit[i].get("text") or "").strip() and fit[i].get("seconds")]
print(f"subtitle cues / spoken lines: {len(shown)} / {len(spoken)}")
diff = [i for (i, text), cue in zip(spoken, shown) if text != cue]
if len(shown) != len(spoken):
    print(f"  !!! {abs(len(shown) - len(spoken))} lines have no counterpart")
print(f"subtitle text differs from the voice: {len(diff)}/{len(spoken)}"
      + (": " + ", ".join(f"#{i}" for i in diff) if diff else ""))

print()
print("=== the names on screen ===")
wrong = 0
for (index, _), cue in zip(spoken, shown):
    en = (lines[index]["text"] or "").strip()
    for latin, chinese in NAMES.items():
        if re.search(rf"\b{latin}\b", en, re.I) and chinese not in cue:
            others = [v for k, v in NAMES.items() if k != latin and v in cue]
            wrong += 1
            printed = f"，这里写作「{others[0]}」" if others else ""
            print(f"  #{index:<3} {latin} 应作「{chinese}」{printed}→「{cue}」")
print(f"name drift on screen: {wrong} line(s)")
