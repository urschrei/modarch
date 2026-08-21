/*
 * imager — raw sector imaging of SCSI MO media via SPTI.
 *
 * Target: Windows Vista (i686, static MinGW-w64 build, msvcrt runtime).
 * Usage:  imager.exe <device> <output-basename>
 *         imager.exe \\.\PhysicalDrive2 disc01
 *         imager.exe \\.\E: disc01
 *
 * Produces <base>.img, <base>.log, <base>.sha256.
 * Run elevated. Reads only: no WRITE opcodes exist in this program.
 */

#include <windows.h>
#include <winioctl.h>
#include <ntddscsi.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include "sha256.h"

#define CHUNK_BYTES    65536   /* bytes per READ(10) in the fast path,
                                  reduced if the adapter allows less   */
#define SPTI_TIMEOUT   30      /* seconds per SCSI command               */
#define MAX_RETRIES    4       /* per-sector retries in the slow path    */
#define READY_WAIT     30      /* seconds to wait for TEST UNIT READY    */
#define CAPACITY_TRIES 3       /* READ CAPACITY attempts                 */
#define SENSE_LEN      32

#ifndef IMAGER_VERSION
#define IMAGER_VERSION "unversioned"
#endif

typedef struct {
    SCSI_PASS_THROUGH_DIRECT sptd;
    UCHAR sense[SENSE_LEN];
} SPTD_WITH_SENSE;

static FILE *g_log;
static int g_progress;   /* a "\r..." progress line is on stderr */

static void logf_(const char *fmt, ...)
{
    va_list ap;
    if (g_progress) { fputc('\n', stderr); g_progress = 0; }
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (g_log) {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fflush(g_log);
    }
}

/* Outcome of one SCSI command. Exactly one of the failure fields is
 * meaningful; err_str() picks it. */
typedef struct {
    DWORD win32;        /* GetLastError() if the kernel rejected the request */
    UCHAR status;       /* SCSI status byte                                    */
    ULONG requested;    /* bytes asked for                                     */
    ULONG transferred;  /* bytes the adapter reports it moved                  */
    UCHAR sense[SENSE_LEN];
} scsi_err;

/* Issue one SCSI command with data-in transfer. Returns 0 on success.
 * Returns -1 if the kernel rejected the request, the device returned a
 * non-GOOD status, or fewer bytes than requested were transferred. On
 * any outcome *e (if given) describes what happened. */
static int scsi_cmd(HANDLE h, const UCHAR *cdb, UCHAR cdblen,
                    void *buf, ULONG buflen, scsi_err *e)
{
    SPTD_WITH_SENSE s;
    scsi_err local;
    DWORD ret;

    if (!e) e = &local;
    memset(e, 0, sizeof *e);
    e->requested = buflen;

    memset(&s, 0, sizeof s);
    s.sptd.Length             = sizeof(SCSI_PASS_THROUGH_DIRECT);
    s.sptd.CdbLength          = cdblen;
    s.sptd.SenseInfoLength    = SENSE_LEN;
    s.sptd.DataIn             = buflen ? SCSI_IOCTL_DATA_IN
                                       : SCSI_IOCTL_DATA_UNSPECIFIED;
    s.sptd.DataTransferLength = buflen;
    s.sptd.TimeOutValue       = SPTI_TIMEOUT;
    s.sptd.DataBuffer         = buf;
    s.sptd.SenseInfoOffset    = offsetof(SPTD_WITH_SENSE, sense);
    memcpy(s.sptd.Cdb, cdb, cdblen);

    if (!DeviceIoControl(h, IOCTL_SCSI_PASS_THROUGH_DIRECT,
                         &s, sizeof s, &s, sizeof s, &ret, NULL)) {
        e->win32 = GetLastError();
        return -1;
    }
    e->status      = s.sptd.ScsiStatus;
    e->transferred = s.sptd.DataTransferLength;
    memcpy(e->sense, s.sense, SENSE_LEN);
    if (e->status != 0) return -1;
    /* A short transfer with GOOD status would leave stale bytes in the
     * buffer. Treat it as a failure so the caller retries. */
    if (e->transferred != buflen) return -1;
    return 0;
}

