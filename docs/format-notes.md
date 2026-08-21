# Format notes (living document)

Everything known or suspected about the Dolby MO print master format goes
here, with provenance. Claims without a disc image behind them are marked
*(unverified)*.

## Provenance of current knowledge

- Source: Matthew Dessem's description (Bluesky, 2026-08-20). No disc
  images in hand yet.

## Media & hardware

- SCSI magneto-optical discs, 1990s vintage. *(unverified: capacity,
  sector size — READ CAPACITY on a real disc will settle sector size;
  1024 B/sector is common for MO.)*
- Written in real time by the Dolby mastering unit during the mix;
  consumed by the Dolby optical printer when shooting the analog optical
  track. Both devices dedicated hardware, no longer manufactured.
- One known practitioner reads these via a modified Dolby printer, i.e.
  capturing real-time output rather than parsing files.

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
