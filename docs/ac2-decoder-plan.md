# AC-2 decoder plan (layer 2)

Goal: an open-source decoder for the AC-2 elementary stream carried in
printmaster `.SVA` files, sufficient to render the SR-encoded Lt/Rt as
PCM. Prototype in Python/numpy (fast iteration during RE); port to Rust
once bit-exact. Written before any real materials are in hand — revise
as they arrive.

Non-goal (for now): an encoder, and real-time performance.

## Why this is tractable

The architecture is public (see format-notes for verified references):
TDAC/MDCT filterbank, KBD-family window, block-floating-point spectrum
(exponents + mantissas), and backward-adaptive bit allocation
recomputed from the exponent envelope. What is *not* public is the
bitstream: field order, exponent coding, quantizer tables, allocation
constants. Backward adaptivity makes the allocation algorithm the
critical path — it must be bit-exact or the mantissa parse
desynchronizes immediately (a strong built-in correctness oracle:
wrong guesses collapse fast).

## Materials, in order of decisiveness

1. **`sva_*.ehx` CA10 firmware** — ships on every MO printmaster disc;
   also presumably held by the dolby-srd project. The decoder's own
   implementation: disassembly recovers exact tables and algorithms.
2. **Expired Dolby patents** (Fielder/Davidson family, ~1989–91; see
   the dolby-srd wiki's SR•D-Patents page and Google Patents) — far
   more implementation detail than the papers, often with tables.
3. **Known-plaintext streams** — the dolby-srd project's `tonefile.sva`
   (1 kHz tone, 18,750 frames; repo link currently 404s — ask), and any
   reel with matching output from Dino Everett's real-time rig.
4. **AC-3 as scaffolding** — ATSC A/52 is public and liba52/FFmpeg are
   open; AC-2's exponent/allocation machinery is its direct ancestor.

## Phases

### 0. Acquisition (blocked on disc one / dolby-srd contact)
Image a disc; locate `.SVA` and `sva_*.ehx` in the raw image (filename
crib from format-notes); extract with `modarch srd --extract` once the
filesystem is understood — or directly from frame runs before then.
In parallel: open an issue on dolby-srd introducing the imaging effort
and asking after the missing example files and firmware.

### 1. Stream characterization (no decoder needed)
Measure the elementary stream: payload rate is 430 B × 93.75/s
= 40,312.5 B/s ≈ 322.5 kbit/s → ~161 kbit/s/channel — consistent with
AC-2's documented 128–192 kbit/s/ch operating points, a useful sanity
check. Then: locate AC-2 frame boundaries within the payload (fixed
frame length is likely at a fixed bitrate), per-position entropy and
bit-change statistics across many frames of the 1 kHz tone (a static
tone should freeze the exponent envelope, making the exponent fields
leap out), diff frames of silence vs. tone.

### 2. Firmware disassembly (the decisive route)
Identify the DSP from `dsp-boot.bin`/`.ehx` binaries (era and Dolby
practice suggest a Motorola DSP56k-family part, but let the bytes
decide). Tooling: Ghidra with a community 56k processor module, or a
standalone DSP56k disassembler; radare2 has partial support. First
targets are the constant tables — window coefficients (match against
the KBD window formula), band structures, quantizer step sizes — then
the allocation loop. Tables are recognizable even when code is hard to
read.

### 3. Reference implementation
Build in layers, each testable alone: bit reader → exponent decode →
allocation (ported from firmware/patents) → mantissa dequant → inverse
TDAC with overlap-add (textbook; verifiable against a self-written
encoder path). Validate continuously against Phase 1 statistics: with
the 1 kHz tone, the first correct decode is unmistakable.

### 4. Validation
- `tonefile.sva`: decode must yield a clean 1 kHz tone at the correct
  level, both channels.
- An Everett-rig pair: decode of the file vs. capture of the hardware
  output for the same reel — sample-accurate correlation after
  alignment (his capture includes the analog chain, so expect high
  but not perfect correlation).
- Long-shot hardware oracle: a working CA10 AC-2 card or a Dolby
  DP501/DP502 codec.
- Output is still SR-encoded (layer 3); archive as-is and document.

## Risks

- **Variant ambiguity**: AC-2 vs. AC-2A (block-switched); the DMU-era
  cinema profile may differ from the satellite/Dolby-FAX profiles in
  the papers. The firmware resolves this authoritatively.
- **Allocation exactness**: the known hard part; firmware route
  mitigates. Patents may describe a near-miss version.
- **Unknown container details**: the 16-bit frame checksum (bytes
  476–477) and any packing between printmaster frames and AC-2 frames.
- Effort: filterbank/framing days; allocation core weeks (much less
  with clean firmware disassembly). Comparable precedent: FFmpeg's
  Dolby E decoder.

## Exit criteria

Phase 3 done when `tonefile.sva` (or first real reel) decodes to
artifact-free audio bit-stably across the whole file; project done
when an Everett-pair validates a full reel end-to-end.
