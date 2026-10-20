// GS rendering self-test: clear, gouraud triangle, textured sprite (CT32 and
// PSMT8+CLUT), alpha blending, depth test, VU1 path (MPG + UNPACK + XGKICK).
#include "ps2hb.h"

static u64 buf[4096] __attribute__((aligned(16)));
static u32 vifbuf[1024] __attribute__((aligned(16)));

static void draw_scene(void) {
    Packet k = {buf, buf};
    giftag(&k, 9, 1, 0, 0, 0, 1, 0xE);
    ad(&k, GS_FRAME_1, (u64)0 | ((u64)10 << 16) | ((u64)0 << 24));
    ad(&k, GS_ZBUF_1, (u64)140 | ((u64)0 << 24));                 // Z32 after the frame buffer
    ad(&k, GS_XYOFFSET_1, 0);
    ad(&k, GS_SCISSOR_1, (u64)0 | ((u64)639 << 16) | ((u64)0 << 32) | ((u64)447 << 48));
    ad(&k, GS_TEST_1, (u64)1 << 16 | (u64)1 << 17);                // ZTE, ZTST ALWAYS
    ad(&k, GS_PRMODECONT, 1);
    ad(&k, GS_PRIM, 6);                                            // sprite: clear
    ad(&k, GS_RGBAQ, 0x80402020ull);
    ad(&k, GS_XYZ2, 0);
    giftag(&k, 1, 1, 0, 0, 0, 1, 0xE);
    ad(&k, GS_XYZ2, (u64)(640 << 4) | ((u64)(448 << 4) << 16));
    // Gouraud triangle: red, green, blue
    giftag(&k, 7, 1, 0, 0, 0, 1, 0xE);
    ad(&k, GS_PRIM, 3 | (1 << 3));
    ad(&k, GS_RGBAQ, 0x800000FFull);
    ad(&k, GS_XYZ2, (u64)(100 << 4) | ((u64)(100 << 4) << 16) | ((u64)1 << 32));
    ad(&k, GS_RGBAQ, 0x8000FF00ull);
    ad(&k, GS_XYZ2, (u64)(300 << 4) | ((u64)(100 << 4) << 16) | ((u64)1 << 32));
    ad(&k, GS_RGBAQ, 0x80FF0000ull);
    ad(&k, GS_XYZ2, (u64)(200 << 4) | ((u64)(300 << 4) << 16) | ((u64)1 << 32));
    dma_gif(buf, (u32)(k.p - k.base) / 2);
}

static void upload_textures(void) {
    // 16x16 CT32 checkerboard at block 0x3000 (8 MB / 256 ... any free area)
    Packet k = {buf, buf};
    giftag(&k, 4, 0, 0, 0, 0, 1, 0xE);
    ad(&k, GS_BITBLTBUF, ((u64)0x3000 << 32) | ((u64)1 << 48) | ((u64)0 << 56));
    ad(&k, GS_TRXPOS, 0);
    ad(&k, GS_TRXREG, 16 | ((u64)16 << 32));
    ad(&k, GS_TRXDIR, 0);
    giftag(&k, 16 * 16 * 4 / 16, 1, 0, 0, 2, 0, 0);
    u32* px = (u32*)k.p;
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) px[y * 16 + x] = (((x >> 2) ^ (y >> 2)) & 1) ? 0x80FFFFFF : 0x80000000;
    k.p += 16 * 16 * 4 / 8;
    // 16x16 PSMT8 indices (diagonal stripes) at block 0x3100, CLUT CT32 at 0x3200
    giftag(&k, 4, 0, 0, 0, 0, 1, 0xE);
    ad(&k, GS_BITBLTBUF, ((u64)0x3100 << 32) | ((u64)2 << 48) | ((u64)0x13 << 56));
    ad(&k, GS_TRXPOS, 0);
    ad(&k, GS_TRXREG, 16 | ((u64)16 << 32));
    ad(&k, GS_TRXDIR, 0);
    giftag(&k, 16 * 16 / 16, 0, 0, 0, 2, 0, 0);
    u8* ix = (u8*)k.p;
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) ix[y * 16 + x] = (u8)((x + y) & 3);
    k.p += 16 * 16 / 8;
    // CLUT: 256 entries CSM1 layout (16x16 CT32); only indices 0..3 matter.
    giftag(&k, 4, 0, 0, 0, 0, 1, 0xE);
    ad(&k, GS_BITBLTBUF, ((u64)0x3200 << 32) | ((u64)1 << 48) | ((u64)0 << 56));
    ad(&k, GS_TRXPOS, 0);
    ad(&k, GS_TRXREG, 16 | ((u64)16 << 32));
    ad(&k, GS_TRXDIR, 0);
    giftag(&k, 16 * 16 * 4 / 16, 1, 0, 0, 2, 0, 0);
    u32* cl = (u32*)k.p;
    for (int i = 0; i < 256; ++i) cl[i] = 0x80000000;
    cl[0] = 0x800000FF; // red
    cl[1] = 0x8000FF00; // green
    cl[2] = 0x80FF0000; // blue
    cl[3] = 0x8000FFFF; // yellow
    k.p += 16 * 16 * 4 / 8;
    dma_gif(buf, (u32)(k.p - k.base) / 2);
}

