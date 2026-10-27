"""Translate each aligned line, once, without a length budget.

This is the draft pass: it says what the line means, not how long it may be. The
fitting stage rewrites whatever does not fit, so asking for compression here would
only lose meaning twice.

Output is `work/zh_final.json` in the shape the fitting stage reads:
  {"index", "en_whisper", "zh"}
A line the model returns nothing for is dropped rather than written empty, because
an empty draft is not a line anybody can fix later.
"""

import json
import os
import pathlib
import re
import subprocess
import sys
import time
import urllib.request

import glossary

ROOT = pathlib.Path(os.environ.get("DUB_ROOT", "F:/tmp/s2st"))
REPO = pathlib.Path(os.environ.get("DUB_REPO", "F:/dlss5-video/dlss5-video-amd"))
WORK = ROOT / "work"

TR_SERVER = os.environ.get("DUB_TR_SERVER", str(REPO / "translation/runtime/llama-server.exe"))
TR_MODEL = os.environ.get("DUB_TR_MODEL", str(REPO / "translation/models/translategemma-4b-it-Q4_K_M.gguf"))
TR_PORT = int(os.environ.get("DUB_TR_PORT", "8133"))
TR_BASE = f"http://127.0.0.1:{TR_PORT}"
TARGET = os.environ.get("DUB_TARGET_NAME", "Chinese (zh-Hans)")
TEMPERATURE = float(os.environ.get("DUB_TR_TEMPERATURE", "0.2"))
# Shared with the fitting stage: it is told the same spellings this stage asks for, and
# enforces them afterwards, so a model that ignores the prompt cannot put 凯莉 on screen
# while the voice says 塞莉亚.
GLOSSARY = glossary.load()


def clean(raw):
    cut = raw.find("<end_of_turn>")
    if cut >= 0:
        raw = raw[:cut]
    raw = raw.strip()
    while len(raw) >= 2 and raw[0] == '"' and raw[-1] == '"':
        raw = raw[1:-1].strip()
    raw = re.sub(r"^\d+[\.、]\s*", "", raw)
    return raw.strip()


# The translator sometimes answers as a service rather than as a translator: asked
# for "Be honest." it wrote 我们致力于提供准确、流畅、符合中国语习惯的翻译服务。
# A dubbed line has to be the line, so those answers are rejected and asked again.
SERVICE_TALK = re.compile(r"翻译服务|致力于|为您提供|作为一名?(?:AI|人工智能)|语言模型"
                          r"|以下是|希望对您|如需更多")
MAX_CHARS_PER_WORD = 4.0     # Chinese characters an English word can plausibly become
SLACK = 8


def implausible(source, translation):
    """Is this answer about the film, or about the translator?

    Length is the second signal: a two-word sentence does not become a 28-character
    one, so an answer that far outside the source's scope is a degeneration even
    when it contains none of the tell-tale phrases.
    """
    if SERVICE_TALK.search(translation):
        return True
    words = max(1, len(re.findall(r"[A-Za-z']+", source)))
    return len(translation) > words * MAX_CHARS_PER_WORD + SLACK


def translate(text, strict=False):
    instruction = ("You are a professional English (en) to Chinese (zh-Hans) translator. Your goal "
                   "is to accurately convey the meaning and nuances of the original English text "
                   "while adhering to Chinese grammar, vocabulary, and cultural sensitivities.")
    if strict:
        instruction += (" Translate only the sentence you are given. Do not describe services, "
                        "policies or your own capabilities, and do not add anything.")
    instruction += glossary.instruction(GLOSSARY, text)
    body = json.dumps({
        "messages": [{"role": "user", "content":
                      instruction + " Output only the translation, without quotes or "
                                    "commentary.\n\n" + text}],
        "temperature": 0.0 if strict else TEMPERATURE, "max_tokens": 200, "stream": False,
    }).encode()
    request = urllib.request.Request(TR_BASE + "/v1/chat/completions", body,
                                     {"Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=180) as response:
        payload = json.load(response)
    return clean(payload["choices"][0]["message"]["content"])


def translate_plain(text):
    """Last resort for a line the role-played prompt cannot handle.

    "Be honest." came back twice as 我们致力于提供准确、流畅... -- a two-word imperative is
    not enough context for the instruct prompt to stay in character. With no framing at
    all the model just translates, which is all this stage needs.
    """
    body = json.dumps({
        "messages": [{"role": "user", "content": "把这句话翻译成简体中文，只输出译文：\n" + text}],
        "temperature": 0.0, "max_tokens": 80, "stream": False,
    }).encode()
    request = urllib.request.Request(TR_BASE + "/v1/chat/completions", body,
                                     {"Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=180) as response:
        payload = json.load(response)
    return clean(payload["choices"][0]["message"]["content"])


def main():
    rows = json.load(open(WORK / "lines.json", encoding="utf-8"))
    target = WORK / "zh_final.json"
    if target.exists():
        existing = json.load(open(target, encoding="utf-8"))
        if len(existing) == len(rows):
            print(f"   reuse {target.name} ({len(existing)} lines)")
            return

    server = subprocess.Popen([TR_SERVER, "-m", TR_MODEL, "-c", "2048", "-ngl", "99",
                               "--port", str(TR_PORT), "--no-jinja", "--chat-template", "gemma"],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    out = []
    try:
        deadline = time.time() + 240
        while time.time() < deadline:
            try:
                with urllib.request.urlopen(TR_BASE + "/health", timeout=2) as response:
                    if response.status == 200:
                        break
            except Exception:
                time.sleep(1.0)
        else:
            sys.exit("translation server did not come up in 240 s")
        print("   server up")

        started = time.time()
        dropped = []
        for row in rows:
            source = row["text"].strip()
            if not source:
                continue
            text = translate(source)
            if not text or implausible(source, text):
                again = translate(source, strict=True)
                text = again if again and not implausible(source, again) else ""
                if not text:
                    plain = translate_plain(source)
                    text = plain if plain and not implausible(source, plain) else ""
                if text and not re.search(r"[。？！…，、,.!?]$", text):
                    text += "。"
                if not text:
                    # Nothing usable twice over. A missing line leaves the shot silent,
                    # which is a loss the viewer can hear but not one that makes the dub
                    # sound broken the way a spoken one would.
                    dropped.append(row["index"])
                    print(f"  #{row['index']:2d} rejected: {source[:40]}")
                    continue
            entry = {"index": row["index"], "en_whisper": source,
                     "zh": glossary.fix(text, GLOSSARY)}
            out.append(entry)
            print(f"  #{row['index']:2d} {source[:44]}")
            print(f"        -> {text}")
            json.dump(out, open(target, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
        print(f"\n  {len(out)}/{len(rows)} translated in {time.time() - started:.0f}s"
              + (f", {len(dropped)} dropped: {dropped}" if dropped else ""))
    finally:
        server.terminate()
        try:
            server.wait(timeout=10)
        except subprocess.TimeoutExpired:
            server.kill()


if __name__ == "__main__":
    main()
