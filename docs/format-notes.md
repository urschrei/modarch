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
  (`srd_*.ehx` = AC-3, `sva_*.ehx` = AC-2 DSP programs).

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

## Findings from real images

*(none yet)*

## Open questions

- Actual sector size and capacity; any vendor-specific SCSI behavior.
- Single audio stream per disc or multiple reels/files?
- Timecode/sync metadata — where does reel alignment live?
- Any per-sector framing/ECC beyond the drive's own?
