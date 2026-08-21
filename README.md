# modarch — Dolby MO print master archaeology

Tooling for imaging and analyzing 1990s Dolby print masters written to
magneto-optical (MO) discs, with the goal of documenting the proprietary
disc/file format and recovering the Lt/Rt program as PCM.

Two workstreams, deliberately decoupled:

| | runs on | language | purpose |
|---|---|---|---|
| `imager/` | Windows Vista (air-gapped) | C (MinGW-w64, i686, static) | byte-exact raw imaging of MO discs over SCSI passthrough |
| `analysis/` | any modern machine | Python 3.10+ | format archaeology on the resulting images |

The Vista machine only ever runs the imager. Everything else iterates on
images, so drive time on real discs is spent exactly once per disc.

## Ground rules for handling discs

1. **Write-protect the cartridge** (slide the tab) before it goes anywhere
   near a drive.
2. **Image first, experiment never** — no tool other than the imager touches
   an original disc.
3. **Image every disc twice** and compare SHA-256 before trusting the image.
4. Debut new imager builds on a sacrificial/ordinary MO disc, not a print
   master.

## imager

Single static executable, no dependencies, no network. Uses SPTI
(`SCSI_PASS_THROUGH_DIRECT`, present since NT) so it works on Vista.
Run from an elevated command prompt:

```
imager.exe \\.\PhysicalDrive2 out\disc01
```

produces `disc01.img` (raw sectors), `disc01.log` (INQUIRY identity,
capacity, per-sector read failures), `disc01.sha256`.

It queries the drive for the true sector size via READ CAPACITY — MO media
is commonly 1024 bytes/sector, not 512 — and reads with per-sector retry,
zero-filling and logging anything unrecoverable.

Build locally (Linux/macOS with mingw installed) with `make -C imager`, or
let CI do it: every push builds `imager.exe` as a GitHub Actions artifact.
Toolchain notes: modern MSVC and Rust have dropped Vista, so this is
C, cross-compiled with MinGW-w64 against **msvcrt** (not UCRT), `-static`,
i686 so it runs on 32- and 64-bit Vista alike.

## analysis

```
pip install -e analysis/
modarch synth  demo.img            # build a synthetic test image
modarch map    demo.img            # entropy/structure map of an image
modarch srd    disc.img            # find Dolby printmaster frame runs
modarch srd    disc.img --extract reel   # ...and extract raw AC-2/AC-3 streams
modarch prospect demo.img -o out/  # sweep PCM interpretations, emit WAVs
```

`srd` is the primary tool for real discs: it parses the documented
512-byte printmaster frame structure (sync word, container type,
ordinals, reel/block numbers) and reassembles the raw elementary
streams from the audio frames. `prospect` brute-forces the small space
of PCM packings (bit depth × endianness × channel interleave × byte
offset), scores each window by audio-likeness (lag-1 autocorrelation,
entropy, clipping), and writes the top candidates as WAVs — retained as
a negative control and for any uncompressed data.

`synth` exists so the toolkit can be validated blind: it plants stereo
audio in an unusual packing inside a plausible fake disc image, and the
test suite asserts `prospect` finds it.

## What we know (see docs/)

- **[docs/format-notes.md](docs/format-notes.md)** — the living format
  document: everything known or suspected, with provenance.
- **[docs/vista-runbook.md](docs/vista-runbook.md)** — field procedure
  for imaging sessions on the air-gapped Vista machine.

Headlines: the discs are DMU printmasters, and much of the file format
(though not the MO filesystem) is documented by the
[dolby-srd](https://github.com/davidferguson/dolby-srd) project —
per-reel `.SRD` (5.1, AC-3-family) and `.SVA` (Lt/Rt, SR-encoded then
**AC-2 compressed — not raw PCM**) files built from 512-byte frames,
plus plaintext logs and, notably, the CA10 decoder DSP firmware on the
disc itself. Extraction is therefore layered: MO filesystem (this
project's contribution) → AC-2 decode (no open decoder exists yet; see
format-notes for the feasibility assessment) → Dolby SR decode.

## License

[Blue Oak Model License 1.0.0](LICENSE.md).
