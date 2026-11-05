"""Frequency-band fidelity: what a single PSNR number cannot say.

On this pipeline the error budget is lopsided. Measured against the lossless source,
the luma residual of a normal AMD render is ~12 levels RMS below sigma 4, ~1.3 levels
between sigma 1 and 4, and ~0.9 levels above sigma 1 - so the low band holds 97% of the
squared error and the high band well under 1%. A change that moves the mid band by 2 dB
therefore shows up in the head-line PSNR as a few hundredths, and a pass that quietly
softens the image can gain dB while losing the only thing the eye reads at 1080p.

This module splits the residual three ways and reports, per band, the PSNR of the
residual, the output's own energy against the source's, and their correlation:

  high   finer than ~2 px   the texture band
  mid    ~2 px to ~8 px     local structure
  low    coarser than ~8 px relight and grading, which is what a neural pass is *for*

`ratio` is the axis the old metric set had no entry for. Under 1 the pass removed
texture, over 1 it invented some. `corr` separates the two ways of removing it: a blur
lowers the energy while keeping the correlation (the detail is still where it was, just
fainter), whereas a wrong field decorrelates it.

`ratio` alone is still ambiguous, because the error-minimising amount of a band is not
the source's amount. The bands are orthogonal, so for a scale k on one band

    MSE(k) = (k^2 OO - 2k SO + SS) / px        ->        k* = SO / OO

and k* is exact from two dot products per frame. Measured on real footage the high band
lands at k* = 0.90-0.93 (equal to its correlation) while `ratio` sits at 1.00, so the
render carries about 10% more high band than the source supports and the first 10% of a
texture loss *raises* high-band PSNR. The mid band comes out at k* = 0.96-1.00, i.e. it
is already where it should be. `gain_at_kstar` is what a band would still gain from being
rescaled to its own optimum, so a small `ratio` drop can be read as "moving toward k*"
rather than "losing detail".
"""
from __future__ import annotations

import math

import cv2
import numpy as np

SIGMA_HI = 1.0
SIGMA_LO = 4.0
BANDS = ("high", "mid", "low")
# The arms that should be identical (same field content, same pass) spread by at most
# 0.0011 on the high band's ratio across the real-footage clips; twice that is the floor
# below which a ratio move is not a move.
RATIO_TOLERANCE = 0.002


def split(y: np.ndarray) -> dict[str, np.ndarray]:
    """Three additive bands of a float32 luma plane."""
    b_hi = cv2.GaussianBlur(y, (0, 0), SIGMA_HI)
    b_lo = cv2.GaussianBlur(y, (0, 0), SIGMA_LO)
    return {"high": y - b_hi, "mid": b_hi - b_lo, "low": b_lo}


def _rms(x: np.ndarray) -> float:
    return float(np.sqrt(np.mean(np.square(x, dtype=np.float32), dtype=np.float64)))


def _corr(a: np.ndarray, b: np.ndarray) -> float:
    a = a.ravel() - float(np.mean(a))
    b = b.ravel() - float(np.mean(b))
    den = math.sqrt(float(np.sum(a * a, dtype=np.float64)) * float(np.sum(b * b, dtype=np.float64)))
    return float(np.sum(a * b, dtype=np.float64) / den) if den > 0 else 1.0