static void draw_textured(void) {
    Packet k = {buf, buf};
    // CT32 checkerboard scaled 8x at (400,100)
    giftag(&k, 10, 1, 0, 0, 0, 1, 0xE);
    ad(&k, GS_TEX0_1, (u64)0x3000 | ((u64)1 << 14) | ((u64)0 << 20) | ((u64)4 << 26) | ((u64)4 << 30) |
                          ((u64)1 << 34) | ((u64)1 << 35)); // TBW 1, CT32, 16x16, TCC, DECAL
    ad(&k, GS_TEX1_1, 0);
    ad(&k, GS_CLAMP_1, 1 | (1 << 2));
    ad(&k, GS_PRIM, 6 | (1 << 4) | (1 << 8));                       // sprite, TME, FST
    ad(&k, GS_RGBAQ, 0x80808080ull);
    ad(&k, GS_UV, 0);
    ad(&k, GS_XYZ2, (u64)(400 << 4) | ((u64)(100 << 4) << 16) | ((u64)2 << 32));
    ad(&k, GS_UV, (16 << 4) | ((u64)(16 << 4) << 16));
    ad(&k, GS_XYZ2, (u64)(528 << 4) | ((u64)(228 << 4) << 16) | ((u64)2 << 32));
    ad(&k, GS_FINISH, 0);
    // PSMT8 + CLUT (CSM1, CT32, CLD=1) scaled 8x at (400,250)
    giftag(&k, 6, 1, 0, 0, 0, 1, 0xE);
    ad(&k, GS_TEX0_1, (u64)0x3100 | ((u64)2 << 14) | ((u64)0x13 << 20) | ((u64)4 << 26) | ((u64)4 << 30) |
                          ((u64)1 << 34) | ((u64)1 << 35) | ((u64)0x3200 << 37) | ((u64)0 << 51) | ((u64)1 << 61));
    ad(&k, GS_UV, 0);
    ad(&k, GS_XYZ2, (u64)(400 << 4) | ((u64)(250 << 4) << 16) | ((u64)2 << 32));
    ad(&k, GS_UV, (16 << 4) | ((u64)(16 << 4) << 16));
    ad(&k, GS_XYZ2, (u64)(528 << 4) | ((u64)(378 << 4) << 16) | ((u64)2 << 32));
    ad(&k, GS_FINISH, 0);
    // Alpha blend: white with A=0x40, (Cs-Cd)*As/128 + Cd over the background at (20..80, 380..440)
    giftag(&k, 5, 1, 0, 0, 0, 1, 0xE);
    ad(&k, GS_ALPHA_1, (u64)0 | (1 << 2) | (0 << 4) | (1 << 6));
    ad(&k, GS_PRIM, 6 | (1 << 6));
    ad(&k, GS_RGBAQ, 0x40FFFFFFull);
    ad(&k, GS_XYZ2, (u64)(20 << 4) | ((u64)(380 << 4) << 16));
    ad(&k, GS_XYZ2, (u64)(80 << 4) | ((u64)(440 << 4) << 16));
    // Depth test: GEQUAL; a far (z=1) sprite must not overwrite the near (z=5) one
    giftag(&k, 7, 1, 0, 0, 0, 1, 0xE);
    ad(&k, GS_TEST_1, (u64)1 << 16 | (u64)2 << 17);
    ad(&k, GS_PRIM, 6);
    ad(&k, GS_RGBAQ, 0x80FF00FFull);                                // magenta, z=5
    ad(&k, GS_XYZ2, (u64)(100 << 4) | ((u64)(350 << 4) << 16) | ((u64)5 << 32));
    ad(&k, GS_XYZ2, (u64)(160 << 4) | ((u64)(410 << 4) << 16) | ((u64)5 << 32));
    ad(&k, GS_RGBAQ, 0x8000FFFFull);                                // yellow, z=1 (behind)
    ad(&k, GS_XYZ2, (u64)(130 << 4) | ((u64)(380 << 4) << 16) | ((u64)1 << 32));
    giftag(&k, 1, 1, 0, 0, 0, 1, 0xE);
    ad(&k, GS_XYZ2, (u64)(190 << 4) | ((u64)(440 << 4) << 16) | ((u64)1 << 32));
    dma_gif(buf, (u32)(k.p - k.base) / 2);
}

