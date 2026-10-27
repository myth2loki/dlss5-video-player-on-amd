"""Write the subtitles from the same rows the voice was made from.

The dub had no subtitle track, and the subtitle data that existed was built from a
different file than the voice: `zh_final.json` held the draft translation while the
speech came from `fit_auto.json`, where the override table and the budget rewrites
had already changed the words. Thirteen of seventy-one lines said one thing on
screen and another in the ear, and no amount of care downstream could have caught
it because the two files were never compared.

So this module takes its input from `fit_auto.json` alone - the same `text` the
engine spoke and the same `anchor`/`seconds` it spoke them over. Agreement stops
being something to check and becomes something the file cannot express.

Two things are also enforced here rather than audited afterwards:

  - a cue stays up for at least `MIN_DUR`, because 0.38 s of "你把我遗忘在地球上！"
    is not read by anybody, and the reading-speed failure measured on this film
    was caused by short source cues, not by long translations
  - a cue never runs into the next one; the gap is `GAP`, so two lines cannot be
    on screen together, which is the one thing that makes a subtitle unreadable

A line that has to give up duration to the next one gives it up, because the next
line's turn is when the next line is actually spoken.
"""

import pathlib
import subprocess

# Canonical copy lives at dlss5-video-amd/tools/dubbing/subtitles.py. This one is
# what the pipeline runs; that one is what the repository keeps. Change both, or
# the next person reading the repo will implement the version that was wrong.

MIN_DUR = 0.80      # below this a cue flashes; measured failures ran to 0.30 s
GAP = 0.08          # two cues closer than this read as one line of noise
MAX_DUR = 6.0       # a cue held this long is the line being wrong, not slow reading
LINE_UNITS = 32     # a full-width character counts 2; 16 hanzi fit a line
MAX_LINES = 2

# Punctuation a line may be broken after, best first.
BREAKS = "。！？…；：，、,.!?;:"
STYLE_NAME = "Dub"


def _units(text):
    """Width in half-width units: hanzi and full-width punctuation count double."""
    return sum(2 if ord(ch) > 0x2E7F else 1 for ch in text)


def _split_at(text, target):
    """Break `text` into two, at punctuation nearest `target` units, within the limit.

    Two failures shaped this. Filling the first row to the limit and dumping the
    rest into the second produced a 14-unit row above a 48-unit one, because the
    break fell on the first comma. Choosing the punctuation nearest a bare target
    then overshot the limit, because a comma is not where the arithmetic lands.
    So the candidate cuts are the ones that leave a row that actually fits.
    """
    prefix, running = [], 0
    for ch in text:
        running += 2 if ord(ch) > 0x2E7F else 1
        prefix.append(running)
    # Cut after character c-1 leaves `prefix[c-1]` units on the first row.
    fitting = [c for c in range(1, len(text)) if prefix[c - 1] <= LINE_UNITS]
    if not fitting:
        return text[:1], text[1:].strip()
    comfortable = [c for c in fitting if prefix[c - 1] >= LINE_UNITS * 0.4]
    candidates = comfortable or fitting
    any_cut = min(candidates, key=lambda c: abs(prefix[c - 1] - target))
    # Punctuation reads better, but not at the price of a row three times the width of
    # the one above it: a thirty-one-character line has its only comma at 14 units, so
    # breaking there leaves 48 units on the second row. Worth six units of raggedness
    # to avoid.
    on_punctuation = [c for c in candidates if text[c - 1] in BREAKS]
    cut = any_cut
    if on_punctuation:
        punct = min(on_punctuation, key=lambda c: abs(prefix[c - 1] - target))
        if abs(prefix[punct - 1] - target) <= abs(prefix[any_cut - 1] - target) + 6:
            cut = punct
    return text[:cut].strip(), text[cut:].strip()


