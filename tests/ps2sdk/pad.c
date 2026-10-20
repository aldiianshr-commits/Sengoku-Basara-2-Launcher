// libpad against the HLE PADMAN service (new protocol: XPADMAN-style RPC).
// The runner injects "Cross + Up" on port 1 with --test-pad 0x4010.
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <libpad.h>

static char padBuf[256] __attribute__((aligned(64)));

static void vsync(int n)
{
    // Busy wait on the vertical blank counter via the GS CSR VSINT bit.
    volatile unsigned long *csr = (volatile unsigned long *)0x12001000;
    while (n-- > 0) {
        *csr = 8;
        while (!(*csr & 8)) ;
    }
}

int main(void)
{
    struct padButtonStatus b;
    int r, state, i;
    SifInitRpc(0);
    printf("pad: load SIO2MAN %d\n", SifLoadModule("rom0:SIO2MAN", 0, NULL) > 0);
    printf("pad: load PADMAN %d\n", SifLoadModule("rom0:PADMAN", 0, NULL) > 0);
    r = padInit(0);
    printf("pad: init %d\n", r);
    printf("pad: portmax %d slotmax %d\n", padGetPortMax(), padGetSlotMax(0));
    r = padPortOpen(0, 0, padBuf);
    printf("pad: open %d\n", r);
    for (i = 0; i < 60; i++) {
        state = padGetState(0, 0);
        if (state == PAD_STATE_STABLE) break;
        vsync(1);
    }
    printf("pad: state %d\n", state);
    printf("pad: modecurid %d modetable %d [%d %d]\n", padInfoMode(0, 0, PAD_MODECURID, 0),
           padInfoMode(0, 0, PAD_MODETABLE, -1), padInfoMode(0, 0, PAD_MODETABLE, 0), padInfoMode(0, 0, PAD_MODETABLE, 1));
    r = padSetMainMode(0, 0, PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK);
    printf("pad: setmainmode %d\n", r);
    for (i = 0; i < 60 && padGetReqState(0, 0) != PAD_RSTAT_COMPLETE; i++) vsync(1);
    printf("pad: reqstate %d\n", padGetReqState(0, 0));
    printf("pad: modecurid %d\n", padInfoMode(0, 0, PAD_MODECURID, 0));
    printf("pad: actuators %d\n", padInfoAct(0, 0, -1, 0));
    printf("pad: pressmode %d\n", padInfoPressMode(0, 0));
    vsync(2);
    r = padRead(0, 0, &b);
    printf("pad: read len %d ok %d mode %02x btns %04x\n", r, b.ok, b.mode, 0xffff ^ b.btns);
    printf("pad: sticks %02x %02x %02x %02x\n", b.ljoy_h, b.ljoy_v, b.rjoy_h, b.rjoy_v);
    printf("pad: cross pressure %d\n", b.cross_p);
    {
        char align[6] = {0, 1, 0xff, 0xff, 0xff, 0xff};
        char act[6] = {1, 200, 0, 0, 0, 0};
        printf("pad: actalign %d\n", padSetActAlign(0, 0, align));
        printf("pad: actdirect %d\n", padSetActDirect(0, 0, act));
    }
    printf("pad: port2 state %d\n", padPortOpen(1, 0, padBuf) ? 1 : 0);
    printf("pad: done\n");
    return 0;
}
