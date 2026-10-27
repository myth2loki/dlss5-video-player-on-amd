"""Qwen3-TTS 客观指标：音色相似度 + 音高 + 共振峰。

按项目里的教训：音高不变 ≠ 音色不变，验证 TTS 必须同时看 F0 / F1 / F2 / 谱质心，
最终以人耳为准；指标只能用来排除"重采样"这一种情况。
跨语言克隆的绝对相似度基线是 XTTS 的 cosine 0.32（同人）/ 0.05（异人）。
"""
import pathlib
import shutil
import warnings

import numpy as np
import soundfile as sf
import librosa

warnings.filterwarnings("ignore")

_symlink = pathlib.Path.symlink_to


def _copy(self, target, target_is_directories=False):
    try:
        _symlink(self, target, target_is_directories)
    except OSError:
        shutil.copy2(self, target)


pathlib.Path.symlink_to = _copy

import torch  # noqa: E402
from speechbrain.inference.speaker import EncoderClassifier  # noqa: E402

encoder = EncoderClassifier.from_hparams(source="speechbrain/spkrec-ecapa-voxceleb",
                                         savedir="F:/tmp/dub-verify/ecapa", run_opts={"device": "cpu"})


def embed(path):
    data, rate = sf.read(path, always_2d=True)
    data = data.mean(axis=1)
    if rate != 16000:
        data = librosa.resample(data, orig_sr=rate, target_sr=16000)
    with torch.no_grad():
        out = encoder.encode_batch(torch.from_numpy(np.asarray(data, np.float32)).unsqueeze(0))
    v = out.squeeze().cpu().numpy()
    return v / (np.linalg.norm(v) + 1e-9), data, rate


def metrics(path):
    data, rate = sf.read(path, always_2d=True)
    data = data.mean(axis=1)
    if len(data) < rate // 4:
        return None
    f0 = librosa.yin(data, fmin=60, fmax=400, sr=rate)
    f0 = f0[np.isfinite(f0)]
    f0 = f0[f0 > 60]
    centroid = librosa.feature.spectral_centroid(y=data, sr=rate)[0]
    flat = librosa.feature.spectral_flatness(y=data)[0]
    rms = float(np.sqrt(np.mean(data ** 2)))
    peak = float(np.abs(data).max())
    # 共振峰（粗略：LPC）
    formants = []
    try:
        from scipy.signal import lfilter
        from numpy.linalg import solve
        for _ in range(3):
            a = librosa.lfilter([1], [1], data)
            a = librosa.effects.preemphasis(data)
            order = 12
            lpc = librosa.lpc(a, order=order)
            roots = np.roots(lpc)
            roots = roots[np.imag(roots) > 0]
            freqs = np.angle(roots) * rate / (2 * np.pi)
            freqs = freqs[(freqs > 90) & (freqs < 4000)]
            formants.append(float(np.median(freqs)) if len(freqs) else float("nan"))
    except Exception:
        formants = [float("nan")] * 3
    return {
        "seconds": len(data) / rate, "rate": rate,
        "f0": float(np.median(f0)) if len(f0) else float("nan"),
        "centroid": float(np.median(centroid)),
        "flatness": float(np.median(flat)),
        "rms": rms, "peak": peak,
        "f1": formants[0] if len(formants) > 0 else float("nan"),
        "f2": formants[1] if len(formants) > 1 else float("nan"),
    }


TARGETS = [
    ("Tom 原声参考", "F:/tmp/dub-verify/refs/tom.wav"),
    ("Celia 原声参考", "F:/tmp/dub-verify/refs/celia.wav"),
    ("Qwen3-TTS 克隆(中文)", "F:/tmp/s2st/probe_tts.wav"),
]

vectors = {}
print(f"{'素材':22s} {'时长':>7s} {'F0中位':>8s} {'F1':>7s} {'F2':>7s} {'谱质心':>8s} {'RMS':>7s} {'峰值':>6s}")
for name, path in TARGETS:
    if not pathlib.Path(path).exists():
        print(f"{name:22s} 缺失 {path}")
        continue
    m = metrics(path)
    v, _, _ = embed(path)
    vectors[name] = v
    print(f"{name:22s} {m['seconds']:6.2f}s {m['f0']:8.1f} {m['f1']:7.0f} {m['f2']:7.0f} "
          f"{m['centroid']:8.0f} {m['rms']:7.4f} {m['peak']:6.3f}")

names = list(vectors)
print("\nECAPA 余弦相似度:")
for i, a in enumerate(names):
    for b in names[i + 1:]:
        print(f"  {a:22s} vs {b:22s} {float(vectors[a] @ vectors[b]):+.3f}")
