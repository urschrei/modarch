"""Frame-run scanner test against fabricated printmaster frames."""

import numpy as np

from modarch.srd import FRAME_LEN, MAGIC, MAGIC_OFFSET, SYNC, find_frame_runs


def _frame(rng) -> bytes:
    body = bytearray(rng.integers(0, 256, FRAME_LEN, dtype=np.uint8).tobytes())
    body[: len(SYNC)] = SYNC
    body[MAGIC_OFFSET : MAGIC_OFFSET + len(MAGIC)] = MAGIC
    return bytes(body)


def test_finds_planted_run():
    rng = np.random.default_rng(3)
    junk1 = rng.integers(0, 256, 3000, dtype=np.uint8).tobytes()
    junk2 = rng.integers(0, 256, 5000, dtype=np.uint8).tobytes()
    frames = b"".join(_frame(rng) for _ in range(20))
    image = junk1 + frames + junk2

    runs = find_frame_runs(image)
    assert len(runs) == 1
    run = runs[0]
    assert run.start == len(junk1)
    assert run.count == 20
    assert run.magic_hits == 20
    assert abs(run.seconds - 20 / 93.75) < 1e-9


def test_no_false_positives_on_synth_image():
    from modarch.synth import make_synthetic_image

    image, _ = make_synthetic_image(seconds=2.0)
    assert find_frame_runs(image) == []
