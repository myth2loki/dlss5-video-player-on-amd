"""Speaker embeddings for the dubbing pipeline -- ONNX Runtime, no torch.

step1_align used to import speechbrain's ECAPA-TDNN through torch, which made 2.5 GB
of wheels a hard requirement for computing a 192-d vector per line. The fbank front
end and the TDNN are now baked into one graph by tools/export_ecapa_onnx.py, so the
runtime side is numpy in, numpy out.

The graph is the *same* ECAPA-VoxCeleb model the pipeline was already calibrated
against: cosine similarity to the torch output is 1.000000 on probes from 0.6 s to
21 s, so CLUSTER_THRESHOLD and the 0.15 confidence margin keep their meaning. A
different embedding model would silently invalidate both.
"""

import os
import pathlib

import numpy as np

ENGINE = pathlib.Path(os.environ.get("DUB_ENGINE",
                                     str(pathlib.Path(__file__).resolve().parent)))
MODEL = pathlib.Path(os.environ.get("DUB_SPK_MODEL",
                                    str(ENGINE / "models" / "ecapa_voxceleb.onnx")))
SAMPLE_RATE = 16000
MIN_SECONDS = 0.5        # shorter than this there is no timbre to measure


class SpeakerEmbedder:
    """Waveform (float32, 16 kHz mono) -> unit-length 192-d embedding."""

    def __init__(self, model=None, threads=None):
        path = pathlib.Path(model or MODEL)
        if not path.exists():
            raise FileNotFoundError(
                f"speaker model missing: {path}\n"
                f"build it once with: python tools/export_ecapa_onnx.py")
        import onnxruntime as ort          # imported here: stage 0/2..5 have no use for it
        options = ort.SessionOptions()
        options.log_severity_level = 3
        options.intra_op_num_threads = int(
            threads or os.environ.get("DUB_SPK_THREADS", min(8, os.cpu_count() or 4)))
        self.session = ort.InferenceSession(str(path), options,
                                            providers=["CPUExecutionProvider"])
        self.input = self.session.get_inputs()[0].name

    def embed(self, samples, sr=SAMPLE_RATE):
        if sr != SAMPLE_RATE:
            raise ValueError(f"expected {SAMPLE_RATE} Hz, got {sr}")
        samples = np.asarray(samples, dtype=np.float32).reshape(-1)
        if len(samples) < int(MIN_SECONDS * sr):
            return None
        out = self.session.run(None, {self.input: samples[None, :]})[0]
        return np.asarray(out, dtype=np.float64).reshape(-1)


def _load16k(path):
    """Read any audio file as 16 kHz mono, via ffmpeg when the rate is not already 16 k."""
    import subprocess
    import tempfile

    import soundfile as sf

    clip, rate = sf.read(path, always_2d=True)
    clip = clip.mean(axis=1).astype(np.float32)
    if rate == SAMPLE_RATE:
        return clip
    ffmpeg = os.environ.get("DUB_FFMPEG", "ffmpeg")
    with tempfile.TemporaryDirectory() as tmp:
        out = pathlib.Path(tmp) / "16k.wav"
        subprocess.run([ffmpeg, "-v", "error", "-y", "-i", str(path), "-ac", "1",
                        "-ar", str(SAMPLE_RATE), "-c:a", "pcm_s16le", str(out)], check=True)
        clip, _ = sf.read(str(out), always_2d=True)
    return clip.mean(axis=1).astype(np.float32)


if __name__ == "__main__":                 # smoke test: python spk.py a.wav b.wav
    import sys

    embedder = SpeakerEmbedder()
    vectors = []
    for name in sys.argv[1:]:
        vectors.append(embedder.embed(_load16k(name)))
        print(f"{name}: {'none' if vectors[-1] is None else vectors[-1].shape}")
    if len(vectors) == 2 and all(v is not None for v in vectors):
        print(f"cosine: {float(vectors[0] @ vectors[1]):.4f}")