/* Render a scsi_err for the log. Sense is decoded for fixed format
 * (response code 0x70/0x71) and descriptor format (0x72/0x73). */
static void err_str(const scsi_err *e, char out[96])
{
    const UCHAR *s = e->sense;
    UCHAR rc = s[0] & 0x7f;
    if (e->win32) {
        _snprintf(out, 95, "win32=%lu (request rejected before reaching "
                  "the device)", (unsigned long)e->win32);
    } else if (e->status != 0) {
        if (rc == 0x70 || rc == 0x71)
            _snprintf(out, 95, "status=%02x key=%02x asc=%02x ascq=%02x%s",
                      e->status, s[2] & 0x0f, s[12], s[13],
                      rc == 0x71 ? " (deferred)" : "");
        else if (rc == 0x72 || rc == 0x73)
            _snprintf(out, 95, "status=%02x key=%02x asc=%02x ascq=%02x%s",
                      e->status, s[1] & 0x0f, s[2], s[3],
                      rc == 0x73 ? " (deferred)" : "");
        else if (rc == 0)
            _snprintf(out, 95, "status=%02x (no sense data)", e->status);
        else
            _snprintf(out, 95, "status=%02x sense[0]=%02x (unrecognised "
                      "format)", e->status, s[0]);
    } else if (e->transferred != e->requested) {
        _snprintf(out, 95, "short transfer %lu/%lu bytes",
                  (unsigned long)e->transferred, (unsigned long)e->requested);
    } else {
        _snprintf(out, 95, "ok");
    }
    out[95] = 0;
}

/* Write a hex dump of n bytes to the log, 16 per line. */
static void log_hex(const char *label, const UCHAR *p, size_t n)
{
    size_t i, j;
    logf_("%s (%lu bytes):\n", label, (unsigned long)n);
    for (i = 0; i < n; i += 16) {
        logf_("  %04lx:", (unsigned long)i);
        for (j = i; j < i + 16 && j < n; j++) logf_(" %02x", p[j]);
        logf_("\n");
    }
}

static const char *dev_type_str(UCHAR t)
{
    switch (t) {
    case 0x00: return "direct-access";
    case 0x01: return "sequential-access";
    case 0x04: return "write-once";
    case 0x05: return "CD/DVD";
    case 0x07: return "optical memory";
    case 0x0e: return "simplified direct-access";
    case 0x1f: return "no device";
    default:   return "other";
    }
}

#define INQ_LEN 96

/* Standard INQUIRY. Returns the number of valid bytes in buf (at least
 * 36, at most INQ_LEN), or -1. */
static int inquiry(HANDLE h, UCHAR buf[INQ_LEN], scsi_err *e)
{
    UCHAR cdb[6] = { 0x12, 0, 0, 0, INQ_LEN, 0 };   /* INQUIRY */
    int len;
    memset(buf, 0, INQ_LEN);
    if (scsi_cmd(h, cdb, 6, buf, INQ_LEN, e) != 0) return -1;
    len = 5 + buf[4];                /* byte 4: additional length */
    if (len < 36) len = 36;
    if (len > INQ_LEN) len = INQ_LEN;
    return len;
}

#define MS_LEN 512

/* MODE SENSE for all pages (0x3F). Tries the 6-byte form first, then
 * the 10-byte form. Returns the number of valid bytes in buf, or -1.
 * *ten tells the caller which header layout buf has. */
static int mode_sense(HANDLE h, UCHAR buf[MS_LEN], int *ten, scsi_err *e)
{
    UCHAR cdb6[6]   = { 0x1a, 0, 0x3f, 0, 255, 0 };
    UCHAR cdb10[10] = { 0x5a, 0, 0x3f, 0, 0, 0, 0,
                        (UCHAR)(MS_LEN >> 8), (UCHAR)MS_LEN, 0 };
    int len;
    memset(buf, 0, MS_LEN);
    if (scsi_cmd(h, cdb6, 6, buf, 255, e) == 0) {
        *ten = 0;
        len  = buf[0] + 1;
        return len > 255 ? 255 : len;
    }
    memset(buf, 0, MS_LEN);
    if (scsi_cmd(h, cdb10, 10, buf, MS_LEN, e) == 0) {
        *ten = 1;
        len  = (buf[0] << 8 | buf[1]) + 2;
        return len > MS_LEN ? MS_LEN : len;
    }
    return -1;
}

