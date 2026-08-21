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
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include "sha256.h"

#define CHUNK_BYTES    65536   /* bytes per READ(10) in the fast path,
                                  reduced if the adapter allows less   */
#define SPTI_TIMEOUT   30      /* seconds per SCSI command               */
#define MAX_RETRIES    4       /* per-sector retries in the slow path    */
#define SENSE_LEN      32

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
    s.sptd.DataIn             = SCSI_IOCTL_DATA_IN;
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

static int inquiry(HANDLE h, char vendor[9], char product[17], char rev[5],
                   scsi_err *e)
{
    UCHAR cdb[6] = { 0x12, 0, 0, 0, 96, 0 };   /* INQUIRY */
    UCHAR buf[96];
    memset(buf, 0, sizeof buf);
    if (scsi_cmd(h, cdb, 6, buf, sizeof buf, e) != 0) return -1;
    memcpy(vendor,  buf + 8, 8);  vendor[8]   = 0;
    memcpy(product, buf + 16, 16); product[16] = 0;
    memcpy(rev,     buf + 32, 4);  rev[4]      = 0;
    return 0;
}

static int read_capacity(HANDLE h, uint32_t *last_lba, uint32_t *block_size,
                         scsi_err *e)
{
    UCHAR cdb[10] = { 0x25, 0,0,0,0,0,0,0,0,0 };   /* READ CAPACITY(10) */
    UCHAR buf[8];
    if (scsi_cmd(h, cdb, 10, buf, sizeof buf, e) != 0) return -1;
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
    char path[512], img_path[512];
    char vendor[9], product[17], rev[5];
    uint32_t last_lba, block_size, nblocks, lba, chunk, chunk_bytes;
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

    {
        scsi_err e;
        char es[96];
        if (inquiry(h, vendor, product, rev, &e) == 0) {
            logf_("device: '%s' '%s' rev '%s'\n", vendor, product, rev);
        } else {
            err_str(&e, es);
            logf_("warning: INQUIRY failed (%s)\n", es);
        }

        if (read_capacity(h, &last_lba, &block_size, &e) != 0) {
            err_str(&e, es);
            logf_("READ CAPACITY failed (%s) — no medium, or unsupported\n", es);
            return 1;
        }
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

    for (lba = 0; lba < nblocks; lba += chunk) {
        uint16_t n = (uint16_t)((nblocks - lba < chunk) ? nblocks - lba : chunk);
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
    logf_("done: %llu unreadable sector(s), %llu recovered on retry, "
          "%llu chunk read(s) fell back to sector-by-sector\n",
          (unsigned long long)bad, (unsigned long long)recovered,
          (unsigned long long)chunk_fail);
    logf_("sha256: ");
    for (i = 0; i < 32; i++) logf_("%02x", digest[i]);
    logf_("\n");
    logf_("REMINDER: image the disc a second time and compare hashes.\n");

    if (g_log) fclose(g_log);
    CloseHandle(h);
    VirtualFree(buf, 0, MEM_RELEASE);
    return bad ? 3 : 0;
}