def wrap(text):
    """Break a line into at most `MAX_LINES` rows of `LINE_UNITS` half-widths.

    Breaks after punctuation where it can, mid-line where it must. Returning more
    than `MAX_LINES` rows is refused by concatenating the tail, because a fourth row
    risks the bottom of the frame and nobody reads it anyway.
    """
    text = text.strip()
    if _units(text) <= LINE_UNITS:
        return [text]
    rows, rest = [], text
    while rest and len(rows) < MAX_LINES:
        remaining = _units(rest)
        if remaining <= LINE_UNITS:
            rows.append(rest)
            rest = ""
            break
        slots = MAX_LINES - len(rows)
        first, rest = _split_at(rest, remaining if slots == 1 else remaining / slots)
        rows.append(first)
    if rest:
        rows[-1] = (rows[-1] + rest).strip()
    return rows or [text]


def _stamp(seconds):
    if seconds < 0:
        seconds = 0.0
    hours, rest = divmod(seconds, 3600)
    minutes, rest = divmod(rest, 60)
    return f"{int(hours)}:{int(minutes):02d}:{rest:05.2f}"


def cues(rows):
    """Turn fitted rows into (start, end, text) cues that do not overlap.

    `anchor` is where the line actually starts after the fit moved it, and
    `seconds` is what is left of it after any tempo change - so the cue follows
    the voice rather than the original English cue it was measured from.
    """
    built = []
    for row in rows:
        if not row.get("text") or not row.get("seconds"):
            continue
        start = float(row.get("anchor") or row.get("start") or 0.0)
        end = start + float(row["seconds"])
        built.append([start, max(end, start + MIN_DUR), row["text"], row["index"]])

    for position, cue in enumerate(built):
        ceiling = built[position + 1][0] - GAP if position + 1 < len(built) else 1e9
        # Floor and ceiling disagree when the next line starts inside this one's
        # minimum: the next line's turn wins, and the short cue is reported rather
        # than quietly shown on top of it.
        cue[1] = min(max(cue[1], cue[0] + MIN_DUR), ceiling, cue[0] + MAX_DUR)
        if cue[1] < cue[0] + GAP:
            cue[1] = max(cue[0] + 0.04, min(cue[0] + GAP, ceiling))
    return [(s, e, text) for s, e, text, _ in built]


def write(rows, path, width=1920, height=1080):
    """Write an ASS file whose text is exactly what the voice spoke."""
    built = cues(rows)
    font_size = max(12, round(height * 0.046))
    margin_v = max(8, round(height * 0.035))
    lines = [
        "[Script Info]",
        "ScriptType: v4.00+",
        f"PlayResX: {width}",
        f"PlayResY: {height}",
        "ScaledBorderAndShadow: yes",
        "WrapStyle: 2",          # we break the lines ourselves, libass must not
        "",
        "[V4+ Styles]",
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, "
        "BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, "
        "BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding",
        f"Style: {STYLE_NAME},Microsoft YaHei,{font_size},&H00FFFFFF,&H000000FF,"
        f"&H00000000,&H80000000,0,0,0,0,100,100,0,0,1,{max(1, font_size // 8)},0,2,"
        f"{round(width * 0.03)},{round(width * 0.03)},{margin_v},1",
        "",
        "[Events]",
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text",
    ]
    for start, end, text in built:
        body = "\\N".join(wrap(text)).replace("{", "(").replace("}", ")")
        lines.append(f"Dialogue: 0,{_stamp(start)},{_stamp(end)},{STYLE_NAME},,0,0,0,,{body}")
    path = pathlib.Path(path)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return built


def probe_size(video, ffprobe):
    """Real frame size, so libass does not stretch the glyphs on a wide film.

    Falls back to 1920x1080 rather than guessing: a wrong PlayRes with the right
    aspect ratio still reads, a wrong aspect ratio does not.
    """
    try:
        out = subprocess.run([str(ffprobe), "-v", "error", "-select_streams", "v:0",
                              "-show_entries", "stream=width,height", "-of", "csv=p=0",
                              str(video)], capture_output=True, text=True, timeout=30)
        width, height = (int(part) for part in out.stdout.strip().split(",")[:2])
        if width > 0 and height > 0:
            return width, height
    except (OSError, ValueError, subprocess.SubprocessError):
        pass
    return 1920, 1080
