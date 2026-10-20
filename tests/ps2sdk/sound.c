// libsdr (sceSdRemote) against the HLE SDRDRV/libsd service and the SPU2
// model: uploads an ADPCM square wave, plays it on core 0 voice 0 and checks
// the envelope / ENDX state.  The runner also dumps and checks the audio.
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <iopheap.h>
#include <libsdr.h>
#define SD_CORE_0 0
#define SD_CORE_1 1

// Work around a ps2sdk packaging bug: libsdr references this object but the
// library does not define it.  8 KB comfortably covers the real structure.
char g_libsdr_cb_data[8192] __attribute__((aligned(64)));

#define NBLOCKS 32
static unsigned char adpcm[NBLOCKS * 16] __attribute__((aligned(64)));

static void vsync(int n)
{
    volatile unsigned long *csr = (volatile unsigned long *)0x12001000;
    while (n-- > 0) {
        *csr = 8;
        while (!(*csr & 8)) ;
    }
}

int main(void)
{
    int i, j, r;
    void *iop;
    SifInitRpc(0);
    printf("sound: load LIBSD %d\n", SifLoadModule("rom0:LIBSD", 0, NULL) > 0);
    printf("sound: load SDRDRV %d\n", SifLoadModule("host0:SDRDRV.IRX", 0, NULL) != 0);
    printf("sound: remoteinit %d\n", sceSdRemoteInit());
    printf("sound: sdinit %d\n", sceSdRemote(1, rSdInit, 0));
    // ADPCM: filter 0, shift 2, square wave of 28-sample half periods.
    for (i = 0; i < NBLOCKS; i++) {
        unsigned char *b = &adpcm[i * 16];
        b[0] = 0x02;
        b[1] = (i == 0) ? 4 : ((i == NBLOCKS - 1) ? 3 : 0);
        for (j = 2; j < 16; j++) b[j] = (i & 1) ? 0x99 : 0x77;
    }
    SifInitIopHeap();
    iop = SifAllocIopHeap(sizeof(adpcm));
    printf("sound: iop alloc %d\n", iop != NULL);
    sceSdTransToIOP(adpcm, iop, sizeof(adpcm), 1);
    r = sceSdRemote(1, rSdVoiceTrans, 0, SD_TRANS_WRITE | SD_TRANS_MODE_DMA, iop, (void *)0x5010, sizeof(adpcm));
    printf("sound: voicetrans %d\n", r);
    printf("sound: transstatus %d\n", sceSdRemote(1, rSdVoiceTransStatus, 0, 1));
    sceSdRemote(1, rSdSetAddr, SD_CORE_0 | SD_VOICE(0, 0) | SD_VADDR_SSA, 0x5010);
    sceSdRemote(1, rSdSetParam, SD_CORE_0 | SD_VOICE(0, 0) | SD_VPARAM_VOLL, 0x3fff);
    sceSdRemote(1, rSdSetParam, SD_CORE_0 | SD_VOICE(0, 0) | SD_VPARAM_VOLR, 0x3fff);
    sceSdRemote(1, rSdSetParam, SD_CORE_0 | SD_VOICE(0, 0) | SD_VPARAM_PITCH, 0x0800);
    sceSdRemote(1, rSdSetParam, SD_CORE_0 | SD_VOICE(0, 0) | SD_VPARAM_ADSR1, SD_SET_ADSR1(0, 0x10, 0xf, 0xf));
    sceSdRemote(1, rSdSetParam, SD_CORE_0 | SD_VOICE(0, 0) | SD_VPARAM_ADSR2, SD_SET_ADSR2(0, 0x7f, 0, 0x10));
    for (i = 0; i < 2; i++) {
        sceSdRemote(1, rSdSetParam, i | SD_PARAM_MVOLL, 0x3fff);
        sceSdRemote(1, rSdSetParam, i | SD_PARAM_MVOLR, 0x3fff);
        sceSdRemote(1, rSdSetCoreAttr, i | SD_CORE_MUTE_ENABLE, 0);
    }
    sceSdRemote(1, rSdSetParam, SD_CORE_1 | SD_PARAM_BVOLL, 0x7fff);
    sceSdRemote(1, rSdSetParam, SD_CORE_1 | SD_PARAM_BVOLR, 0x7fff);
    printf("sound: mvol readback %04x\n", sceSdRemote(1, rSdGetParam, SD_CORE_0 | SD_PARAM_MVOLL));
    printf("sound: ssa readback %05x\n", sceSdRemote(1, rSdGetAddr, SD_CORE_0 | SD_VOICE(0, 0) | SD_VADDR_SSA));
    sceSdRemote(1, rSdSetSwitch, SD_CORE_0 | SD_SWITCH_KON, 1);
    vsync(30);
    r = sceSdRemote(1, rSdGetParam, SD_CORE_0 | SD_VOICE(0, 0) | SD_VPARAM_ENVX);
    printf("sound: envx after attack > 0x7000: %d\n", r > 0x7000);
    printf("sound: endx %d\n", sceSdRemote(1, rSdGetSwitch, SD_CORE_0 | SD_SWITCH_ENDX) & 1);
    sceSdRemote(1, rSdSetSwitch, SD_CORE_0 | SD_SWITCH_KOFF, 1);
    vsync(30);
    r = sceSdRemote(1, rSdGetParam, SD_CORE_0 | SD_VOICE(0, 0) | SD_VPARAM_ENVX);
    printf("sound: envx after release %d\n", r);
    printf("sound: note2pitch %04x\n", sceSdRemote(1, rSdNote2Pitch, 60, 0, 72, 0));
    printf("sound: done\n");
    return 0;
}
