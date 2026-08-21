"""Synthetic 'MO disc' image generator.

Exists so the analysis toolkit can be validated blind: plants stereo
audio in a non-obvious packing inside a plausible fake disc layout
(label sector, ASCII TOC, blank gaps, a coded/high-entropy region), and
returns ground truth so tests can assert the prospector finds it.

Nothing here is a guess about the real Dolby format — it is a test rig.
"""

from __future__ import annotations

import numpy as np

SECTOR = 1024


def _audio_like(n: int, rate: int, seed: int) -> np.ndarray:
    """Stereo float32 that statistically resembles program audio:
    band-limited noise (speech-ish envelope) plus swept tone."""
    rng = np.random.default_rng(seed)
    t = np.arange(n) / rate
    # brownian-ish noise: cumulative sum of white noise, detrended
    w = rng.standard_normal(n + 1)
    b = np.cumsum(w)[1:]
    b -= np.linspace(b[0], b[-1], n)
    b /= np.abs(b).max() + 1e-9
    sweep = 0.3 * np.sin(2 * np.pi * (200 + 40 * t) * t)
    env = 0.5 + 0.5 * np.sin(2 * np.pi * 0.7 * t) ** 2
    left = env * (0.7 * b + sweep)
    right = 0.85 * left + 0.15 * np.roll(left, 17)   # correlated stereo, as Lt/Rt is
    x = np.stack([left, right], axis=1).astype(np.float32)
    return 0.8 * x / np.abs(x).max()


def _pack24be(x: np.ndarray) -> bytes:
    v = np.clip((x * (1 << 23)).round(), -(1 << 23), (1 << 23) - 1).astype(np.int64)
    v = np.where(v < 0, v + (1 << 24), v).astype(np.uint32).ravel()
    out = np.empty((v.size, 3), dtype=np.uint8)
    out[:, 0] = (v >> 16) & 0xFF
    out[:, 1] = (v >> 8) & 0xFF
    out[:, 2] = v & 0xFF
    return out.tobytes()


def make_synthetic_image(
    seconds: float = 4.0, rate: int = 48000, seed: int = 7
) -> tuple[bytes, dict]:
    """Return (image_bytes, meta). Audio planted as 24-bit BE interleaved
    stereo, sector-aligned, surrounded by realistic non-audio regions."""
    rng = np.random.default_rng(seed)
    n = int(seconds * rate)
    audio = _audio_like(n, rate, seed)
    payload = _pack24be(audio)

    label = b"DOLBY MO ARCHIVE TEST VOLUME 001".ljust(SECTOR, b"\x00")
    toc = (
        b"FILE PRINTMASTER.PM  START 00000040 LEN %08X RATE 48000\n"
        % len(payload)
    ).ljust(SECTOR, b"\x00") * 4
    blank = b"\x00" * (SECTOR * 59)   # label(1) + toc(4) + blank(59) = 0x40 sectors
    payload_offset = len(label) + len(toc) + len(blank)
    assert payload_offset == 0x40 * SECTOR

    pad = (-len(payload)) % SECTOR
    coded = rng.integers(0, 256, SECTOR * 64, dtype=np.uint8).tobytes()
    tail = b"\x00" * (SECTOR * 16)

    image = label + toc + blank + payload + b"\x00" * pad + coded + tail
    meta = {
        "payload_offset": payload_offset,
        "payload_len": len(payload),
        "bit_depth": 24,
        "endian": ">",
        "channels": 2,
        "rate": rate,
        "audio": audio,
    }
    return image, meta
