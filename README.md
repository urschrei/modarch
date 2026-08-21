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

produces `disc01.img` (raw sectors), `disc01.log`, `disc01.sha256`.

It queries the drive for the true sector size via READ CAPACITY — MO media
is commonly 1024 bytes/sector, not 512 — and reads in chunks of up to
64 KiB (less if the adapter reports a smaller limit). When a chunk read
fails it re-reads that chunk one sector at a time with retries,
zero-filling and logging anything unrecoverable. A short transfer, or a
write error on the image file, is a hard failure, not a silent one.

The log is deliberately verbose, because the Vista machine is air-gapped
and the log is the only record of what the drive did. It contains:

- the imager build id, command line, machine clock and Windows version
- the INQUIRY response (vendor/product/revision, device type, removable
  bit) and its raw bytes
- readiness polling (TEST UNIT READY) and READ CAPACITY results, raw
- the MODE SENSE response, raw, with the medium type, block descriptor
  and the write-protect bit — a warning is logged if the medium is not
  reported write-protected
- the adapter's bus type and transfer limits, and the chunk size chosen
- every chunk read that fell back to sector-by-sector reads, with the
  cause (Win32 error, SCSI status and sense, or short transfer)
- every sector that needed a retry, and every sector that was given up
  on and zero-filled
- bytes written, elapsed time, throughput, and the SHA-256

Exit codes: 0 clean, 1 setup or write failure, 2 usage, 3 completed
with unreadable sectors.

Write the image to an NTFS volume. A FAT32 destination fails at 4 GiB;
the imager detects this and stops, but the run is wasted.

Build locally (Linux/macOS with mingw installed) with `make -C imager`, or
let CI do it: every push builds `imager.exe` as a GitHub Actions artifact.
Toolchain notes: modern MSVC and Rust have dropped Vista, so this is
C, cross-compiled with MinGW-w64 against **msvcrt** (not UCRT), `-static`,
i686 so it runs on 32- and 64-bit Vista alike. Homebrew's `mingw-w64` on
macOS links UCRT: it is fine for compile checks, but `make -C imager
check-vista` will fail on its output, and that binary must not go to the
Vista machine. Use the CI artifact.

## analysis

```
pip install -e analysis/
modarch synth  demo.img            # build a synthetic test image
modarch map    demo.img            # entropy/structure map of an image
modarch prospect demo.img -o out/  # sweep PCM interpretations, emit WAVs
```

`prospect` brute-forces the small space of PCM packings
(bit depth × endianness × channel interleave × byte offset), scores each
window by audio-likeness (lag-1 autocorrelation, entropy, clipping), and
writes the top candidates as WAVs for listening.

`synth` exists so the toolkit can be validated blind: it plants stereo
audio in an unusual packing inside a plausible fake disc image, and the
test suite asserts `prospect` finds it.

## What we think we know (see docs/format-notes.md)

- Media: SCSI magneto-optical, written by Dolby mastering hardware in
  real time during the mix; read by the Dolby optical printer.
- Proprietary format including the filesystem; plaintext strings have been
  observed in a hex dump, which argues against encryption.
- Expected payload includes the Lt/Rt print master. NB: an optical print
  master's Lt/Rt is almost certainly Dolby SR (or A-type) encoded —
  extraction yields SR-encoded PCM, which is the correct thing to archive,
  but it is not listenable flat.

## License

[Blue Oak Model License 1.0.0](LICENSE.md).
