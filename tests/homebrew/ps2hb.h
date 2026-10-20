// Minimal freestanding helpers for homebrew test programs (no SDK).
#pragma once
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef int s32;

#define REG32(a) (*(volatile u32*)(a))
#define REG64(a) (*(volatile u64*)(a))

// SIO TX for text output
static inline void putch(char c) { REG32(0x1000F180) = (u8)c; }
static inline void puts_(const char* s) { while (*s) putch(*s++); }
static inline void puthex(u32 v) {
    for (int i = 7; i >= 0; --i) putch("0123456789abcdef"[(v >> (i * 4)) & 15]);
}

// GS privileged registers
#define GS_PMODE    0x12000000
#define GS_SMODE2   0x12000020
#define GS_DISPFB1  0x12000070
#define GS_DISPLAY1 0x12000080
#define GS_BGCOLOR  0x120000E0
#define GS_CSR      0x12001000

// GS general register addresses (A+D)
enum {
    GS_PRIM = 0x00, GS_RGBAQ = 0x01, GS_ST = 0x02, GS_UV = 0x03, GS_XYZ2 = 0x05, GS_TEX0_1 = 0x06,
    GS_CLAMP_1 = 0x08, GS_TEX1_1 = 0x14, GS_XYOFFSET_1 = 0x18, GS_PRMODECONT = 0x1A, GS_TEXA = 0x3B,
    GS_SCISSOR_1 = 0x40, GS_ALPHA_1 = 0x42, GS_TEST_1 = 0x47, GS_FRAME_1 = 0x4C, GS_ZBUF_1 = 0x4E,
    GS_BITBLTBUF = 0x50, GS_TRXPOS = 0x51, GS_TRXREG = 0x52, GS_TRXDIR = 0x53, GS_FINISH = 0x61,
};

// A packet buffer of 128-bit words, built in RAM and sent by DMA.
typedef struct {
    u64* p;
    u64* base;
} Packet;

static inline void giftag(Packet* k, u32 nloop, u32 eop, u32 pre, u32 prim, u32 flg, u32 nreg, u64 regs) {
    k->p[0] = (u64)nloop | ((u64)eop << 15) | ((u64)pre << 46) | ((u64)prim << 47) | ((u64)flg << 58) | ((u64)nreg << 60);
    k->p[1] = regs;
    k->p += 2;
}
static inline void ad(Packet* k, u32 reg, u64 v) {
    k->p[0] = v;
    k->p[1] = reg;
    k->p += 2;
}

// DMA channel 2 (GIF), normal mode
static inline void dma_gif(const void* data, u32 qwc) {
    REG32(0x1000E000) = 1;               // D_CTRL: DMAE
    REG32(0x1000A010) = (u32)data;       // MADR
    REG32(0x1000A020) = qwc;             // QWC
    REG32(0x1000A000) = 0x100;           // CHCR: STR, normal, from memory
    while (REG32(0x1000A000) & 0x100) {}
}
// DMA channel 1 (VIF1), normal mode
static inline void dma_vif1(const void* data, u32 qwc) {
    REG32(0x1000E000) = 1;
    REG32(0x10009010) = (u32)data;
    REG32(0x10009020) = qwc;
    REG32(0x10009000) = 0x101;           // STR, DIR=from memory
    while (REG32(0x10009000) & 0x100) {}
}

static inline void wait_vsync(void) {
    REG64(GS_CSR) = 8;                   // clear VSINT
    while (!(REG64(GS_CSR) & 8)) {}
}

static inline void setup_display_640x448(void) {
    REG64(GS_PMODE) = 0x0000000000000001ull | (0xFFull << 8); // EN1, ALP=0xFF
    REG64(GS_SMODE2) = 0;                                      // progressive frame
    REG64(GS_DISPFB1) = (u64)0 | ((u64)10 << 9) | ((u64)0 << 15); // FBP 0, FBW 10, CT32
    REG64(GS_DISPLAY1) = (u64)0 | ((u64)3 << 23) | ((u64)2559 << 32) | ((u64)447 << 44);
}
