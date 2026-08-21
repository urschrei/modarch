# Vista machine runbook

Field procedure for imaging MO discs with `imager.exe` on the air-gapped
Windows Vista machine. Print this or keep it on the phone — the Vista box
has no network.

## Before you go

- [ ] Get the freshest `imager-vista-i686` artifact from GitHub Actions
      (or the verified `imager-vista-i386.exe` from `imager/`).
- [ ] Confirm it is Vista-safe:
      `i686-w64-mingw32-objdump -p imager.exe | grep 'DLL Name'`
      must list **only** `KERNEL32.dll` and `msvcrt.dll`. Anything
      `api-ms-win-crt-*` is a UCRT build and will not start on Vista.
- [ ] Copy the exe to a USB stick. Nothing else is needed — it is a
      single static binary.

## On the Vista machine

### 1. Confirm the binary loads

Open an **elevated** command prompt (right-click cmd → Run as
administrator; SPTI requires admin) and run it bare:

```
imager.exe
```

It should print usage and exit. If Windows complains about a missing
DLL instead, the wrong build travelled — stop here.

Output goes next to the basename you give, so run from a directory on
an **NTFS** volume with room for the full image. FAT32 (most USB
sticks) stops at 4 GiB and the run fails.

### 2. Identify the MO drive

```
wmic diskdrive list brief
```

(or Disk Management: diskmgmt.msc). Note the `\\.\PhysicalDriveN`
number of the MO drive. Do not guess — imaging the wrong device wastes
time, and you want the log's INQUIRY line to name the MO drive.

### 3. Sacrificial disc first

**Write-protect the cartridge** (slide the tab) before it enters the
drive. Then image it twice, to two different basenames:

```
imager.exe \\.\PhysicalDrive2 sac_a
imager.exe \\.\PhysicalDrive2 sac_b
fc /b sac_a.img sac_b.img
```

- `fc` must report no differences (equivalently, `sac_a.sha256` and
  `sac_b.sha256` must match).
- The imager waits up to 30 s for the drive to report ready after a
  cartridge goes in, and logs each not-ready response. If it gives up,
  the log says why.
- Read `sac_a.log` in full before the print master goes in:
  - The `device:` and `inquiry:` lines (vendor/product/revision, device
    type, removable bit) and the raw INQUIRY, READ CAPACITY and MODE
    SENSE dumps are format documentation in themselves — copy them into
    `docs/format-notes.md` later.
  - `mode sense(...)`: `write-protect YES` is expected. A `WARNING:
    medium is not reported write-protected` line means either the tab
    is not set or the drive does not report it; find out which before
    continuing.
  - `capacity:` — 1024 B/sector is expected for MO; anything else is
    worth noting.
  - `adapter:` and `chunk:` — the transfer limits the host reported and
    the chunk size chosen. If every chunk read "fell back to
    sector-by-sector" with a `win32=` cause, the host is rejecting the
    transfer size rather than the disc being bad.
  - `chunk ... failed`, `RECOVERED sector`, `BAD sector` lines — the
    error geography. A sacrificial disc should produce none.
  - `done:`, `elapsed:` — the summary counts and throughput, a baseline
    for judging later runs.
- Exit code 0 = clean; 1 = setup or write failure (see the log); 3 =
  completed with unreadable sectors (listed in the log, zero-filled in
  the image).

If the double-image hashes disagree, stop: that is a drive, cable, or
media problem, and no print master goes in the drive until it is
understood.

### 4. Print master

Same procedure, same discipline:

```
imager.exe \\.\PhysicalDrive2 pm_<title>_a
imager.exe \\.\PhysicalDrive2 pm_<title>_b
fc /b pm_<title>_a.img pm_<title>_b.img
```

One matched pair per disc. Copy `.img`, `.log`, and `.sha256` to the
USB stick. Re-verify the hash after the copy, on the other machine:

```
shasum -a 256 -c pm_<title>_a.sha256
```

## Back home

```
modarch map pm_<title>_a.img          # first look at the layout
modarch prospect pm_<title>_a.img -o out/   # candidate Lt/Rt WAVs
```

Record everything learned — INQUIRY string, sector size, capacity, map
output, any bad sectors — in `docs/format-notes.md`, moving items out
of *(unverified)* as they are confirmed.

## Reminders

- The disc is the artifact. The image is the work product. Nothing
  writes to the disc: the tool issues only TEST UNIT READY, INQUIRY,
  READ CAPACITY, MODE SENSE and READ(10), and the cartridge tab is
  belt-and-braces on top.
- Bring the `.log` files home even from failed runs. The log is the
  only evidence of what the drive and host did, and it is what
  shortens the next build-test cycle.
- Unreadable sectors are zero-filled, never skipped, so file offsets in
  the image stay true to the disc.
- If the drive spins down mid-read or a disc reads with errors, note
  which sectors (the log has them) — error geography is data too.
