# Format notes (living document)

Everything known or suspected about the Dolby MO print master format goes
here, with provenance. Claims without a disc image behind them are marked
*(unverified)*.

## Provenance of current knowledge

- Source: Matthew Dessem's description (Bluesky, 2026-08-20). No disc
  images in hand yet.
- Source: the [dolby-srd reverse-engineering project]
  (https://github.com/davidferguson/dolby-srd) and its wiki, which
  documents the DMU printmaster format from example files — see
  "Printmaster format (from the dolby-srd wiki)" below. Their gap is
  the MO disc filesystem, which is exactly what our imaging effort can
  supply.

## Media & hardware

- SCSI magneto-optical discs, 1990s vintage. *(unverified: capacity,
  sector size — READ CAPACITY on a real disc will settle sector size;
  1024 B/sector is common for MO.)*
- Written in real time by the Dolby mastering unit during the mix;
  consumed by the Dolby optical printer when shooting the analog optical
  track. Both devices dedicated hardware, no longer manufactured.
- One known practitioner reads these via a modified Dolby printer, i.e.
  capturing real-time output rather than parsing files — reportedly
  Dino Everett (archivist, USC Hugh M. Hefner Moving Image Archive)
  *(unverified — per Steph, 2026-08-21)*. Relevant for ground truth: a
  disc captured on that rig AND imaged by us would let file-level
  decoding be validated against known-good real-time output.

## Printmaster format (from the dolby-srd wiki)

Documented by https://github.com/davidferguson/dolby-srd/wiki from
example printmaster files (not from MO discs — they postdate the MO era
or were obtained as files). Treat as strong prior, to be confirmed
against our images:

- A printmaster is a set of files **per reel** (~2000 ft / ~22 min max):
  - `.SRD` — the 5.1 audio, AC-3 codec, wrapped in an "EC9/EC11"
    encoding (wrapper not yet documented).
  - `.SVA` — optional; the **Lt/Rt for the analog Stereo Variable Area
    track: SR-encoded, then compressed with the Dolby AC-2 codec**.
    Not raw PCM.
  - `.CAT` (project info, DOS-era), `.LOG` (plaintext operation log
    with timestamps and block counts — likely what was seen in the hex
    editor), `.MEP` (project name, encoding mode).
- `.SRD`/`.SVA` files are built from **512-byte frames**: sync word
  `37 2B 38 69` at frame start, ASCII `DOLBY SR*D` at byte offset 484,
  at **93.75 frames/second**. `modarch srd <image>` scans for exactly
  this structure.
- Storage was MO disc originally ("a few gigabytes"); later Compact
  Flash and plain file delivery.
- Decoder firmware for the CA10 exists for both codecs
  (`srd_*.ehx` = AC-3, `sva_*.ehx` = AC-2 DSP programs) — and **ships on
  the MO disc itself**: the printmaster includes `srd_e5.ehx`,
  `srd_d4.ehx`, `sva_e3.ehx`, `sva_d2.ehx`, `sva_d1.ehx`, plus
  `dsp-boot.bin`, `dsp.ldc`, `dyn.ldr` and a zero-byte `PROTECT.DMU`.
  The CA10 auto-updates its coder DSPs from the disc. So **every disc we
  image should contain the AC-2 decoder firmware** — the primary
  disassembly target for layer 2.
- Full 512-byte frame layout is documented (see the docstring in
  `analysis/modarch/srd.py`): sync `37 2B 38 69`; container type at
  byte 4 (0x0B SR-D / 0x09 SVA-or-filler); 24-bit BE ordinal at 7–9;
  validity marker `44 99 FB 0B` at 42; **430-byte audio payload at
  bytes 46–475**; checksum at 476–477; reel/block packed at 478–480;
  `DOLBY SR*D` at 484; Dolby logo bitmap to end (`… 64 61 DB 88`).
  `modarch srd <image> --extract` parses frames and reassembles the
  raw AC-2/AC-3 elementary stream in ordinal order.
- EC9/EC11 is believed close to AC-3: the DA20 decodes it on a Zoran
  ZR38500, a stock "Six-Channel Dolby Digital Surround Processor" used
  in DVD players — softening the layer-2 problem for the 5.1 side.
- The wiki links example DMU files (`tonefile.sva`/`.srd`, 1 kHz tone,
  18,750 frames) but the repo paths 404 — worth an issue/contact to the
  project. The CA10's AC-2 card is a generic "Cat. No. 621" DSP card;
  DSP architecture not yet identified (the `.ehx` binary will tell).

Consequences for this project:

- The Lt/Rt extraction is a **three-layer** problem: MO filesystem →
  AC-2 decode → (optionally) SR decode. Layer 1 is ours; layer 2 has
  no known open-source decoder (AC-3 is well supported — liba52,
  FFmpeg — but AC-2 is not; the algorithm is described in AES
  literature: Davidson, Fielder & Antill's AES papers on AC-2, and
  Fielder et al., "AC-2 and AC-3: Low-Complexity Transform-Based Audio
  Coding", AES 17th Int. Conference, 1999). Layer 3 is the known SR
  caveat.
- The PCM prospector remains useful as a negative control and for any
  uncompressed data, but the primary detection tool for real discs is
  the frame-run scanner.
- Archival deliverable unchanged: byte-exact disc images plus
  extracted per-reel files; decode is a separable later problem.

## Format hypotheses

- Proprietary filesystem, but plaintext has been observed in a hex dump
  *(unverified)* — argues against whole-disc encryption and for a simple
  TOC + extents layout typical of streaming writers.
- Real-time creation implies streaming writes: expect large contiguous
  extents, likely sector-aligned, minimal fragmentation.
- Expected payload: Lt/Rt print master. Optical print masters carry
  Dolby SR (or A-type) noise-reduction encoding applied before printing —
  so recovered PCM will be NR-encoded. Archive it as such and document it;
  flat playback will sound wrong. Sample rate and word length unknown
  *(unverified — prospector sweeps these)*.
- Possibility the disc also carries data for the digital (SR•D) track;
  AC-3 frames would betray themselves with 0x0B77 sync words.

## Layer 2: AC-2 decoding feasibility

Assessment 2026-08-21. Writing an AC-2 decoder from public sources is
feasible; the papers give the architecture, but not the bitstream.

What the literature provides:

- Filterbank: TDAC/MDCT — Princen & Bradley, "Analysis/Synthesis filter
  bank design based on time domain aliasing cancellation," IEEE Trans.
  ASSP 34(5), 1986. DOI: 10.1109/TASSP.1986.1164954 (verified).
- Codec architecture: Davidson, Fielder & Antill, "High-quality audio
  transform coding at 128 kbits/s," ICASSP 1990.
  DOI: 10.1109/ICASSP.1990.116136 (verified). Block-floating-point
  spectrum (exponents + mantissas); **backward-adaptive bit allocation**
  recomputed by the decoder from the exponent envelope — no allocation
  side info in the stream, so a decoder must match the algorithm
  bit-exactly or produce garbage.
- Block switching: Bosi, Davidson & Fielder, ASPAA 1991.
  DOI: 10.1109/ASPAA.1991.634112 (verified).
- Overview: Fielder, Bosi, Davidson, Davis, Todd & Vernon, "AC-2 and
  AC-3: Low-Complexity Transform-Based Audio Coding," in *Collected
  Papers on Digital Audio Bit-Rate Reduction*, AES, 1996 (no DOI —
  AES publications of the era were not assigned them).

Rate cross-check: 93.75 frames/s × 512 B = 48,000 B/s = 384 kbit/s for
the .SVA stream = 192 kbit/s/channel at 48 kHz — inside AC-2's
documented operating range. The wiki numbers cohere.

What closes the bitstream gap, in rough order of decisiveness:

1. The CA10 DSP firmware (`sva_*.ehx` in the dolby-srd repo):
   disassembly recovers the actual tables and allocation algorithm.
2. The expired Dolby patents (Fielder/Davidson family, filed
   ~1989–91) — far more implementation detail than the papers.
3. AC-3 as a Rosetta stone: the public ATSC A/52 spec and liba52/FFmpeg
   descend directly from AC-2's exponent/allocation machinery.
4. Known-plaintext: the wiki's example .SVA (1 kHz tone); reels
   captured on Everett's rig give input/output pairs.

Estimated effort: framing + filterbank are textbook (days);
exponent/allocation core is careful RE (weeks, less with firmware
disassembly). Comparable to FFmpeg's Dolby E effort. Fully separable:
archival deliverables (images + extracted files) do not wait on it.

## Findings from real images

*(none yet)*

## Open questions

- Actual sector size and capacity; any vendor-specific SCSI behavior.
- The MO filesystem layout itself — the one layer nobody has documented.
  Expected contents per the wiki: per-reel `.SRD`/`.SVA`/`.CAT`/`.LOG`,
  a `.MEP` project file, and the CA10 firmware set
  (`*.ehx`, `dsp-boot.bin`, `dsp.ldc`, `dyn.ldr`, `PROTECT.DMU`) —
  finding these names in a raw image is the fastest way to reverse the
  directory structure.
- Reel alignment/timecode: block ordinals and packed reel numbers exist
  in-frame (bytes 7–9 and 478–480); is there anything at filesystem
  level beyond that?
- Any per-sector framing/ECC beyond the drive's own? (The CA10 adds the
  film-side error correction itself, so the printmaster likely carries
  little or none — per the wiki's CA10 card analysis.)
- The frame checksum at bytes 476–477: algorithm unknown; recoverable
  by brute-forcing common 16-bit checksums over a real frame corpus.

Answered since first draft: audio is per-reel files, not one stream
(wiki); Lt/Rt is AC-2-coded, not PCM (wiki); plaintext seen in hex
dumps is consistent with the `.LOG` files (wiki).
