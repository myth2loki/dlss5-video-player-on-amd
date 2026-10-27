"""Check a translated line against its English source without asking a human.

Three kinds of error are checkable by machine and are the ones a viewer notices
first: a number that changed, a negation that vanished, a name that drifted. All
three survive any amount of "does it read well", so they belong in a gate rather
than in a review.

Operates on what was actually spoken (fit_auto.json.text), because that is the
line the audience hears - the stored translation is a different string on 13 of
71 lines.
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
stored = {r["index"]: (r["zh"] or "") for r in load("zh_final.json")}

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
        if token not in zh and want not in zh:
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
# The other half of translation accuracy: the subtitle and the voice disagree.
diff = [i for i in fit if stored.get(i, "").strip() != (fit[i].get("text") or "").strip()]
print(f"spoken text differs from the stored translation: {len(diff)}/{len(fit)}")
print("  " + ", ".join(f"#{i}" for i in diff))

print()
print("=== the stored translation (what a subtitle would show) ===")
wrong = 0
for index, raw in sorted(stored.items()):
    en = (lines[index]["text"] or "").strip()
    zh = (raw or "").strip()
    if not zh:
        continue
    for latin, chinese in NAMES.items():
        if re.search(rf"\b{latin}\b", en, re.I) and chinese not in zh:
            others = [v for k, v in NAMES.items() if k != latin and v in zh]
            wrong += 1
            shown = f"，这里写作「{others[0]}」" if others else ""
            print(f"  #{index:<3} {latin} 应作「{chinese}」{shown}→「{zh}」")
print(f"name drift in the stored translation: {wrong} line(s)")
