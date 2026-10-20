// libcdvd against the HLE CDVDFSV service.  DATA.BIN on the test disc holds
// bytes (i * 7 + 3) & 0xFF for i in [0, 40960).
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <sifrpc.h>
#include <libcdvd.h>

static unsigned char buf[20 * 2048] __attribute__((aligned(64)));
static unsigned char stbuf[16 * 2048] __attribute__((aligned(64)));

static int check(const unsigned char *p, int n, int start)
{
    int i;
    for (i = 0; i < n; i++)
        if (p[i] != (unsigned char)(((start + i) * 7 + 3) & 0xFF)) return 0;
    return 1;
}

int main(void)
{
    sceCdlFILE f;
    sceCdRMode mode = {0, SCECdSpinNom, SCECdSecS2048, 0};
    sceCdCLOCK clk;
    int r, err;
    SifInitRpc(0);
    printf("cdvd: init %d\n", sceCdInit(SCECdINIT));
    printf("cdvd: mmode %d\n", sceCdMmode(SCECdMmodeDvd));
    printf("cdvd: diskready %d\n", sceCdDiskReady(0));
    printf("cdvd: disktype %02x\n", sceCdGetDiskType());
    printf("cdvd: readclock %d\n", sceCdReadClock(&clk));
    r = sceCdSearchFile(&f, "\\DATA.BIN;1");
    printf("cdvd: search %d size %u name %s\n", r, f.size, f.name);
    printf("cdvd: search missing %d\n", sceCdSearchFile(&f, "\\NOPE.BIN;1") ? 1 : 0);
    sceCdSearchFile(&f, "\\DATA.BIN;1");
    r = sceCdRead(f.lsn, 20, buf, &mode);
    printf("cdvd: read issued %d\n", r);
    sceCdSync(0);
    printf("cdvd: read data ok %d error %d\n", check(buf, 20 * 2048, 0), sceCdGetError());
    // Unaligned destination
    memset(buf, 0, sizeof buf);
    sceCdRead(f.lsn + 2, 1, buf + 5, &mode);
    sceCdSync(0);
    printf("cdvd: unaligned read ok %d\n", check(buf + 5, 2048, 2 * 2048));
    // Streaming
    printf("cdvd: stinit %d\n", sceCdStInit(64, 4, stbuf));
    printf("cdvd: ststart %d\n", sceCdStStart(f.lsn + 4, &mode));
    r = sceCdStRead(8, (u32 *)stbuf, STMBLK, (u32 *)&err);
    printf("cdvd: stread %d err %d ok %d\n", r, err, check(stbuf, 8 * 2048, 4 * 2048));
    printf("cdvd: ststop %d\n", sceCdStStop());
    printf("cdvd: status %d\n", sceCdStatus());
    printf("cdvd: done\n");
    return 0;
}
