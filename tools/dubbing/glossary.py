"""One glossary, read by both the translator and the fitting stage.

Two stages write Chinese for the same film: `step2t_translate.py` drafts it, and
`step12_fit_auto.py` may rewrite a line that will not fit. Each used to carry its
own idea of names -- the translator had none and produced 凯莉 for Celia, while the
fitter patched 凯莉 back to 塞莉亚 in its own hard-coded table. The subtitle file
then disagreed with the voice on three lines because the patch lived downstream of
where the file was written.

The file is `glossary.json` next to this module, with two sections:

  names  Latin spelling -> Chinese, given to the translator as a constraint
  fixes  wrong Chinese -> right Chinese, applied after the fact to anything, so a
         model that ignores the constraint still cannot put the wrong name on screen

A line the model gets right is left alone; `fixes` only ever rewrites substrings, so
it stays safe to apply to output that already follows the glossary.
"""

import json
import pathlib
import re

# Canonical copy lives at dlss5-video-amd/tools/dubbing/glossary.py - change both.

HERE = pathlib.Path(__file__).resolve().parent
FALLBACK = {"names": {}, "fixes": {}}


def load(path=None):
    """Read the glossary, and never fail a run over a missing or broken file."""
    target = pathlib.Path(path) if path else pathlib.Path(__import__("os").environ.get(
        "DUB_GLOSSARY", str(HERE / "glossary.json")))
    try:
        data = json.load(open(target, encoding="utf-8"))
    except (OSError, ValueError):
        return dict(FALLBACK)
    return {"names": dict(data.get("names") or {}), "fixes": dict(data.get("fixes") or {})}


def instruction(glossary, source_words=()):
    """The sentence added to the translator's prompt, or "" when there is nothing to say.

    Only the names that actually appear in this line are listed. Handing the model
    eight names to "remember" for a two-word line was measured to make it start
    explaining instead of translating.
    """
    names = glossary.get("names") or {}
    if not names:
        return ""
    wanted = [f"{latin} -> {zh}" for latin, zh in names.items()
              if not source_words or re.search(rf"\b{re.escape(latin)}\b", source_words, re.I)]
    if not wanted:
        return ""
    return (" Write these names exactly as given, with no other spelling: "
            + "; ".join(wanted) + ".")


def fix(text, glossary):
    """Apply the wrong-spelling table. Longest keys first, so 汤姆· wins over 汤姆."""
    for wrong in sorted(glossary.get("fixes") or {}, key=len, reverse=True):
        text = text.replace(wrong, glossary["fixes"][wrong])
    return text
