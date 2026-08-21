"""Blind-recovery test: the prospector must find planted audio without
being told where it is or how it is packed."""

import numpy as np
import pytest
from modarch.entropy import entropy_map
from modarch.prospect import audio_regions, prospect
from modarch.synth import make_synthetic_image


@pytest.fixture(scope="module")
def synth():
    return make_synthetic_image(seconds=4.0)


def test_entropy_map_sees_layout(synth):
    image, _ = synth
    stats = entropy_map(image)
    labels = {s.label for s in stats}
    assert "text/meta" in labels
    assert "blank" in labels
    assert "audio?" in labels
    assert "coded/compressed" in labels


def test_audio_region_located(synth):
    image, meta = synth
    regions = audio_regions(image)
    assert regions, "no audio-like region found"
    off, _ = regions[0]
    lo, hi = meta["payload_offset"], meta["payload_offset"] + meta["payload_len"]
    assert lo <= off < hi


def test_blind_recovery(synth):
    image, meta = synth
    top = prospect(image)[0]

    assert top.bit_depth == meta["bit_depth"]
    assert top.endian == meta["endian"]
    # any sample-aligned offset is a valid decode (odd sample counts just
    # swap which channel is 'left'), so require sample alignment only
    assert top.byte_offset % (meta["bit_depth"] // 8) == 0

    # correlate recovered audio against ground truth on the interleaved
    # sample stream, which is invariant to channel-role swaps
    start = (top.region_offset + top.byte_offset - meta["payload_offset"]) // 3
    truth = meta["audio"].reshape(-1)[start:]
    rec = top.samples.reshape(-1)
    n = min(len(truth), len(rec))
    assert n > 96000, "recovered window too short"
    r = np.corrcoef(truth[:n], rec[:n])[0, 1]
    assert r > 0.99, f"stream correlation {r}"


def test_wrong_packings_score_lower(synth):
    image, meta = synth
    cands = prospect(image)
    top = cands[0]
    wrong = [
        c
        for c in cands
        if c.bit_depth != meta["bit_depth"] or c.endian != meta["endian"]
    ]
    assert wrong and top.score > max(c.score for c in wrong)