class BandFidelity:
    """Accumulates the three-band comparison one frame at a time.

    Only running sums are held, so a whole clip streams through in constant memory. The
    per-frame ratios are averaged with equal weight: a single frame where the source is
    momentarily flat must not dominate the clip's texture budget.
    """

    def __init__(self, enabled: bool = True):
        self.enabled = enabled
        self.frames = 0
        self.residual = {b: [] for b in BANDS}
        self.energy_source = {b: [] for b in BANDS}
        self.energy_output = {b: [] for b in BANDS}
        self.correlation = {b: [] for b in BANDS}
        # Pooled dot products, kept for the band's optimal scale. Ratio and corr above
        # are per-frame means so one flat frame cannot dominate; k* has to be pooled,
        # because it is the argmin of the pooled squared error.
        self.ss = {b: 0.0 for b in BANDS}
        self.oo = {b: 0.0 for b in BANDS}
        self.so = {b: 0.0 for b in BANDS}
        self.px = 0.0

    def add(self, source_luma: np.ndarray, output_luma: np.ndarray) -> None:
        if not self.enabled:
            return
        s, o = split(source_luma), split(output_luma)
        for b in BANDS:
            self.residual[b].append(_rms(o[b] - s[b]))
            self.energy_source[b].append(_rms(s[b]))
            self.energy_output[b].append(_rms(o[b]))
            if b != "low":  # correlation of the relight band is a style question, not fidelity
                self.correlation[b].append(_corr(o[b], s[b]))
            sv, ov = s[b].ravel(), o[b].ravel()
            self.ss[b] += float(np.dot(sv, sv))
            self.oo[b] += float(np.dot(ov, ov))
            self.so[b] += float(np.dot(sv, ov))
        self.px += float(s["low"].size)
        self.frames += 1

    def result(self) -> dict | None:
        if not self.enabled or not self.frames:
            return None
        mean = lambda xs: sum(xs) / len(xs) if xs else None
        bands = {}
        for b in BANDS:
            res = mean(self.residual[b])
            ss, oo, so = self.ss[b], self.oo[b], self.so[b]
            kstar = so / oo if oo > 0 else 1.0
            mse = (ss + oo - 2 * so) / self.px if self.px else 0.0
            best = (ss - so * so / oo) / self.px if oo > 0 else mse
            bands[b] = dict(
                psnr=99.0 if res == 0 else 20 * math.log10(255.0 / res),
                residual_rms=res,
                energy_source=mean(self.energy_source[b]),
                energy_output=mean(self.energy_output[b]),
                ratio=(mean(self.energy_output[b]) / mean(self.energy_source[b])
                       if mean(self.energy_source[b]) else None),
                corr=mean(self.correlation[b]) if b != "low" else None,
                kstar=kstar,
                gain_at_kstar=(0.0 if best <= 0 or mse <= 0
                               else 10 * math.log10(mse / best)))
        share = {b: bands[b]["residual_rms"] ** 2 for b in BANDS}
        total = sum(share.values()) or 1.0
        return dict(frames=self.frames, sigmas=[SIGMA_HI, SIGMA_LO], bands=bands,
                    error_share={b: share[b] / total for b in BANDS},
                    detail_ratio_high=bands["high"]["ratio"],
                    ratio_tolerance=RATIO_TOLERANCE)


def verdict(reference: dict | None, arm: dict | None) -> tuple[str, str]:
    """Names the arm's effect, weighting each band by the error it is allowed to move.

    A band move is only worth reporting at the size it actually contributes to the
    head-line number: `share` of the squared error times the band's dB move. On real
    footage the low band holds 97-99% of that error, so a +2 dB mid-band change lands in
    the head-line as +0.02 dB and this refuses to call it a gain. `relight` is not a
    dismissal - the low band is what a neural pass exists to change - but it is a
    different claim from a reconstruction win, and the two were being confused."""
    if not reference or not arm:
        return "unknown", "no band data"
    d = {b: arm["bands"][b]["psnr"] - reference["bands"][b]["psnr"] for b in BANDS}
    share = reference["error_share"]
    contrib = {b: share[b] * d[b] for b in BANDS}
    total = sum(contrib.values())
    d_ratio = (arm["detail_ratio_high"] or 0) - (reference["detail_ratio_high"] or 0)
    kstar = reference["bands"]["high"]["kstar"]
    # A band counts as moved only when its contribution clears a hundredth of a dB,
    # which is where an 8-bit frame stops resolving a change.
    moved = {b: v for b, v in contrib.items() if abs(v) >= 0.01}
    detail = (f"low {contrib['low']:+.3f} / mid {contrib['mid']:+.3f} / "
              f"high {contrib['high']:+.3f} dB")
    if not moved and abs(d_ratio) <= RATIO_TOLERANCE:
        return "inert", "nothing reaches 0.01 dB"
    if abs(total) < 0.01:
        if d_ratio < -RATIO_TOLERANCE:
            return "texture-only-loss", (f"texture {d_ratio * 100:+.1f}%, high band "
                                        f"{d['high']:+.2f} dB worth {share['high'] * 100:.1f}% of the error")
        if d_ratio > RATIO_TOLERANCE:
            return "texture-only-gain", (f"texture {d_ratio * 100:+.1f}%, high band "
                                        f"{d['high']:+.2f} dB worth {share['high'] * 100:.1f}% of the error")
        # Bands moved but cancelled. Saying "inert" here would hide the largest move
        # in the set, which is exactly the case const-fixed produces.
        return "cancel", detail
    if total < 0:
        return "degradation", detail
    if set(moved) == {"low"}:
        toward = (" toward k*" if d_ratio < 0 and (reference["detail_ratio_high"] or 0) > kstar
                  else "")
        return "relight", (f"{d['low']:+.2f} dB in the {share['low'] * 100:.0f}% band, "
                           f"texture {d_ratio * 100:+.1f}%{toward}")
    if len(moved) >= 2 and min(moved.values()) > -0.01:
        return "broad-gain", f"{total:+.3f} dB over {len(moved)} bands"
    return "mixed", detail
