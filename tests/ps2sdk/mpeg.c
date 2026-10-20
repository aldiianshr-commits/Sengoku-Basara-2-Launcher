// MPEG-1 / MPEG-2 video decoding through ps2sdk's libmpeg, which drives the
// IPU (BDEC/VDEC/FDEC/SETIQ/CSC commands, IPU_BP/IPU_TOP, DMA channels 3/4)
// and the scratchpad DMA channels for motion compensation.
//
// The test disc holds the elementary streams (CLIP2.M2V: MPEG-2 with
// intra_vlc_format, alternate scan, non-linear quantiser and B-frames;
// CLIP1.M1V: MPEG-1 with B-frames) and reference RGB24 frames decoded by
// ffmpeg (CLIPn.RGB).  Every decoded frame is compared with its reference
// and the minimum PSNR must be at least 30 dB.
//
// Notes on libmpeg itself (same on hardware): it stops at the sequence end
// code without returning the last reference picture (9 of 10 frames), it
// needs a sequence_end_code to terminate (appended to the ffmpeg output),
// and its EE-side motion compensation derives chroma half-pel positions
// with floor(mv/2) instead of the standard's truncation, so predicted
// frames score ~35 dB against ffmpeg while intra frames reach ~45 dB.
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <malloc.h>
#include <kernel.h>
#include <sifrpc.h>
#include <libcdvd.h>
#include <dma.h>
#include <libmpeg.h>

static unsigned char *s_Data, *s_Ptr, *s_End;
static unsigned char *s_Pict;
static MPEGSequenceInfo *s_Info;

static unsigned char *loadFile(const char *name, unsigned int *size)
{
    sceCdlFILE f;
    sceCdRMode mode = {0, SCECdSpinNom, SCECdSecS2048, 0};
    unsigned char *buf;
    unsigned int sectors;
    if (!sceCdSearchFile(&f, name)) return NULL;
    sectors = (f.size + 2047) / 2048;
    buf = memalign(64, sectors * 2048 + 4096);
    memset(buf, 0, sectors * 2048 + 4096);
    sceCdRead(f.lsn, sectors, buf, &mode);
    sceCdSync(0);
    SyncDCache(buf, buf + sectors * 2048 + 4096);
    *size = f.size;
    return buf;
}

static int dataCB(void *user)
{
    unsigned int n;
    (void)user;
    if (s_Ptr >= s_End) return 0;
    n = (unsigned int)(s_End - s_Ptr);
    if (n > 2048) n = 2048;
    dma_channel_wait(DMA_CHANNEL_toIPU, 0);
    dma_channel_send_normal(DMA_CHANNEL_toIPU, s_Ptr, (n + 15) >> 4, 0, 0);
    s_Ptr += 2048;
    return 1;
}

static void *initCB(void *user, MPEGSequenceInfo *si)
{
    (void)user;
    s_Info = si;
    s_Pict = memalign(64, si->m_Width * si->m_Height * 4);
    printf("mpeg: sequence %dx%d chroma %d\n", si->m_Width, si->m_Height, si->m_ChromaFmt);
    return s_Pict;
}

static double framePsnr(const unsigned char *pict, const unsigned char *ref, int w, int h)
{
    // pict: 16x16 RGBA macroblocks in raster order; ref: RGB24 raster.
    double se = 0;
    int x, y, c;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            const unsigned char *p = pict + ((y >> 4) * (w >> 4) + (x >> 4)) * 1024 + ((y & 15) * 16 + (x & 15)) * 4;
            const unsigned char *r = ref + (y * w + x) * 3;
            for (c = 0; c < 3; c++) {
                double d = (double)p[c] - (double)r[c];
                se += d * d;
            }
        }
    se /= (double)(w * h * 3);
    if (se < 1e-9) return 99.0;
    return 10.0 * log10(255.0 * 255.0 / se);
}

static void runClip(const char *label, const char *video, const char *refName)
{
    unsigned int vsize = 0, rsize = 0;
    unsigned char *ref;
    s64 curPts = 0, pts = 0;
    int frames = 0;
    double minPsnr = 99.0;
    s_Data = loadFile(video, &vsize);
    ref = loadFile(refName, &rsize);
    if (!s_Data || !ref) {
        printf("mpeg: %s missing files\n", label);
        return;
    }
    s_Ptr = s_Data;
    s_End = s_Data + vsize;
    s_Pict = NULL;
    MPEG_Initialize(dataCB, NULL, initCB, NULL, &curPts);
    while (MPEG_Picture(s_Pict, &pts)) {
        const int w = s_Info->m_Width, h = s_Info->m_Height;
        const unsigned int fsz = (unsigned int)(w * h * 3);
        double p = 0;
        FlushCache(0);
        if ((unsigned int)(frames + 1) * fsz <= rsize) p = framePsnr(s_Pict, ref + frames * fsz, w, h);
        printf("mpeg: %s frame %d psnr %d.%d\n", label, frames, (int)p, (int)(p * 10) % 10);
#ifdef DUMP
        {
            int x, y;
            static char line[96 * 6 + 8];
            for (y = 0; y < h; y++) {
                char *o = line;
                for (x = 0; x < w; x++) {
                    const unsigned char *q = s_Pict + ((y >> 4) * (w >> 4) + (x >> 4)) * 1024 + ((y & 15) * 16 + (x & 15)) * 4;
                    o += sprintf(o, "%02x%02x%02x", q[0], q[1], q[2]);
                }
                printf("D %s\n", line);
            }
        }
#endif
        if (p < minPsnr) minPsnr = p;
        frames++;
    }
    MPEG_Destroy();
    printf("mpeg: %s frames %d\n", label, frames);
    printf("mpeg: %s quality ok %d\n", label, frames > 0 && minPsnr >= 30.0);
    free(s_Data);
    free(ref);
}

int main(void)
{
    SifInitRpc(0);
    sceCdInit(SCECdINIT);
    dma_channel_initialize(DMA_CHANNEL_toIPU, NULL, 0);
    runClip("mpeg2", "\\CLIP2.M2V;1", "\\CLIP2.RGB;1");
    runClip("mpeg1", "\\CLIP1.M1V;1", "\\CLIP1.RGB;1");
    printf("mpeg: done\n");
    return 0;
}
