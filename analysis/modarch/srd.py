"""Parser for Dolby printmaster frame structure (.SRD / .SVA files).

Byte-level layout per the dolby-srd reverse-engineering wiki
(https://github.com/davidferguson/dolby-srd/wiki, "Printmaster Format"
pages), documented from a DMU-created example file:

offset  len  field
------  ---  -----
0       4    sync word 37 2B 38 69
4       1    container type: 0x0B SR-D audio | 0x09 SVA/filler
5       1    container subtype: 0x05 SR-D audio | 0x0A SVA/filler
6       1    region marker: 0x00 audio | 0xFF filler
7       3    24-bit BE packet ordinal (stream position)
10      32   padding (0x00)
42      2    constant 0x44 0x99
44      2    block type (audio = 0xFB 0x0B)
46      430  audio payload (AC-3 for .SRD, AC-2 for .SVA)
476     2    checksum
478     3    reel/block: b478=(blk>>8)&0xFF, b479=(reel<<2)|((blk>>16)&3),
             b480=blk&0xFF
481     3    padding (0x00)
484     10   ASCII "DOLBY SR*D"
494     18   Dolby double-D logo bitmap (frame ends 64 61 DB 88)

Frames are 512 bytes at 93.75 frames/s (audio blocks). Valid audio
frames carry 0x4499FB0B at offset 42.
"""

from __future__ import annotations

from dataclasses import dataclass

FRAME_LEN = 512
SYNC = bytes((0x37, 0x2B, 0x38, 0x69))
MAGIC = b"DOLBY SR*D"
MAGIC_OFFSET = 484
AUDIO_MARKER = bytes((0x44, 0x99, 0xFB, 0x0B))
AUDIO_MARKER_OFFSET = 42
PAYLOAD_SLICE = slice(46, 476)
FRAME_RATE = 93.75  # audio frames per second per the wiki

CONTAINER_TYPES = {0x0B: "SR-D", 0x09: "SVA/filler"}


@dataclass
class Frame:
    offset: int
    container_type: int
    subtype: int
    region: int          # 0x00 audio | 0xFF filler
    ordinal: int         # 24-bit BE stream position
    reel: int
    block: int
    is_audio: bool       # 0x4499FB0B marker present and region == 0x00
    has_magic: bool

    @property
    def kind(self) -> str:
        return CONTAINER_TYPES.get(self.container_type, f"0x{self.container_type:02X}?")


def parse_frame(data: bytes, off: int) -> Frame | None:
    f = data[off : off + FRAME_LEN]
    if len(f) < FRAME_LEN or f[:4] != SYNC:
        return None
    block = (f[479] & 0x03) << 16 | f[478] << 8 | f[480]
    return Frame(
        offset=off,
        container_type=f[4],
        subtype=f[5],
        region=f[6],
        ordinal=f[7] << 16 | f[8] << 8 | f[9],
        reel=f[479] >> 2,
        block=block,
        is_audio=(f[AUDIO_MARKER_OFFSET : AUDIO_MARKER_OFFSET + 4] == AUDIO_MARKER and f[6] == 0x00),
        has_magic=(f[MAGIC_OFFSET : MAGIC_OFFSET + len(MAGIC)] == MAGIC),
    )


@dataclass
class FrameRun:
    start: int
    frames: list[Frame]

    @property
    def count(self) -> int:
        return len(self.frames)

    @property
    def audio_frames(self) -> int:
        return sum(1 for f in self.frames if f.is_audio)

    @property
    def magic_hits(self) -> int:
        return sum(1 for f in self.frames if f.has_magic)

    @property
    def seconds(self) -> float:
        return self.audio_frames / FRAME_RATE

    def kinds(self) -> dict[str, int]:
        out: dict[str, int] = {}
        for f in self.frames:
            out[f.kind] = out.get(f.kind, 0) + 1
        return out

    def reels(self) -> set[int]:
        return {f.reel for f in self.frames if f.is_audio}

    def describe(self) -> str:
        kinds = ", ".join(f"{k}:{v}" for k, v in sorted(self.kinds().items()))
        reels = ",".join(str(r) for r in sorted(self.reels())) or "-"
        return (
            f"{self.start:#010x}: {self.count} frames ({kinds}); "
            f"{self.audio_frames} audio ({self.seconds:.1f}s @ {FRAME_RATE} fps), "
            f"reel(s) {reels}, magic {self.magic_hits}/{self.count}"
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
        frames: list[Frame] = []
        o = off
        while o in hits:
            seen.add(o)
            fr = parse_frame(data, o)
            if fr is None:
                break
            frames.append(fr)
            o += FRAME_LEN
        if len(frames) >= min_frames:
            runs.append(FrameRun(off, frames))
    return runs


def extract_payload(data: bytes, run: FrameRun) -> bytes:
    """Concatenated 430-byte payloads of the run's valid audio frames, in
    ordinal order — the raw AC-3 (.SRD) or AC-2 (.SVA) elementary stream."""
    audio = sorted((f for f in run.frames if f.is_audio), key=lambda f: f.ordinal)
    return b"".join(data[f.offset + PAYLOAD_SLICE.start : f.offset + PAYLOAD_SLICE.stop] for f in audio)