// VU1 path: micro program XGKICKs a GIF packet that was UNPACKed to VU1 memory.
static void draw_vu1(void) {
    u32* w = vifbuf;
    // GIF packet (6 qwords): tag + A+D PRIM + RGBAQ + XYZ2 + XYZ2 (sprite at 220..300, 350..430, cyan)
    u64 pkt[10];
    pkt[0] = (u64)4 | ((u64)1 << 15) | ((u64)1 << 60); pkt[1] = 0xE;
    pkt[2] = 6; pkt[3] = GS_PRIM;
    pkt[4] = 0x80FFFF00ull; pkt[5] = GS_RGBAQ;
    pkt[6] = (u64)(220 << 4) | ((u64)(350 << 4) << 16); pkt[7] = GS_XYZ2;
    pkt[8] = (u64)(300 << 4) | ((u64)(430 << 4) << 16); pkt[9] = GS_XYZ2;
    *w++ = 0x01000101;                 // STCYCL 1,1
    *w++ = 0x6C050000;                 // UNPACK V4-32, NUM=5, ADDR=0
    for (int i = 0; i < 10; ++i) {
        *w++ = (u32)pkt[i];
        *w++ = (u32)(pkt[i] >> 32);
    }
    // MPG: 4 instruction pairs at address 0
    *w++ = 0x4A040000;
    // pair 0: IADDIU vi1, vi0, 0 | NOP
    *w++ = 0x10010000; *w++ = 0x000002FF;
    // pair 1: XGKICK vi1 | NOP
    *w++ = 0x80000EFC; *w++ = 0x000002FF;
    // pair 2: NOP | NOP[E]
    *w++ = 0x8000033C; *w++ = 0x400002FF;
    // pair 3: NOP | NOP
    *w++ = 0x8000033C; *w++ = 0x000002FF;
    *w++ = 0x14000000;                 // MSCAL 0
    while (((u32)(w - vifbuf)) & 3) *w++ = 0; // pad to qword
    dma_vif1(vifbuf, (u32)(w - vifbuf) / 4);
}

int main(void) {
    puts_("gs_basic: start\n");
    setup_display_640x448();
    draw_scene();
    upload_textures();
    draw_textured();
    draw_vu1();
    wait_vsync();
    wait_vsync();
    puts_("gs_basic: done\n");
    return 0;
}