static int test_unit_ready(HANDLE h, scsi_err *e)
{
    UCHAR cdb[6] = { 0x00, 0, 0, 0, 0, 0 };    /* TEST UNIT READY */
    return scsi_cmd(h, cdb, 6, NULL, 0, e);
}

/* Wait for the drive to report ready. A cartridge takes several seconds
 * to spin up after insertion, and the first command after a medium
 * change may return UNIT ATTENTION, which this poll absorbs. */
static int wait_ready(HANDLE h)
{
    scsi_err e;
    char es[96];
    int t;
    for (t = 0; t <= READY_WAIT; t++) {
        if (test_unit_ready(h, &e) == 0) {
            logf_("ready after %d s\n", t);
            return 0;
        }
        err_str(&e, es);
        logf_("not ready (%s), waiting (%d/%d s)\n", es, t, READY_WAIT);
        Sleep(1000);
    }
    return -1;
}

static int read_capacity(HANDLE h, uint32_t *last_lba, uint32_t *block_size,
                         UCHAR buf[8], scsi_err *e)
{
    UCHAR cdb[10] = { 0x25, 0,0,0,0,0,0,0,0,0 };   /* READ CAPACITY(10) */
    if (scsi_cmd(h, cdb, 10, buf, 8, e) != 0) return -1;
    *last_lba   = (uint32_t)buf[0]<<24 | (uint32_t)buf[1]<<16 | (uint32_t)buf[2]<<8 | buf[3];
    *block_size = (uint32_t)buf[4]<<24 | (uint32_t)buf[5]<<16 | (uint32_t)buf[6]<<8 | buf[7];
    return 0;
}

static const char *bus_type_str(UCHAR t)
{
    static const char *names[] = {
        "unknown", "SCSI", "ATAPI", "ATA", "IEEE1394", "SSA", "Fibre",
        "USB", "RAID", "iSCSI", "SAS", "SATA", "SD", "MMC"
    };
    return t < sizeof names / sizeof names[0] ? names[t] : "other";
}

/* Ask the port driver for the adapter's transfer limits. Returns 0 and
 * fills *d on success; the caller then keeps each READ within them. */
static int query_adapter(HANDLE h, STORAGE_ADAPTER_DESCRIPTOR *d)
{
    STORAGE_PROPERTY_QUERY q;
    DWORD ret;
    memset(&q, 0, sizeof q);
    q.PropertyId = StorageAdapterProperty;
    q.QueryType  = PropertyStandardQuery;
    memset(d, 0, sizeof *d);
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q,
                         d, sizeof *d, &ret, NULL))
        return -1;
    return 0;
}

static int read10(HANDLE h, uint32_t lba, uint16_t nblocks,
                  void *buf, uint32_t block_size, scsi_err *e)
{
    UCHAR cdb[10];
    memset(cdb, 0, sizeof cdb);
    cdb[0] = 0x28;                       /* READ(10) */
    cdb[2] = (UCHAR)(lba >> 24);
    cdb[3] = (UCHAR)(lba >> 16);
    cdb[4] = (UCHAR)(lba >> 8);
    cdb[5] = (UCHAR)(lba);
    cdb[7] = (UCHAR)(nblocks >> 8);
    cdb[8] = (UCHAR)(nblocks);
    return scsi_cmd(h, cdb, 10, buf, nblocks * block_size, e);
}

