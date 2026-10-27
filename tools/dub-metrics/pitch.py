"""One F0 measurement for the audits, and why it is not Praat's.

Every "is this the right voice / the right gender" question comes down to where a voice
sits on the pitch axis, so the audit needs an F0 estimator that agrees with itself across
files. Praat's was already in the pipeline (`voice.py`) and it does not: on this film it
reports the male lead's reference at a median of 233 Hz, twice his actual pitch, because
autocorrelation locks onto the second harmonic as readily as the first, and which one it
picks depends on how loud that frame was. A statistic that doubles half the frames still
orders two voices correctly but cannot be compared against a fixed threshold, and worse,
the doubling rate differs between the source recording and a synthesised clip.

YIN (de Cheveigne & Kawahara, 2002) avoids that by construction: the cumulative mean
normalised difference function is searched for the *first* dip under a threshold, so a
period is only accepted if no shorter period explains the signal. Measured on the three
reference clips it puts them where the ears put them -- celia 211 Hz, tom 155, robot 132 --
and, unlike the median, the 10th percentile is stable across a line's emotional range.

Pure numpy: no parselmouth, no scipy. 40 ms window, 10 ms hop, 60-400 Hz.
"""

import numpy as np

FLOOR = 60.0
CEILING = 400.0
WINDOW = 0.04
HOP = 0.01
THRESHOLD = 0.15      # YIN's own default; above this the frame is re-examined, not trusted
UNVOICED = 0.35       # a dip this shallow is not a period, it is noise


def yin_track(samples, rate):
    """F0 for every frame, 0 where YIN cannot vouch for one.

    A track rather than a list, so a caller can line it up with other frame-rate evidence
    (how loud the backing track is, say) and keep only the frames where this reading can
    be believed.
    """
    x = np.asarray(samples, dtype=np.float32).reshape(-1)
    size = int(WINDOW * rate)
    hop = int(HOP * rate)
    if rate <= 0 or len(x) < size:
        return np.zeros(0)

    tau_min = max(2, int(rate / CEILING))
    tau_max = min(size - 1, int(rate / FLOOR))
    # No window on purpose. YIN's difference function compares a frame against itself
    # delayed by tau, and the running mean that normalises it assumes every sample carries
    # the same weight; tapering the frame breaks that assumption and shifts the result --
    # measured, a Hann window moved the male lead's reference from 153 Hz to 171, most of
    # the way to the female band.
    fft_size = 1
    while fft_size < 4 * size:
        fft_size <<= 1

    frames = 1 + (len(x) - size) // hop
    track = np.zeros(frames)
    for i in range(frames):
        frame = x[i * hop:i * hop + size].astype(np.float64)
        frame = frame - frame.mean()
        if np.sqrt(np.mean(frame ** 2)) < 1e-4:      # silence, not a quiet voice
            continue
        spectrum = np.fft.rfft(frame, fft_size)
        energy = np.fft.irfft(spectrum * np.conj(spectrum), fft_size)[:size]
        cumulative = np.concatenate(([0.0], np.cumsum(frame ** 2)))
        tau = np.arange(tau_max + 1)
        # The tail energy of the shifted frame, so the difference function is not biased
        # by comparing a full window against a shorter one.
        head = cumulative[size] + (cumulative[size - tau] - cumulative[0])
        difference = head - 2 * energy[:tau_max + 1]
        difference[0] = 0.0
        running = np.cumsum(difference[1:])
        cmnd = np.ones_like(difference)
        cmnd[1:] = difference[1:] * np.arange(1, tau_max + 1) / np.maximum(running, 1e-12)

        pick = None
        for candidate in range(tau_min, tau_max + 1):
            if cmnd[candidate] < THRESHOLD:
                while candidate + 1 <= tau_max and cmnd[candidate + 1] < cmnd[candidate]:
                    candidate += 1
                pick = candidate
                break
        if pick is None:
            band = cmnd[tau_min:tau_max + 1]
            if not len(band):
                continue
            pick = tau_min + int(np.argmin(band))
            if cmnd[pick] > UNVOICED:
                continue
        track[i] = rate / pick
    return track


def yin_f0(samples, rate):
    """F0 per voiced frame, in Hz. Frames YIN cannot vouch for are dropped."""
    track = yin_track(samples, rate)
    return track[track > 0]


def floor_of(values, minimum=20):
    """(10th percentile, frames) of a set of F0 readings, or (None, frames)."""
    values = np.asarray(values, dtype=np.float64)
    values = values[values > 0]
    if len(values) < minimum:
        return None, len(values)
    return float(np.percentile(values, 10)), len(values)


def pitch_floor(samples, rate, minimum=20):
    """(10th percentile of voiced F0, frames) or (None, frames) when there is too little.

    The low percentile, not the median: a line that ends on a rising question has a median
    far above the speaker's resting pitch, and one shouted word has no resting pitch at
    all. The bottom of the distribution is the part that identifies the voice.
    """
    return floor_of(yin_f0(samples, rate), minimum)


def semitones(a, b):
    """Distance from b to a, in semitones. Positive means a is the higher voice."""
    if not a or not b:
        return None
    return 12.0 * float(np.log2(a / b))
