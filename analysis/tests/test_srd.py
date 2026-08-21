"""Frame parser/extractor tests against fabricated printmaster frames."""

import numpy as np

from modarch.srd import (
    AUDIO_MARKER,
    AUDIO_MARKER_OFFSET,
    FRAME_LEN,
    MAGIC,
    MAGIC_OFFSET,
    SYNC,
    extract_payload,
    find_frame_runs,
    parse_frame,
)


def _frame(rng, ordinal, reel=1, block=None, audio=True, ctype=0x0B) -> bytes:
    block = ordinal if block is None else block
    body = bytearray(rng.integers(0, 256, FRAME_LEN, dtype=np.uint8).tobytes())
    body[:4] = SYNC
    body[4] = ctype
    body[5] = 0x05 if ctype == 0x0B else 0x0A
    body[6] = 0x00 if audio else 0xFF
    body[7:10] = bytes(((ordinal >> 16) & 0xFF, (ordinal >> 8) & 0xFF, ordinal & 0xFF))
    if audio:
        body[AUDIO_MARKER_OFFSET : AUDIO_MARKER_OFFSET + 4] = AUDIO_MARKER
    else:
        body[AUDIO_MARKER_OFFSET : AUDIO_MARKER_OFFSET + 4] = b"\x00\x00\x00\x00"
    body[478] = (block >> 8) & 0xFF
    body[479] = (reel << 2) | ((block >> 16) & 0x03)
    body[480] = block & 0xFF
    body[MAGIC_OFFSET : MAGIC_OFFSET + len(MAGIC)] = MAGIC
    return bytes(body)


def test_parse_frame_fields():
    rng = np.random.default_rng(1)
    f = parse_frame(_frame(rng, ordinal=0x0201FF, reel=9, block=0x030201), 0)
    assert f is not None
    assert f.container_type == 0x0B and f.kind == "SR-D"
    assert f.ordinal == 0x0201FF
    assert f.reel == 9
    assert f.block == 0x030201
    assert f.is_audio and f.has_magic


def test_finds_run_and_extracts_ordered_payload():
    rng = np.random.default_rng(3)
    junk1 = rng.integers(0, 256, 3000, dtype=np.uint8).tobytes()
    junk2 = rng.integers(0, 256, 5000, dtype=np.uint8).tobytes()
    # 20 audio frames written out of order by ordinal, plus 2 filler frames
    ordinals = list(range(20))
    rng.shuffle(ordinals)
    frames = [_frame(rng, o) for o in ordinals]
    frames.insert(5, _frame(rng, 999, audio=False, ctype=0x09))
    frames.insert(10, _frame(rng, 998, audio=False, ctype=0x09))
    image = junk1 + b"".join(frames) + junk2

    runs = find_frame_runs(image)
    assert len(runs) == 1
    run = runs[0]
    assert run.start == len(junk1)
    assert run.count == 22
    assert run.audio_frames == 20
    assert run.kinds() == {"SR-D": 20, "SVA/filler": 2}
    assert run.reels() == {1}

    payload = extract_payload(image, run)
    assert len(payload) == 20 * 430
    # payload must come back in ordinal order regardless of disc order
    positions = {f.ordinal: f.offset for f in run.frames if f.is_audio}
    for i, ordinal in enumerate(sorted(positions)):
        expect = image[positions[ordinal] + 46 : positions[ordinal] + 476]
        assert payload[i * 430 : (i + 1) * 430] == expect


def test_no_false_positives_on_synth_image():
    from modarch.synth import make_synthetic_image

    image, _ = make_synthetic_image(seconds=2.0)
    assert find_frame_runs(image) == []