int main(int argc, char **argv)
{
    HANDLE h;
    FILE *img;
    char path[512] = {0}, img_path[512] = {0};
    char vendor[9], product[17], rev[5];
    UCHAR inq[INQ_LEN], cap[8], ms[MS_LEN];
    int inq_len, ms_len, ms_ten = 0;
    DWORD t0, t1;
    SYSTEMTIME st;
    OSVERSIONINFOA osv;
    uint32_t last_lba, block_size, nblocks, lba, chunk, chunk_bytes;
    uint16_t n;
    uint64_t bad = 0, written = 0, chunk_fail = 0, recovered = 0;
    UCHAR *buf;
    sha256_ctx sha;
    uint8_t digest[32];
    int i;

    if (argc != 3) {
        fprintf(stderr,
            "usage: imager.exe <device> <output-basename>\n"
            "  e.g. imager.exe \\\\.\\PhysicalDrive2 disc01\n");
        return 2;
    }
    /* _snprintf does not terminate on truncation; keep room for the
     * suffix and the NUL. */
    if (strlen(argv[2]) > sizeof path - 16) {
        fprintf(stderr, "output basename too long\n");
        return 2;
    }

    /* IOCTL_SCSI_PASS_THROUGH_DIRECT requires a handle with both read
     * and write access. No command issued here writes to the medium. */
    h = CreateFileA(argv[1], GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "cannot open %s (err=%lu) — run elevated?\n",
                argv[1], GetLastError());
        return 1;
    }

    _snprintf(path, sizeof path - 1, "%s.log", argv[2]);
    g_log = fopen(path, "w");
    if (!g_log)
        fprintf(stderr, "warning: cannot create %s (%s), logging to "
                "stderr only\n", path, strerror(errno));

    logf_("imager %s (built %s %s)\n", IMAGER_VERSION, __DATE__, __TIME__);
    logf_("command: %s %s\n", argv[1], argv[2]);
    GetLocalTime(&st);
    logf_("started: %04u-%02u-%02u %02u:%02u:%02u (machine clock)\n",
          st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    memset(&osv, 0, sizeof osv);
    osv.dwOSVersionInfoSize = sizeof osv;
    if (GetVersionExA(&osv))
        logf_("windows: %lu.%lu build %lu %s\n",
              (unsigned long)osv.dwMajorVersion,
              (unsigned long)osv.dwMinorVersion,
              (unsigned long)osv.dwBuildNumber, osv.szCSDVersion);
    t0 = GetTickCount();

    {
        scsi_err e;
        char es[96];
        inq_len = inquiry(h, inq, &e);
        if (inq_len > 0) {
            memcpy(vendor,  inq + 8,  8);  vendor[8]   = 0;
            memcpy(product, inq + 16, 16); product[16] = 0;
            memcpy(rev,     inq + 32, 4);  rev[4]      = 0;
            logf_("device: '%s' '%s' rev '%s'\n", vendor, product, rev);
            logf_("inquiry: type 0x%02x (%s), qualifier %u, %s, "
                  "version 0x%02x, response format %u\n",
                  inq[0] & 0x1f, dev_type_str(inq[0] & 0x1f), inq[0] >> 5,
                  (inq[1] & 0x80) ? "removable" : "non-removable",
                  inq[2], inq[3] & 0x0f);
            log_hex("inquiry data", inq, (size_t)inq_len);
        } else {
            err_str(&e, es);
            logf_("warning: INQUIRY failed (%s)\n", es);
        }

        if (wait_ready(h) != 0)
            logf_("warning: drive not ready after %d s, trying anyway\n",
                  READY_WAIT);

        for (i = 1; ; i++) {
            if (read_capacity(h, &last_lba, &block_size, cap, &e) == 0) break;
            err_str(&e, es);
            logf_("READ CAPACITY failed (%s), attempt %d/%d\n",
                  es, i, CAPACITY_TRIES);
            if (i == CAPACITY_TRIES) {
                logf_("giving up — no medium, or unsupported\n");
                return 1;
            }
            Sleep(1000);
        }
        log_hex("read capacity data", cap, 8);

        /* The mode parameter header carries the write-protect bit: a
         * cheap check that the cartridge tab is set. */
        ms_len = mode_sense(h, ms, &ms_ten, &e);
        if (ms_len > 0) {
            UCHAR medium = ms_ten ? ms[2] : ms[1];
            UCHAR dsp    = ms_ten ? ms[3] : ms[2];
            unsigned bdl = ms_ten ? (unsigned)(ms[6] << 8 | ms[7]) : ms[3];
            const UCHAR *bd = ms + (ms_ten ? 8 : 4);
            logf_("mode sense(%d): medium type 0x%02x, device-specific "
                  "0x%02x, write-protect %s\n", ms_ten ? 10 : 6,
                  medium, dsp, (dsp & 0x80) ? "YES" : "NO");
            if (bdl >= 8)
                logf_("  block descriptor: density 0x%02x, blocks %lu, "
                      "block length %lu\n", bd[0],
                      (unsigned long)(bd[1] << 16 | bd[2] << 8 | bd[3]),
                      (unsigned long)(bd[5] << 16 | bd[6] << 8 | bd[7]));
            if (!(dsp & 0x80))
                logf_("WARNING: medium is not reported write-protected — "
                      "check the cartridge tab\n");
            log_hex("mode sense data", ms, (size_t)ms_len);
        } else {
            err_str(&e, es);
            logf_("mode sense: unavailable (%s)\n", es);
        }
    }
    if (last_lba == 0xffffffffUL) {
        logf_("capacity exceeds READ CAPACITY(10) range — not supported\n");
        return 1;
    }
    if (block_size == 0 || block_size > CHUNK_BYTES) {
        logf_("implausible block size %lu — refusing\n",
              (unsigned long)block_size);
        return 1;
    }
    nblocks = last_lba + 1;
    logf_("capacity: %lu blocks x %lu bytes = %llu bytes\n",
          (unsigned long)nblocks, (unsigned long)block_size,
          (unsigned long long)nblocks * block_size);

    chunk_bytes = CHUNK_BYTES;
    {
        STORAGE_ADAPTER_DESCRIPTOR ad;
        if (query_adapter(h, &ad) == 0) {
            logf_("adapter: bus %s, max transfer %lu bytes, max pages %lu, "
                  "alignment mask 0x%lx, %s\n",
                  bus_type_str(ad.BusType),
                  (unsigned long)ad.MaximumTransferLength,
                  (unsigned long)ad.MaximumPhysicalPages,
                  (unsigned long)ad.AlignmentMask,
                  ad.AdapterUsesPio ? "PIO" : "DMA");
            if (ad.MaximumTransferLength && ad.MaximumTransferLength < chunk_bytes)
                chunk_bytes = ad.MaximumTransferLength;
            /* The buffer is page-aligned, so it spans exactly
             * chunk_bytes / 4096 pages. */
            if (ad.MaximumPhysicalPages &&
                ad.MaximumPhysicalPages < chunk_bytes / 4096)
                chunk_bytes = ad.MaximumPhysicalPages * 4096;
        } else {
            logf_("adapter: IOCTL_STORAGE_QUERY_PROPERTY failed (win32=%lu), "
                  "assuming %lu-byte transfers\n",
                  (unsigned long)GetLastError(), (unsigned long)chunk_bytes);
        }
    }
    chunk = chunk_bytes / block_size;
    if (chunk < 1) chunk = 1;
    if (chunk > 0xffff) chunk = 0xffff;
    logf_("chunk: %lu blocks (%lu bytes) per READ(10)\n",
          (unsigned long)chunk, (unsigned long)(chunk * block_size));

    /* VirtualAlloc gives a page-aligned buffer, which satisfies any
     * adapter alignment mask. */
    buf = (UCHAR *)VirtualAlloc(NULL, (size_t)chunk * block_size,
                                MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) { logf_("out of memory\n"); return 1; }

    _snprintf(img_path, sizeof img_path - 1, "%s.img", argv[2]);
    img = fopen(img_path, "wb");
    if (!img) {
        logf_("cannot create %s (%s)\n", img_path, strerror(errno));
        return 1;
    }

    sha256_init(&sha);

    for (lba = 0; lba < nblocks; lba += n) {
        n = (uint16_t)((nblocks - lba < chunk) ? nblocks - lba : chunk);
        scsi_err e;
        char es[96];

        if (read10(h, lba, n, buf, block_size, &e) != 0) {
            /* slow path: one sector at a time with retries */
            uint32_t s;
            err_str(&e, es);
            logf_("chunk lba=%lu n=%u failed (%s) — reading sector by sector\n",
                  (unsigned long)lba, (unsigned)n, es);
            chunk_fail++;
            for (s = 0; s < n; s++) {
                int ok = 0, r;
                scsi_err last;
                memset(&last, 0, sizeof last);
                for (r = 0; r < MAX_RETRIES && !ok; r++) {
                    ok = (read10(h, lba + s, 1, buf + (size_t)s * block_size,
                                 block_size, &e) == 0);
                    if (!ok) last = e;
                }
                if (!ok) {
                    err_str(&last, es);
                    logf_("BAD sector lba=%lu after %d tries (%s) — zero-filled\n",
                          (unsigned long)(lba + s), MAX_RETRIES, es);
                    memset(buf + (size_t)s * block_size, 0, block_size);
                    bad++;
                } else if (r > 1) {
                    err_str(&last, es);
                    logf_("RECOVERED sector lba=%lu on try %d (earlier: %s)\n",
                          (unsigned long)(lba + s), r, es);
                    recovered++;
                }
            }
        }
        /* The hash covers the bytes read from the disc. A write error
         * therefore cannot be detected from the hash: check it here. */
        if (fwrite(buf, block_size, n, img) != n) {
            logf_("\nWRITE ERROR on %s after %llu bytes (%s) — "
                  "image is incomplete, no hash written\n",
                  img_path, (unsigned long long)written, strerror(errno));
            fclose(img);
            return 1;
        }
        written += (uint64_t)n * block_size;
        sha256_update(&sha, buf, (size_t)n * block_size);
        if ((lba / chunk) % 64 == 0) {
            fprintf(stderr, "\r%lu / %lu blocks",
                    (unsigned long)lba, (unsigned long)nblocks);
            g_progress = 1;
        }
    }
    fprintf(stderr, "\r%lu / %lu blocks\n",
            (unsigned long)nblocks, (unsigned long)nblocks);
    g_progress = 0;
    if (fclose(img) != 0) {
        logf_("WRITE ERROR closing %s (%s) — image may be incomplete, "
              "no hash written\n", img_path, strerror(errno));
        return 1;
    }
    logf_("image: %s, %llu bytes written\n",
          img_path, (unsigned long long)written);

    sha256_final(&sha, digest);
    _snprintf(path, sizeof path - 1, "%s.sha256", argv[2]);
    {
        FILE *f = fopen(path, "w");
        if (!f) {
            logf_("warning: cannot create %s (%s)\n", path, strerror(errno));
        } else {
            for (i = 0; i < 32; i++) fprintf(f, "%02x", digest[i]);
            fprintf(f, "  %s.img\n", argv[2]);
            if (fclose(f) != 0)
                logf_("warning: error writing %s (%s)\n", path, strerror(errno));
        }
    }
    t1 = GetTickCount();
    logf_("done: %llu unreadable sector(s), %llu recovered on retry, "
          "%llu chunk read(s) fell back to sector-by-sector\n",
          (unsigned long long)bad, (unsigned long long)recovered,
          (unsigned long long)chunk_fail);
    logf_("elapsed: %lu s, %.2f MB/s\n", (unsigned long)((t1 - t0) / 1000),
          (t1 > t0) ? (double)written / 1e6 / ((double)(t1 - t0) / 1000.0)
                    : 0.0);
    logf_("sha256: ");
    for (i = 0; i < 32; i++) logf_("%02x", digest[i]);
    logf_("\n");
    logf_("REMINDER: image the disc a second time and compare hashes.\n");

    if (g_log) fclose(g_log);
    CloseHandle(h);
    VirtualFree(buf, 0, MEM_RELEASE);
    return bad ? 3 : 0;
}
