// libmc against the HLE MCSERV service (folder-backed memory cards).
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <libmc.h>
#include <fcntl.h>
#include <unistd.h>

static sceMcTblGetDir table[16] __attribute__((aligned(64)));
static char buf[128] __attribute__((aligned(64)));

static int mcwait(void)
{
    int cmd, res;
    mcSync(0, &cmd, &res);
    return res;
}

int main(void)
{
    int type, freek, format, fd, r, n, i;
    const char *msg = "Sengoku Basara 2 Heroes save test";
    SifInitRpc(0);
    printf("mc: load MCMAN %d\n", SifLoadModule("rom0:MCMAN", 0, NULL) > 0);
    printf("mc: load MCSERV %d\n", SifLoadModule("rom0:MCSERV", 0, NULL) > 0);
    r = mcInit(MC_TYPE_XMC);
    printf("mc: init %d\n", r);
    mcGetInfo(0, 0, &type, &freek, &format);
    r = mcwait();
    printf("mc: getinfo %d type %d formatted %d free>0 %d\n", r, type, format, freek > 0);
    mcGetInfo(0, 0, &type, &freek, &format);
    printf("mc: getinfo again %d\n", mcwait());
    mcDelete(0, 0, "/SB2HTEST/DATA.BIN");
    mcwait();
    mcDelete(0, 0, "/SB2HTEST");
    mcwait();
    mcMkDir(0, 0, "/SB2HTEST");
    printf("mc: mkdir %d\n", mcwait());
    mcOpen(0, 0, "/SB2HTEST/DATA.BIN", O_WRONLY | O_CREAT);
    fd = mcwait();
    printf("mc: open for write %d\n", fd >= 0);
    strcpy(buf, msg);
    mcWrite(fd, buf + 3, 25); // unaligned source
    printf("mc: write %d\n", mcwait());
    mcClose(fd);
    printf("mc: close %d\n", mcwait());
    mcOpen(0, 0, "/SB2HTEST/DATA.BIN", O_RDONLY);
    fd = mcwait();
    memset(buf, 0, sizeof buf);
    mcRead(fd, buf, 64);
    n = mcwait();
    printf("mc: read %d '%s'\n", n, buf);
    mcSeek(fd, 6, SEEK_SET);
    printf("mc: seek %d\n", mcwait());
    memset(buf, 0, sizeof buf);
    mcRead(fd, buf + 1, 5);
    printf("mc: read2 %d '%s'\n", mcwait(), buf + 1);
    mcClose(fd);
    mcwait();
    mcGetDir(0, 0, "/SB2HTEST/*", 0, 16, table);
    n = mcwait();
    printf("mc: getdir %d\n", n);
    for (i = 0; i < n; i++)
        printf("mc:  %s size %u attr %04x\n", table[i].EntryName, table[i].FileSizeByte, table[i].AttrFile);
    mcOpen(0, 0, "/NOSUCHDIR/FILE", O_RDONLY);
    printf("mc: open missing %d\n", mcwait());
    mcDelete(0, 0, "/SB2HTEST");
    printf("mc: delete non-empty dir %d\n", mcwait());
    mcDelete(0, 0, "/SB2HTEST/DATA.BIN");
    printf("mc: delete file %d\n", mcwait());
    mcDelete(0, 0, "/SB2HTEST");
    printf("mc: delete dir %d\n", mcwait());
    printf("mc: done\n");
    return 0;
}
