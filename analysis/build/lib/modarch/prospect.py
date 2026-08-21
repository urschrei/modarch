"""Brute-force PCM interpretation sweep ("audio prospecting").

Uncompressed audio is self-identifying: decode a region under every
plausible packing (bit depth x endianness x byte offset, stereo
interleave assumed) and score the result by audio-likeness. Real audio
decoded correctly has high lag-1 autocorrelation (samples are smooth);
wrong packings scramble byte significance and the score collapses.
"""

from __future__ import annotations

import wave
from dataclasses import dataclass, field

import numpy as np

from modarch.entropy import entropy_map


@dataclass
class Candidate:
    region_offset: int          # byte offset of region in the image
    byte_offset: int            # additional offset into the region
    bit_depth: int              # 16 or 24
    endian: str                 # '<' or '>'
    channels: int
    score: float
    samples: np.ndarray = field(repr=False)   # float32 in [-1, 1], (n, ch)

    def describe(self) -> str:
        e = "LE" if self.endian == "<" else "BE"
        return (
            f"region {self.region_offset:#x} +{self.byte_offset} "
            f"{self.bit_depth}-bit {e} {self.channels}ch  score={self.score:.4f}"
        )


def _decode(buf: bytes, bit_depth: int, endian: str, channels: int) -> np.ndarray:
    """Decode raw bytes to float32 (n, channels) in [-1, 1]."""
    if bit_depth == 16:
        n = len(buf) // 2 * 2
        x = np.frombuffer(buf[:n], dtype=np.dtype(endian + "i2")).astype(np.float32)
        x /= 32768.0
    elif bit_depth == 24:
        n = len(buf) // 3 * 3
        b = np.frombuffer(buf[:n], dtype=np.uint8).reshape(-1, 3).astype(np.uint32)
        if endian == ">":
            v = (b[:, 0] << 16) | (b[:, 1] << 8) | b[:, 2]
        else:
            v = (b[:, 2] << 16) | (b[:, 1] << 8) | b[:, 0]
        v = v.astype(np.int64)
        v[v >= 1 << 23] -= 1 << 24
        x = v.astype(np.float32) / float(1 << 23)
    else:
        raise ValueError(f"unsupported bit depth {bit_depth}")
    n = len(x) // channels * channels
    return x[:n].reshape(-1, channels)


def _score(x: np.ndarray) -> float:
    """Audio-likeness of decoded samples: mean per-channel lag-1
    autocorrelation, with penalties for clipping and near-silence."""
    if x.shape[0] < 1024:
        return 0.0
    score = 0.0
    for ch in range(x.shape[1]):
        c = x[:, ch] - x[:, ch].mean()
        s = c.std()
        if s < 1e-6:
            continue                       # silent channel: no evidence
        r = float(np.dot(c[:-1], c[1:]) / (len(c) - 1) / (s * s))
        clip = float((np.abs(x[:, ch]) > 0.999).mean())
        score += r - 5.0 * clip
    return score / x.shape[1]


def audio_regions(data: bytes, block: int = 2048, min_blocks: int = 16) -> list[tuple[int, int]]:
    """Contiguous (offset, length) runs classified as plausible audio."""
    stats = entropy_map(data, block=block)
    runs: list[tuple[int, int]] = []
    i = 0
    while i < len(stats):
        if stats[i].label == "audio?":
            j = i
            while j < len(stats) and stats[j].label == "audio?":
                j += 1
            if j - i >= min_blocks:
                runs.append((stats[i].offset, (j - i) * block))
            i = j
        else:
            i += 1
    return runs


def prospect(
    data: bytes,
    regions: list[tuple[int, int]] | None = None,
    bit_depths: tuple[int, ...] = (16, 24),
    endians: str = "<>",
    channels: int = 2,
    window: int = 1 << 20,
) -> list[Candidate]:
    """Sweep packings over each candidate region; return ranked candidates."""
    if regions is None:
        regions = audio_regions(data)
    out: list[Candidate] = []
    for off, length in regions:
        chunk = data[off : off + min(length, window)]
        for depth in bit_depths:
            frame = depth // 8 * channels
            for endian in endians:
                for boff in range(frame):
                    x = _decode(chunk[boff:], depth, endian, channels)
                    out.append(
                        Candidate(off, boff, depth, endian, channels, _score(x), x)
                    )
    out.sort(key=lambda c: c.score, reverse=True)
    return out


def export_wav(cand: Candidate, path: str, sample_rate: int = 48000) -> None:
    """Write a candidate as 16-bit WAV for listening. The sample rate is a
    guess until confirmed; pitch will be off if wrong, but identity is
    usually obvious anyway."""
    pcm = np.clip(cand.samples, -1.0, 1.0)
    pcm16 = (pcm * 32767.0).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(cand.channels)
        w.setsampwidth(2)
        w.setframerate(sample_rate)
        w.writeframes(pcm16.tobytes())
