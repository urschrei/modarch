"""Detector for Dolby printmaster frame structure (.SRD / .SVA files).

Per the dolby-srd reverse-engineering wiki
(https://github.com/davidferguson/dolby-srd/wiki), DMU printmaster audio
files are built from 512-byte frames:

- frame sync word 0x37 0x2B 0x38 0x69 at frame start
- ASCII "DOLBY SR*D" at byte offset 484 within the frame
- 93.75 frames per second
- .SRD payload: EC9/EC11-wrapped AC-3 (5.1); .SVA payload: AC-2-coded,
  SR-encoded Lt/Rt

This scanner finds sync words in a raw disc image, groups them into runs
with 512-byte stride, and cross-checks the "DOLBY SR*D" marker — locating
printmaster files without needing to understand the MO filesystem first.
"""

from __future__ import annotations

from dataclasses import dataclass

FRAME_LEN = 512
SYNC = bytes((0x37, 0x2B, 0x38, 0x69))
MAGIC = b"DOLBY SR*D"
MAGIC_OFFSET = 484
FRAME_RATE = 93.75  # frames per second per the wiki


@dataclass
class FrameRun:
    start: int          # byte offset of first frame
    count: int          # consecutive frames at 512-byte stride
    magic_hits: int     # frames whose MAGIC_OFFSET bytes match "DOLBY SR*D"

    @property
    def seconds(self) -> float:
        return self.count / FRAME_RATE

    def describe(self) -> str:
        return (
            f"{self.start:#010x}: {self.count} frames "
            f"({self.seconds:.1f}s @ {FRAME_RATE} fps), "
            f"magic in {self.magic_hits}/{self.count}"
        )


def find_sync_offsets(data: bytes) -> list[int]:
    out, i = [], data.find(SYNC)
    while i != -1:
        out.append(i)
        i = data.find(SYNC, i + 1)
    return out


def find_frame_runs(data: bytes, min_frames: int = 4) -> list[FrameRun]:
    """Group sync hits into runs with FRAME_LEN stride; a run of stray
    matches is statistically implausible past a few frames."""
    offsets = find_sync_offsets(data)
    hits = set(offsets)
    runs: list[FrameRun] = []
    seen: set[int] = set()
    for off in offsets:
        if off in seen:
            continue
        count = 0
        o = off
        while o in hits:
            seen.add(o)
            count += 1
            o += FRAME_LEN
        if count >= min_frames:
            magic = sum(
                1
                for k in range(count)
                if data[off + k * FRAME_LEN + MAGIC_OFFSET : off + k * FRAME_LEN + MAGIC_OFFSET + len(MAGIC)]
                == MAGIC
            )
            runs.append(FrameRun(off, count, magic))
    return runs
