"""Build clips whose motion is known exactly, so a motion estimator can be scored.

Every clip is generated from one real film frame, not from a synthetic pattern.
That is deliberate: testsrc2 and friends are periodic, and a periodic pattern
makes zero displacement a *correct* answer for a shifted frame - the SAD is
already zero - so an estimator that reports "nothing moved" on a 4 px pan is not
wrong, it is under-determined. The first attempt at this corpus used exactly
that and produced a beautiful, meaningless result.

Raw frames go straight to x264 in lossless RGB: any codec noise here would be
noise the estimators have to fight, and it would differ between the arms.

`base.png` is one 1280x720 frame taken from the test film and is deliberately
not checked in (it is copyrighted footage). See `docs/MOTION-GUIDE-AB-zh.md`
for the exact cut; `plate()` removes the letterbox and keeps the burned-in
subtitle, whose true motion is zero.
"""
import json
import os
import subprocess
import sys

import cv2
import numpy as np

W, H = 1280, 520
FRAMES = 72
HERE = os.path.dirname(os.path.abspath(__file__))
CLIPS = os.path.join(HERE, "clips")
FFMPEG = os.environ.get("FFMPEG", r"F:\rvc\ffmpeg\bin\ffmpeg.exe")


def plate():
    img = cv2.imread(os.path.join(HERE, "base.png"), cv2.IMREAD_COLOR)
    # The letterbox is ~110 px top and ~120 px bottom; cropped out so a pan does
    # not smear a black bar over the picture. The burned-in subtitle stays: it is
    # glued to the screen, so its true motion is zero and it punishes an estimator
    # that invents motion wherever there is none.
    return img[115:115 + H, 0:W]


def shift(img, dx, dy):
    m = np.float32([[1, 0, dx], [0, 1, dy]])
    return cv2.warpAffine(img, m, (W, H), flags=cv2.INTER_LINEAR,
                          borderMode=cv2.BORDER_REPLICATE)


def encode(name, frames):
    path = os.path.join(CLIPS, name + ".mp4")
    p = subprocess.Popen(
        [FFMPEG, "-y", "-f", "rawvideo", "-pix_fmt", "bgr24",
         "-s", f"{W}x{H}", "-r", "24", "-i", "-",
         "-c:v", "libx264rgb", "-qp", "0", "-preset", "veryfast", path],
        stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for f in frames:
        p.stdin.write(np.ascontiguousarray(f, dtype=np.uint8).tobytes())
    p.stdin.close()
    if p.wait() != 0:
        raise SystemExit(f"ffmpeg failed for {name}")
    return path


def pan(name, step):
    img = plate()
    encode(name, (shift(img, 0, step * t) for t in range(FRAMES)))
    # Current pixel (x,y) holds content that sat at (x, y - step) last frame.
    return {"kind": "pan", "dy": -step, "dx": 0.0}


def zoom(name, s):
    img = plate()
    def frame(t):
        f = s ** t
        m = cv2.getRotationMatrix2D((W / 2, H / 2), 0, f)
        return cv2.warpAffine(img, m, (W, H), flags=cv2.INTER_LINEAR,
                              borderMode=cv2.BORDER_REPLICATE)
    encode(name, (frame(t) for t in range(FRAMES)))
    # Frame t is the plate scaled by s^t, so the point at (x,y) now was at
    # c + (p-c)/s one frame ago.
    return {"kind": "zoom", "scale": s}


def layers(name, step, bw=220, bh=160):
    img = plate()
    patch = img[180:180 + bh, 300:300 + bw].copy()
    out = []
    for t in range(FRAMES):
        f = img.copy()
        x = int(round(300 + step * t))
        f[180:180 + bh, x:x + bw] = patch
        out.append(f)
    encode(name, out)
    # The patch's content moves with the patch; the background does not move.
    return {"kind": "layers", "dx": -step, "dy": 0, "box": [180, 180 + bh, step]} 


def main():
    os.makedirs(CLIPS, exist_ok=True)
    truth = {}
    truth["pan0p5"] = pan("pan0p5", 0.5)
    truth["pan1"] = pan("pan1", 1.0)
    truth["pan2"] = pan("pan2", 2.0)
    truth["pan4"] = pan("pan4", 4.0)
    truth["pan8"] = pan("pan8", 8.0)
    truth["zoom"] = zoom("zoom", 1.005)
    truth["layers"] = layers("layers", 6.0)
    for name, t in truth.items():
        t["width"], t["height"], t["frames"] = W, H, FRAMES
    with open(os.path.join(HERE, "truth.json"), "w") as fh:
        json.dump(truth, fh, indent=2)
    print("wrote", ", ".join(sorted(truth)))


if __name__ == "__main__":
    main()
