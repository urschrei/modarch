"""Block-wise entropy / structure mapping of a raw disc image.

The point: filesystem metadata, ASCII labels, PCM audio, compressed/coded
data, and blank regions all have distinct byte statistics. A coarse map is
usually enough to see the disc's gross layout before any real parsing.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np


@dataclass
class BlockStats:
    offset: int
    entropy: float        # Shannon entropy, bits/byte (0..8)
    printable: float      # fraction of printable ASCII
    zeros: float          # fraction of 0x00 bytes
    label: str            # coarse classification


def _classify(H: float, printable: float, zeros: float) -> str:
    if zeros > 0.98:
        return "blank"
    nonzero = 1.0 - zeros
    # judge printability among the non-zero bytes: metadata sectors are
    # typically short ASCII records padded out with zeros
    if nonzero > 0.005 and printable / nonzero > 0.8:
        return "text/meta"
    if H > 7.9:
        return "coded/compressed"   # or encrypted — flat spectrum
    if 5.0 < H <= 7.9:
        return "audio?"             # PCM lives here: high but not flat
    return "structured"


def entropy_map(data: bytes | np.ndarray, block: int = 2048) -> list[BlockStats]:
    a = np.frombuffer(bytes(data), dtype=np.uint8)
    out: list[BlockStats] = []
    for off in range(0, len(a) - len(a) % block, block):
        b = a[off : off + block]
        counts = np.bincount(b, minlength=256).astype(np.float64)
        p = counts / counts.sum()
        nz = p[p > 0]
        H = float(-(nz * np.log2(nz)).sum())
        printable = float(((b >= 0x20) & (b < 0x7F)).mean())
        zeros = float((b == 0).mean())
        out.append(BlockStats(off, H, printable, zeros, _classify(H, printable, zeros)))
    return out


def summarize(stats: list[BlockStats]) -> str:
    """Run-length-encoded one-line-per-region description."""
    lines = []
    i = 0
    while i < len(stats):
        j = i
        while j < len(stats) and stats[j].label == stats[i].label:
            j += 1
        H = float(np.mean([s.entropy for s in stats[i:j]]))
        lines.append(
            f"{stats[i].offset:#010x}..{stats[j-1].offset + 2048:#010x}  "
            f"{stats[i].label:<17} H≈{H:.2f}"
        )
        i = j
    return "\n".join(lines)
