"""One way to read a WAV in these tools.

There used to be four copies of this function, and they disagreed: one refused anything
but 16-bit, three decoded everything *as* 16-bit. That was harmless while every file was
16-bit, and then the mix master became 24-bit (the true-peak limiter writes pcm_s24le) and
the copies did not fail -- they returned noise. The residue check read a 24-bit file as
16-bit and reported "13 dB of the old voice is back" against a mix that had not changed at
all, which is the kind of number that sends you looking in the wrong place for a day.

Both paths are kept: soundfile if it is installed, else the stdlib, which now knows about
24-bit and 32-bit as well as 16.
"""

import numpy as np


def read_wav(path):
    """(mono float32 samples, rate) from any WAV these tools write."""
    try:
        import soundfile as sf
        data, rate = sf.read(str(path), always_2d=True, dtype="float32")
        return data.mean(axis=1), rate
    except ImportError:
        pass

    import wave
    with wave.open(str(path)) as w:
        rate, channels, width = w.getframerate(), w.getnchannels(), w.getsampwidth()
        raw = w.readframes(w.getnframes())
    if width == 2:
        data = np.frombuffer(raw, dtype="<i2").astype(np.float32) / 32768.0
    elif width == 3:
        # 24-bit packed little-endian: widen to int32 by putting a zero byte in front, so
        # the shift by 8 is done by the byte layout and the scale becomes 2**23.
        b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3)
        wide = np.hstack([np.zeros((len(b), 1), np.uint8), b]).tobytes()
        data = np.frombuffer(wide, dtype="<i4").astype(np.float32) / 2147483648.0
    elif width == 4:
        data = np.frombuffer(raw, dtype="<i4").astype(np.float32) / 2147483648.0
    else:
        raise SystemExit(f"unsupported sample width {width}: {path}")
    if channels > 1:
        data = data.reshape(-1, channels).mean(axis=1)
    return data, rate
