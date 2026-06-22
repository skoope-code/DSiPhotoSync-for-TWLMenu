// DSiPhotoSync - sync DSi camera JPEGs into the TWiLightMenu++ slideshow folder.
//
// Scans sd:/DCIM/ for .jpg files, decodes each, downscales to fit TWiLight's
// top-screen photo limit (208x156 max), and writes a PNG into
// sd:/_nds/TWiLightMenu/dsimenu/photos/ so TWiLight's stock auto-cycle picks
// them up. A photo is treated as already converted when its output PNG exists,
// so repeat runs only touch new photos and deleting a PNG regenerates it.
//
// File map (top to bottom):
//   - JPEG decode (picojpeg glue) + downscale
//   - JPEG preview decode (reduce mode, for hover thumbnails)
//   - PNG writer (minimal, stored-deflate) and PNG reader (its own files)
//   - path helpers (make_out_name / make_src_path / copy_name)
//   - framebuffer primitives (pixels, rects, dim, blit, text, button glyphs)
//   - photo list build + single-photo conversion
//   - preview cache (settle-gated) + gallery thumbnail cache
//   - UI: colors, chrome, list rows, gallery grid, footer hints
//   - controls legend (drawn on the opposite/sub screen)
//   - settings (load/save, apply UI screen)
//   - shared list-screen helpers (navigate / preview / return-to-menu)
//   - main(): dual-screen video setup, then the screen state machine

#include <nds.h>
#include <fat.h>
#include <filesystem.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

#include "puff.h"   /* tiny inflate, for reading compressed theme PNGs */
#include "frame_data.h"   /* embedded top-screen frame overlay (PNG bytes) */
#include "shoulder_data.h" /* embedded cleaned L/R shoulder photo overlays */
#include "default_preview_data.h" /* embedded preview for the Default theme row */

#include "picojpeg.h"

// TWiLight's dsimenu top-screen photo loader rejects images larger than
// 208x156 (renders them black), so output must stay within this. Kept at 4:3
// to match the DSi camera's 640x480 so there's no distortion.
#define OUT_W 208
#define OUT_H 156

// picojpeg stores each 8x8 decoded block as 64 consecutive bytes in its
// per-MCU R/G/B buffers.
#define PJPG_BLOCK_PIXELS 64

// Upper bound for the PNG reader's in-RAM load (our own output PNGs are ~95KB;
// this just guards against trying to slurp a garbage file). JPEGs are no longer
// size-capped: they stream from disk through the decode callback.
#define MAX_PNG_BYTES (8 * 1024 * 1024)

#define DCIM_DIR   "sd:/DCIM"
#define PHOTOS_DIR "sd:/_nds/TWiLightMenu/dsimenu/photos"
#define THEMES_DIR "sd:/_nds/TWiLightMenu/dsimenu/themes"
#define TWL_SETTINGS_PATH "sd:/_nds/TWiLightMenu/settings.ini"
// Folder name written to DSI_THEME for the built-in "Default theme" row, and
// excluded from the regular theme lists (shown only as the pinned Default row).
#define DEFAULT_THEME_NAME "Default"

// Custom error codes (start high to stay clear of picojpeg's small enum).
// g_last_err carries the most recent failure reason so the UI can print
// exactly what went wrong on a given photo.
#define ERR_NOMEM_RGB    200
#define ERR_SHORT_DECODE 201
#define ERR_FILE_OPEN    202
#define ERR_FILE_READ    203
#define ERR_PNG_WRITE    204
#define ERR_TOO_BIG      205   // full-res decode would exceed the RAM budget
static int g_last_err = 0;

// Max pixels we'll decode at full resolution. W*H*3 bytes are allocated for the
// RGB buffer, so this caps that near ~4MB (margin under the DSi's 16MB). Larger
// images are decoded in reduce mode (1/8 each axis) instead, which is still far
// more resolution than the 208x156 output needs.
#define DECODE_FULL_MAX_PIXELS (1400L * 1400L)

// "Riddle of the day": shown on the otherwise-idle second screen in list view.
// One is chosen at boot. Each is a question (fits 256px wide) and its answer.
typedef struct { const char *q, *a; } Riddle;
static const Riddle g_riddles[] = {
    { "What has to be broken before you use it?", "An egg." },
    { "What has many keys but opens no locks?",   "A piano." },
    { "What gets wetter the more it dries?",      "A towel." },
    { "What has hands but cannot clap?",          "A clock." },
    { "What has a neck but no head?",             "A bottle." },
    { "What goes up but never comes down?",       "Your age." },
    { "What has an eye but cannot see?",          "A needle." },
    { "What has teeth but cannot bite?",          "A comb." },
    { "What runs but never walks?",               "Water." },
    { "The more you take, the more you leave.",   "Footsteps." },
    { "What can travel the world staying in?",     "A stamp." },
    { "What has one foot but four legs?",         "A bed." },
    { "What gets bigger the more you take away?", "A hole." },
    { "What has words but never speaks?",         "A book." },
    { "What building has the most stories?",      "A library." },
    { "What can you catch but not throw?",        "A cold." },
};
#define N_RIDDLES (int)(sizeof(g_riddles) / sizeof(g_riddles[0]))
static int g_riddle_idx = 0;   // chosen once at boot

// ---------------------------------------------------------------------------
// picojpeg glue: it pulls bytes from a callback. We feed it a whole file
// buffered in RAM (DSi cam JPEGs are well under a megabyte).
// ---------------------------------------------------------------------------
// picojpeg pulls bytes sequentially through this callback. To support large
// camera JPEGs (often >8MB) without loading the whole file into RAM, we stream
// directly from disk: the callback fread()s the next chunk on demand. Peak RAM
// is then just the small decode buffer, not the whole compressed file.
static FILE *g_jpg_file = NULL;

static unsigned char pj_read(unsigned char *pBuf, unsigned char bufSize,
                             unsigned char *pBytesActuallyRead, void *pData) {
    (void)pData;
    size_t n = 0;
    if (g_jpg_file) n = fread(pBuf, 1, bufSize, g_jpg_file);
    *pBytesActuallyRead = (unsigned char)n;
    return 0;   // picojpeg treats a short/zero read as end-of-stream
}

// Open a JPEG for streaming decode. Returns 0 on success.
static int jpg_open(const char *path) {
    g_jpg_file = fopen(path, "rb");
    if (!g_jpg_file) { g_last_err = ERR_FILE_OPEN; return -1; }
    return 0;
}

static void jpg_close(void) {
    if (g_jpg_file) { fclose(g_jpg_file); g_jpg_file = NULL; }
}

// Decode a full JPEG into RGB888 (caller frees). Returns 0 on ok. Oversized
// images (beyond the RAM budget) return -1 with ERR_TOO_BIG so the caller can
// skip them — this app targets DSi photos, which are small.
static int decode_jpeg(const char *path,
                       unsigned char **out_rgb, int *out_w, int *out_h) {
    if (jpg_open(path) != 0) return -1;

    pjpeg_image_info_t info;
    unsigned char rc = pjpeg_decode_init(&info, pj_read, NULL, 0);
    if (rc != 0) { jpg_close(); g_last_err = rc; return -1; }

    int W = info.m_width, H = info.m_height;
    // Guard the full-resolution allocation. A 24MP camera photo would need
    // ~72MB here, far beyond the DSi's 16MB. Oversized images return
    // ERR_TOO_BIG so the caller can fall back to a reduced (1/8) decode.
    if ((long)W * H > DECODE_FULL_MAX_PIXELS) {
        jpg_close();
        g_last_err = ERR_TOO_BIG;
        return -1;
    }
    unsigned char *rgb = (unsigned char *)malloc((size_t)W * H * 3);
    if (!rgb) { jpg_close(); g_last_err = ERR_NOMEM_RGB; return -1; }

    // picojpeg decodes one MCU at a time into m_pMCUBufR/G/B. The number of
    // 8x8 blocks per MCU and their layout depend on the scan type; the blocks
    // are stored consecutively (block 0 at offset 0, block 1 at 64, etc.):
    //   GRAYSCALE / H1V1 : 1 block  (8x8)
    //   H2V1             : 2 blocks side by side (16x8)
    //   H1V2             : 2 blocks stacked     (8x16)
    //   H2V2             : 4 blocks quadrant    (16x16)
    int mcuW = info.m_MCUWidth, mcuH = info.m_MCUHeight;
    int mcusPerRow = info.m_MCUSPerRow;
    int mcusPerCol = info.m_MCUSPerCol;

    for (int my = 0; my < mcusPerCol; my++) {
        for (int mx = 0; mx < mcusPerRow; mx++) {
            unsigned char status = pjpeg_decode_mcu();
            if (status) {
                free(rgb); jpg_close();
                g_last_err = (status == PJPG_NO_MORE_BLOCKS) ? ERR_SHORT_DECODE : status;
                return -1;
            }
            // Blocks fill the buffer left-to-right, top-to-bottom within the MCU.
            int blockIdx = 0;
            for (int by = 0; by < mcuH; by += 8) {
                for (int bx = 0; bx < mcuW; bx += 8) {
                    int src = blockIdx * PJPG_BLOCK_PIXELS;
                    int x0 = mx * mcuW + bx;
                    int y0 = my * mcuH + by;
                    if (x0 + 8 <= W && y0 + 8 <= H) {
                        // Fast path: the whole 8x8 block is inside the image, so
                        // skip the per-pixel bounds checks and address multiply --
                        // walk source and destination with plain pointers. This is
                        // the overwhelmingly common case (only right/bottom edge
                        // blocks clip). Output is byte-for-byte identical.
                        const unsigned char *R = info.m_pMCUBufR + src;
                        const unsigned char *G = info.m_pMCUBufG + src;
                        const unsigned char *B = info.m_pMCUBufB + src;
                        for (int y = 0; y < 8; y++) {
                            unsigned char *d = rgb + ((size_t)(y0 + y) * W + x0) * 3;
                            const unsigned char *rr = R + y * 8;
                            const unsigned char *gg = G + y * 8;
                            const unsigned char *bb = B + y * 8;
                            for (int x = 0; x < 8; x++) {
                                *d++ = *rr++; *d++ = *gg++; *d++ = *bb++;
                            }
                        }
                    } else {
                        // Edge block: clip per pixel.
                        for (int y = 0; y < 8; y++) {
                            int py = y0 + y;
                            if (py >= H) break;
                            for (int x = 0; x < 8; x++) {
                                int px = x0 + x;
                                if (px >= W) continue;
                                int s = src + y * 8 + x;
                                unsigned char *d = rgb + ((size_t)py * W + px) * 3;
                                d[0] = info.m_pMCUBufR[s];
                                d[1] = info.m_pMCUBufG[s];
                                d[2] = info.m_pMCUBufB[s];
                            }
                        }
                    }
                    blockIdx++;
                }
            }
        }
    }

    jpg_close();
    *out_rgb = rgb; *out_w = W; *out_h = H;
    return 0;
}

// Nearest-neighbor downscale RGB888 -> arbitrary dw x dh RGB888.
static void downscale_to(const unsigned char *src, int sw, int sh,
                         unsigned char *dst, int dw, int dh) {
    for (int y = 0; y < dh; y++) {
        int sy = (int)((long)y * sh / dh);
        for (int x = 0; x < dw; x++) {
            int sx = (int)((long)x * sw / dw);
            const unsigned char *s = src + ((size_t)sy * sw + sx) * 3;
            unsigned char *d = dst + ((size_t)y * dw + x) * 3;
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
        }
    }
}

// Convenience wrapper: downscale to the fixed conversion output size.
static void downscale(const unsigned char *src, int sw, int sh,
                      unsigned char *dst) {
    downscale_to(src, sw, sh, dst, OUT_W, OUT_H);
}

// ---------------------------------------------------------------------------
// Fast preview decode: picojpeg "reduce" mode decodes only the first pixel of
// each 8x8 block, so the result is (W/8 x H/8) and ~8x faster + far less RAM
// than a full decode. Used for hover previews where quality doesn't matter.
// In reduce mode every block's single pixel lands at offset 0 of its block,
// laid out the same block order as full decode. Returns 0 on success and fills
// a caller-allocated RGB buffer plus the reduced dimensions.
// ---------------------------------------------------------------------------
static int decode_jpeg_preview(const char *path, unsigned char **out_rgb,
                               int *out_w, int *out_h) {
    if (jpg_open(path) != 0) return -1;

    pjpeg_image_info_t info;
    unsigned char rc = pjpeg_decode_init(&info, pj_read, NULL, 1); // reduce=1
    if (rc != 0) { jpg_close(); g_last_err = rc; return -1; }

    // In reduce mode the decoded image is one pixel per block: each MCU yields
    // (MCUWidth/8)*(MCUHeight/8) pixels. The reduced image size is the MCU grid
    // times the blocks-per-MCU in each axis.
    int blocksX = info.m_MCUWidth / 8;
    int blocksY = info.m_MCUHeight / 8;
    int RW = info.m_MCUSPerRow * blocksX;
    int RH = info.m_MCUSPerCol * blocksY;

    unsigned char *rgb = (unsigned char *)malloc((size_t)RW * RH * 3);
    if (!rgb) { jpg_close(); g_last_err = ERR_NOMEM_RGB; return -1; }

    for (int my = 0; my < info.m_MCUSPerCol; my++) {
        for (int mx = 0; mx < info.m_MCUSPerRow; mx++) {
            unsigned char status = pjpeg_decode_mcu();
            if (status) {
                free(rgb); jpg_close();
                g_last_err = (status == PJPG_NO_MORE_BLOCKS) ? ERR_SHORT_DECODE : status;
                return -1;
            }
            // Each block contributes one pixel, stored at the block's offset 0.
            int blockIdx = 0;
            for (int by = 0; by < blocksY; by++) {
                for (int bx = 0; bx < blocksX; bx++) {
                    int s = blockIdx * PJPG_BLOCK_PIXELS; // pixel 0 of this block
                    int px = mx * blocksX + bx;
                    int py = my * blocksY + by;
                    unsigned char *d = rgb + ((size_t)py * RW + px) * 3;
                    d[0] = info.m_pMCUBufR[s];
                    d[1] = info.m_pMCUBufG[s];
                    d[2] = info.m_pMCUBufB[s];
                    blockIdx++;
                }
            }
        }
    }

    jpg_close();
    *out_rgb = rgb; *out_w = RW; *out_h = RH;
    return 0;
}

// ---------------------------------------------------------------------------
// Minimal PNG writer: 8-bit RGB, single IDAT using stored (uncompressed)
// zlib/deflate blocks. No external deps. Big but TWiLight reads it fine.
// ---------------------------------------------------------------------------
static unsigned int crc_table[256];
static int crc_ready = 0;
static void crc_init(void) {
    for (unsigned int n = 0; n < 256; n++) {
        unsigned int c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_table[n] = c;
    }
    crc_ready = 1;
}
static unsigned int crc_upd(unsigned int crc, const unsigned char *buf, int len) {
    if (!crc_ready) crc_init();
    crc ^= 0xFFFFFFFFu;
    for (int i = 0; i < len; i++) crc = crc_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}
static unsigned int adler32(const unsigned char *d, long len) {
    unsigned int a = 1, b = 0;
    for (long i = 0; i < len; i++) { a = (a + d[i]) % 65521; b = (b + a) % 65521; }
    return (b << 16) | a;
}
static void wbe32(FILE *f, unsigned int v) {
    fputc((v>>24)&0xFF,f); fputc((v>>16)&0xFF,f); fputc((v>>8)&0xFF,f); fputc(v&0xFF,f);
}
static void wchunk(FILE *f, const char *type, const unsigned char *data, unsigned int len) {
    wbe32(f, len);
    unsigned int crc = crc_upd(0, (const unsigned char *)type, 4);
    if (len) crc = crc_upd(crc, data, len);
    fwrite(type, 1, 4, f);
    if (len) fwrite(data, 1, len, f);
    wbe32(f, crc);
}

static int write_png(const char *path, const unsigned char *rgb, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    static const unsigned char sig[8] = {137,80,78,71,13,10,26,10};
    fwrite(sig, 1, 8, f);

    unsigned char ihdr[13];
    ihdr[0]=(w>>24)&0xFF; ihdr[1]=(w>>16)&0xFF; ihdr[2]=(w>>8)&0xFF; ihdr[3]=w&0xFF;
    ihdr[4]=(h>>24)&0xFF; ihdr[5]=(h>>16)&0xFF; ihdr[6]=(h>>8)&0xFF; ihdr[7]=h&0xFF;
    ihdr[8]=8; ihdr[9]=2; ihdr[10]=0; ihdr[11]=0; ihdr[12]=0; // 8-bit, truecolor
    wchunk(f, "IHDR", ihdr, 13);

    // Raw scanlines: filter byte 0 + RGB row.
    long raw_len = (long)h * (1 + w * 3);
    unsigned char *raw = (unsigned char *)malloc(raw_len);
    if (!raw) { fclose(f); return -1; }
    long p = 0;
    for (int y = 0; y < h; y++) {
        raw[p++] = 0;
        memcpy(raw + p, rgb + (size_t)y * w * 3, w * 3);
        p += w * 3;
    }

    // zlib stream: 2-byte header + stored deflate blocks + adler32.
    long zcap = raw_len + (raw_len / 65535 + 1) * 5 + 2 + 4 + 16;
    unsigned char *z = (unsigned char *)malloc(zcap);
    if (!z) { free(raw); fclose(f); return -1; }
    long zp = 0;
    z[zp++] = 0x78; z[zp++] = 0x01; // zlib header, no preset dict
    long off = 0;
    while (off < raw_len) {
        long block = raw_len - off;
        if (block > 65535) block = 65535;
        int final = (off + block >= raw_len) ? 1 : 0;
        z[zp++] = final;                    // stored block, BFINAL flag
        z[zp++] = block & 0xFF;
        z[zp++] = (block >> 8) & 0xFF;
        z[zp++] = (~block) & 0xFF;
        z[zp++] = ((~block) >> 8) & 0xFF;
        memcpy(z + zp, raw + off, block);
        zp += block; off += block;
    }
    unsigned int ad = adler32(raw, raw_len);
    z[zp++]=(ad>>24)&0xFF; z[zp++]=(ad>>16)&0xFF; z[zp++]=(ad>>8)&0xFF; z[zp++]=ad&0xFF;

    wchunk(f, "IDAT", z, (unsigned int)zp);
    wchunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw); free(z);
    return 0;
}

// ---------------------------------------------------------------------------
// Minimal PNG reader for the library view. Only needs to read PNGs that THIS
// app wrote: 8-bit truecolor RGB, single zlib stream made of stored
// (uncompressed) deflate blocks, filter byte 0 on every scanline. That lets
// us skip a real inflate implementation. Returns 0 on success with a
// caller-freed RGB buffer and dimensions. Returns -1 on anything unexpected.
// ---------------------------------------------------------------------------
static unsigned int rbe32(const unsigned char *p) {
    return ((unsigned int)p[0]<<24)|((unsigned int)p[1]<<16)|((unsigned int)p[2]<<8)|p[3];
}

static int read_png(const char *path, unsigned char **out_rgb, int *out_w, int *out_h) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 57 || sz > MAX_PNG_BYTES) { fclose(f); return -1; }
    unsigned char *buf = (unsigned char *)malloc(sz);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, sz, f) != (size_t)sz) { fclose(f); free(buf); return -1; }
    fclose(f);

    static const unsigned char sig[8] = {137,80,78,71,13,10,26,10};
    if (memcmp(buf, sig, 8) != 0) { free(buf); return -1; }

    int W = 0, H = 0;
    // Concatenate all IDAT payloads into one zlib stream.
    unsigned char *zdat = (unsigned char *)malloc(sz); // upper bound
    if (!zdat) { free(buf); return -1; }
    long zlen = 0;
    long off = 8;
    int have_ihdr = 0;
    while (off + 8 <= sz) {
        unsigned int clen = rbe32(buf + off);
        const unsigned char *ctype = buf + off + 4;
        const unsigned char *cdata = buf + off + 8;
        if (off + 12 + (long)clen > sz) break;
        if (memcmp(ctype, "IHDR", 4) == 0 && clen >= 13) {
            W = (int)rbe32(cdata);
            H = (int)rbe32(cdata + 4);
            int bitdepth = cdata[8], colortype = cdata[9];
            if (bitdepth != 8 || colortype != 2) { free(buf); free(zdat); return -1; }
            have_ihdr = 1;
        } else if (memcmp(ctype, "IDAT", 4) == 0) {
            memcpy(zdat + zlen, cdata, clen);
            zlen += clen;
        } else if (memcmp(ctype, "IEND", 4) == 0) {
            break;
        }
        off += 12 + clen; // length + type + data + crc
    }
    free(buf);
    if (!have_ihdr || W <= 0 || H <= 0 || zlen < 2) { free(zdat); return -1; }

    // Walk the stored-deflate zlib stream: skip 2-byte zlib header, then a
    // series of stored blocks (1 flag byte + LEN + NLEN + LEN raw bytes),
    // collecting the raw (filtered scanline) bytes.
    long raw_cap = (long)H * (1 + W * 3);
    unsigned char *raw = (unsigned char *)malloc(raw_cap);
    if (!raw) { free(zdat); return -1; }
    long rp = 0;
    long p = 2; // skip zlib header (0x78 0x01)
    while (p + 5 <= zlen) {
        unsigned char final = zdat[p] & 1;
        int btype = (zdat[p] >> 1) & 3;
        if (btype != 0) { free(zdat); free(raw); return -1; } // only stored blocks
        long len = zdat[p+1] | (zdat[p+2] << 8);
        p += 5;
        if (p + len > zlen || rp + len > raw_cap) { free(zdat); free(raw); return -1; }
        memcpy(raw + rp, zdat + p, len);
        rp += len; p += len;
        if (final) break;
    }
    free(zdat);

    if (rp < raw_cap) { free(raw); return -1; } // truncated

    // Strip the per-scanline filter byte (always 0 in our writer).
    unsigned char *rgb = (unsigned char *)malloc((size_t)W * H * 3);
    if (!rgb) { free(raw); return -1; }
    for (int y = 0; y < H; y++) {
        const unsigned char *srow = raw + (long)y * (1 + W * 3) + 1;
        memcpy(rgb + (size_t)y * W * 3, srow, W * 3);
    }
    free(raw);

    *out_rgb = rgb; *out_w = W; *out_h = H;
    return 0;
}

// Paeth predictor (PNG filter type 4).
static int paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

// Full PNG decoder core: decodes an in-memory PNG (compressed, filtered, RGB/
// RGBA/palette) to RGBA. Used by both the file reader and the embedded frame.
static int decode_png_mem(const unsigned char *buf, long sz,
                          unsigned char **out_rgba, int *out_w, int *out_h) {
    if (sz < 57) return -1;
    static const unsigned char sig[8] = {137,80,78,71,13,10,26,10};
    if (memcmp(buf, sig, 8) != 0) return -1;

    int W = 0, H = 0, bitdepth = 0, colortype = -1;
    unsigned char *zdat = (unsigned char *)malloc(sz);   // upper bound
    unsigned char palette[256 * 3]; int pal_n = 0;
    unsigned char trns[256];        int trns_n = 0;      // palette alpha (tRNS)
    if (!zdat) return -1;
    long zlen = 0, off = 8;
    int have_ihdr = 0;
    while (off + 8 <= sz) {
        unsigned int clen = rbe32(buf + off);
        const unsigned char *ctype = buf + off + 4;
        const unsigned char *cdata = buf + off + 8;
        if (off + 12 + (long)clen > sz) break;
        if (memcmp(ctype, "IHDR", 4) == 0 && clen >= 13) {
            W = (int)rbe32(cdata); H = (int)rbe32(cdata + 4);
            bitdepth = cdata[8]; colortype = cdata[9];
            if (cdata[12] != 0) { free(zdat); return -1; }   // no interlace
            // Bit depths: 8 for all types; 1/2/4 only for palette(3)/grayscale(0).
            int bd_ok = (bitdepth == 8) ||
                        ((bitdepth == 1 || bitdepth == 2 || bitdepth == 4) &&
                         (colortype == 3 || colortype == 0));
            if (!bd_ok) { free(zdat); return -1; }
            if (colortype != 0 && colortype != 2 && colortype != 3 &&
                colortype != 4 && colortype != 6) { free(zdat); return -1; }
            have_ihdr = 1;
        } else if (memcmp(ctype, "PLTE", 4) == 0) {
            int n = clen / 3; if (n > 256) n = 256;
            memcpy(palette, cdata, n * 3); pal_n = n;
        } else if (memcmp(ctype, "tRNS", 4) == 0) {
            int n = clen; if (n > 256) n = 256;
            memcpy(trns, cdata, n); trns_n = n;
        } else if (memcmp(ctype, "IDAT", 4) == 0) {
            if (zlen + (long)clen <= sz) { memcpy(zdat + zlen, cdata, clen); zlen += clen; }
        } else if (memcmp(ctype, "IEND", 4) == 0) {
            break;
        }
        off += 12 + clen;
    }
    if (!have_ihdr || W <= 0 || H <= 0 || zlen < 3) { free(zdat); return -1; }

    int channels = (colortype == 2) ? 3 : (colortype == 6) ? 4 :
                   (colortype == 4) ? 2 : 1;   // 0/3 = 1, 4 = 2 (gray+alpha)
    // Bytes per scanline: pixels are packed when bitdepth < 8.
    long stride = ((long)W * channels * bitdepth + 7) / 8;
    long raw_cap = H * (1 + stride);

    unsigned char *raw = (unsigned char *)malloc(raw_cap);
    if (!raw) { free(zdat); return -1; }
    unsigned long destlen = raw_cap, srclen = zlen - 2;
    int prc = puff(raw, &destlen, zdat + 2, &srclen);
    free(zdat);
    if (prc != 0 || (long)destlen != raw_cap) { free(raw); return -1; }

    int bpp = (bitdepth < 8) ? 1 : channels;   // filter step in bytes
    for (int y = 0; y < H; y++) {
        unsigned char *row = raw + (long)y * (1 + stride);
        int ft = row[0];
        unsigned char *cur = row + 1;
        const unsigned char *prev = (y > 0) ? (raw + (long)(y - 1) * (1 + stride) + 1) : NULL;
        for (long i = 0; i < stride; i++) {
            int a = (i >= bpp) ? cur[i - bpp] : 0;
            int b = prev ? prev[i] : 0;
            int c = (prev && i >= bpp) ? prev[i - bpp] : 0;
            int v = cur[i];
            switch (ft) {
                case 0: break;
                case 1: v = (v + a) & 0xff; break;
                case 2: v = (v + b) & 0xff; break;
                case 3: v = (v + ((a + b) >> 1)) & 0xff; break;
                case 4: v = (v + paeth(a, b, c)) & 0xff; break;
                default: free(raw); return -1;
            }
            cur[i] = (unsigned char)v;
        }
    }

    unsigned char *rgba = (unsigned char *)malloc((size_t)W * H * 4);
    if (!rgba) { free(raw); return -1; }
    int maxval = (1 << bitdepth) - 1;   // for grayscale scaling at low depths
    for (int y = 0; y < H; y++) {
        const unsigned char *src = raw + (long)y * (1 + stride) + 1;
        unsigned char *dst = rgba + (size_t)y * W * 4;
        for (int x = 0; x < W; x++) {
            if (bitdepth < 8) {
                // Extract the packed sample for pixel x (MSB-first within a byte).
                int per_byte = 8 / bitdepth;
                int byte = src[x / per_byte];
                int shift = 8 - bitdepth * (x % per_byte + 1);
                int s = (byte >> shift) & maxval;
                if (colortype == 3) {            // palette index
                    int idx = s; if (idx >= pal_n) idx = 0;
                    dst[0]=palette[idx*3+0]; dst[1]=palette[idx*3+1]; dst[2]=palette[idx*3+2];
                    dst[3]=(idx < trns_n) ? trns[idx] : 255;
                } else {                          // grayscale: scale to 0..255
                    int g = s * 255 / maxval;
                    dst[0]=g; dst[1]=g; dst[2]=g; dst[3]=255;
                }
            } else if (colortype == 2) {
                dst[0]=src[x*3+0]; dst[1]=src[x*3+1]; dst[2]=src[x*3+2]; dst[3]=255;
            } else if (colortype == 6) {
                dst[0]=src[x*4+0]; dst[1]=src[x*4+1]; dst[2]=src[x*4+2]; dst[3]=src[x*4+3];
            } else if (colortype == 0) {
                int g = src[x]; dst[0]=g; dst[1]=g; dst[2]=g; dst[3]=255;
            } else if (colortype == 4) {
                int g = src[x*2+0]; dst[0]=g; dst[1]=g; dst[2]=g; dst[3]=src[x*2+1];
            } else {
                int idx = src[x]; if (idx >= pal_n) idx = 0;
                dst[0]=palette[idx*3+0]; dst[1]=palette[idx*3+1]; dst[2]=palette[idx*3+2];
                dst[3]=(idx < trns_n) ? trns[idx] : 255;
            }
            dst += 4;
        }
    }
    free(raw);
    *out_rgba = rgba; *out_w = W; *out_h = H;
    return 0;
}

// File wrapper around decode_png_mem: loads the file then decodes it to RGBA.
static int read_png_full(const char *path, unsigned char **out_rgba,
                         int *out_w, int *out_h) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 57 || sz > MAX_PNG_BYTES) { fclose(f); return -1; }
    unsigned char *buf = (unsigned char *)malloc(sz);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, sz, f) != (size_t)sz) { fclose(f); free(buf); return -1; }
    fclose(f);
    int rc = decode_png_mem(buf, sz, out_rgba, out_w, out_h);
    free(buf);
    return rc;
}

// ---------------------------------------------------------------------------
// "Already converted?" is derived directly from the filesystem: a photo is
// done iff its output PNG already exists. No separate manifest to keep in
// sync, so deleting a PNG from the photos folder simply makes it regenerate.
// ---------------------------------------------------------------------------
static int output_exists(const char *out_png) {
    struct stat st;
    return stat(out_png, &st) == 0;
}

// Turn "DCIM/100NIN03/HNI_0001.JPG" into a flat, unique-ish output name.
static void make_out_name(const char *subdir, const char *file, char *out, int outsz) {
    snprintf(out, outsz, "%s/%s_%s.png", PHOTOS_DIR, subdir, file);
    for (char *p = out + strlen(PHOTOS_DIR) + 1; *p; p++)
        if (*p == '.' && strcasecmp(p, ".png") != 0) *p = '_';
}

// Build the full source-JPEG path for a photo entry. Buffer should be >=400
// to match make_out_name and comfortably hold DCIM + subdir + file.
static void make_src_path(const char *subdir, const char *file, char *out, int outsz) {
    snprintf(out, outsz, "%s/%s/%s", DCIM_DIR, subdir, file);
}

static int has_jpg_ext(const char *n) {
    const char *d = strrchr(n, '.');
    return d && (!strcasecmp(d, ".jpg") || !strcasecmp(d, ".jpeg"));
}

// Copy a directory-entry name into a fixed field. Returns 0 on success, or -1
// if the name is too long to fit (caller skips it rather than storing a
// truncated, possibly-colliding name). DCIM names are short in practice, so
// this only guards against pathological input.
static int copy_name(char *dst, int dstsz, const char *src) {
    int len = (int)strlen(src);
    if (len >= dstsz) return -1;
    memcpy(dst, src, len + 1);
    return 0;
}

// ===========================================================================
// Bottom-screen graphical UI layer.
//
// We render EVERYTHING into a single 256x192 16-bit framebuffer (one bitmap
// background on the main engine). Text is drawn with a built-in 6x8 font and
// the photo preview is blitted into the same buffer. Using one bitmap path
// (instead of console-text layered over a bitmap) avoids the most common
// first-run DS pitfall: console/background layer conflicts.
//
// The DS framebuffer is BGR555 with the top bit as alpha/opacity. A pixel is
// ABGR1555: bit15=1 (opaque), then 5 bits each of B,G,R.
// ===========================================================================
#define SCR_W 256
#define SCR_H 192

static u16 *g_fb = NULL;       // main engine VRAM framebuffer
static u16 *g_fb_sub = NULL;   // sub engine VRAM framebuffer (legend screen)
static u16 *g_draw_fb = NULL;  // current draw target (points at one of the above)

static inline u16 rgb15(unsigned char r, unsigned char g, unsigned char b) {
    return 0x8000 | ((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3);
}

static inline void fb_px(int x, int y, u16 c) {
    if ((unsigned)x < SCR_W && (unsigned)y < SCR_H) g_draw_fb[y * SCR_W + x] = c;
}

static void fb_rect(int x, int y, int w, int h, u16 c) {
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++)
            fb_px(x + xx, y + yy, c);
}

// Darken the existing pixels in a rect (multiply each channel toward black).
// Used to dim the screen behind a modal popup without erasing what's there.
static void fb_dim_rect(int x, int y, int w, int h) {
    for (int yy = 0; yy < h; yy++) {
        int py = y + yy;
        if ((unsigned)py >= SCR_H) continue;
        for (int xx = 0; xx < w; xx++) {
            int px = x + xx;
            if ((unsigned)px >= SCR_W) continue;
            u16 v = g_fb[py * SCR_W + px];
            int r = v & 31, g = (v >> 5) & 31, b = (v >> 10) & 31;
            // ~35% brightness keeps a hint of the image while clearly dimming.
            r = r * 9 / 25; g = g * 9 / 25; b = b * 9 / 25;
            g_fb[py * SCR_W + px] = 0x8000 | (b << 10) | (g << 5) | r;
        }
    }
}

// Blit an RGB888 image, nearest-neighbour scaled, into a framebuffer rect.
static void fb_blit_rgb(const unsigned char *rgb, int sw, int sh,
                        int dx, int dy, int dw, int dh) {
    for (int y = 0; y < dh; y++) {
        int sy = (int)((long)y * sh / dh);
        for (int x = 0; x < dw; x++) {
            int sx = (int)((long)x * sw / dw);
            const unsigned char *s = rgb + ((size_t)sy * sw + sx) * 3;
            fb_px(dx + x, dy + y, rgb15(s[0], s[1], s[2]));
        }
    }
}

// Off-screen scratch the size of one screen. Used to compose a full-screen image
// (e.g. the expanded photo) without timing pressure, then push it to the live
// framebuffer in a single fast pass right after VBlank -- so the slow scaled
// blit never shows mid-draw (no top/bottom split).
static u16 g_scratch[SCR_W * SCR_H];

// Scale an RGB image into the scratch buffer (fills the whole screen).
static void scratch_blit_rgb(const unsigned char *rgb, int sw, int sh) {
    for (int y = 0; y < SCR_H; y++) {
        int sy = (int)((long)y * sh / SCR_H);
        for (int x = 0; x < SCR_W; x++) {
            int sx = (int)((long)x * sw / SCR_W);
            const unsigned char *s = rgb + ((size_t)sy * sw + sx) * 3;
            g_scratch[y * SCR_W + x] = rgb15(s[0], s[1], s[2]);
        }
    }
}

// Push the scratch buffer to the live main framebuffer in one fast 32-bit copy
// (two pixels per write), synced to VBlank so the whole screen updates at once.
static void scratch_present(void) {
    swiWaitForVBlank();
    u32 *d = (u32 *)g_fb, *src = (u32 *)g_scratch;
    for (int i = 0; i < SCR_W * SCR_H / 2; i++) d[i] = src[i];
}

// ---- 6x8 bitmap font: ASCII 32..126. Each glyph is 8 rows of a 6-bit mask
// (top 6 bits of each byte used). Compact but legible. ------------------------
#include "font6x8.h"

static void fb_char(int x, int y, char ch, u16 col) {
    if (ch < 32 || ch > 126) ch = '?';
    const unsigned char *g = font6x8[(int)ch - 32];
    for (int row = 0; row < 8; row++) {
        unsigned char bits = g[row];
        for (int c = 0; c < 6; c++) {
            if (bits & (0x80 >> c)) fb_px(x + c, y + row, col);
        }
    }
}

static void fb_text(int x, int y, const char *s, u16 col) {
    int cx = x;
    for (; *s; s++) {
        if (*s == '\n') { y += 9; cx = x; continue; }
        fb_char(cx, y, *s, col);
        cx += 6;
    }
}


// Draw text truncated to a max pixel width (adds nothing if it fits).
static void fb_text_clip(int x, int y, const char *s, u16 col, int maxchars) {
    char buf[64];
    int n = 0;
    while (s[n] && n < maxchars && n < (int)sizeof(buf) - 1) { buf[n] = s[n]; n++; }
    buf[n] = 0;
    fb_text(x, y, buf, col);
}

// Draw a round face-button glyph: a filled circle in `col` with the character
// `ch` knocked out (in `bg`) so it shows through as an outline. Returns width.
// The font's visible ink is exactly 5px wide (cols 0-4) x 7px tall (rows 0-6),
// drawn at scale 1, centered in a small circle so the letter has clear margin.
#define BTN_D    13      // button diameter
#define GLYPH_W   5      // visible ink width  (font cols 0-4)
#define GLYPH_H   7      // visible ink height (font rows 0-6)
static int fb_button(int x, int y, char ch, u16 col, u16 bg) {
    int r = (BTN_D - 1) / 2;     // 6, circle center at (6,6)
    for (int yy = 0; yy < BTN_D; yy++) {
        for (int xx = 0; xx < BTN_D; xx++) {
            int dx = xx - r, dy = yy - r;
            // strict < drops the four cardinal-point nubs for a cleaner circle
            if (dx*dx + dy*dy < r*r) fb_px(x + xx, y + yy, col);
        }
    }
    // Center the 5x7 ink. Font col 0 is the ink's left edge, row 0 its top.
    int ox = (BTN_D - GLYPH_W) / 2;
    int oy = (BTN_D - GLYPH_H) / 2;
    const unsigned char *g = font6x8[(int)ch - 32];
    for (int row = 0; row < 8; row++) {
        unsigned char bits = g[row];
        for (int c = 0; c < 6; c++) {
            if (bits & (0x80 >> c)) fb_px(x + ox + c, y + oy + row, bg);
        }
    }
    return BTN_D;
}

// Like fb_button but draws a small triangle (direction arrow) instead of a
// letter. dir: 0=up, 1=down, 2=left, 3=right. The 4-deep triangle is positioned
// so it sits centered in the circle with margin on all sides.
static int fb_button_arrow(int x, int y, int dir, u16 col, u16 bg) {
    int r = (BTN_D - 1) / 2;     // 6
    for (int yy = 0; yy < BTN_D; yy++)
        for (int xx = 0; xx < BTN_D; xx++) {
            int dx = xx - r, dy = yy - r;
            if (dx*dx + dy*dy < r*r) fb_px(x + xx, y + yy, col);
        }
    const int DEPTH = 4;         // rows from tip to base; widths 1,3,5,7
    const int OFFV = 5;          // vertical span (up/down): rows 5..8, centered
    const int OFFH = 4;          // horizontal span (left/right): cols 4..7, centered
    for (int k = 0; k < DEPTH; k++) {
        for (int w = -k; w <= k; w++) {
            int cx, cy;
            if (dir == 0)      { cx = r + w;        cy = OFFV + k; }            // up
            else if (dir == 1) { cx = r + w;        cy = OFFV + (DEPTH-1 - k); }// down
            else if (dir == 2) { cx = OFFH + k;     cy = r + w; }              // left
            else               { cx = OFFH + (DEPTH-1 - k); cy = r + w; }      // right
            fb_px(x + cx, y + cy, bg);
        }
    }
    return BTN_D;
}

// ---------------------------------------------------------------------------
// Phase 1: build a list of every JPEG under DCIM. We store the subdir and
// filename for each so phase 2 can show a review list and a progress bar.
// ---------------------------------------------------------------------------
#define MAX_PHOTOS 1024
typedef struct {
    char subdir[64];
    char file[64];
    int  done;      // already converted (output PNG exists)?
    int  convert;   // checkbox state for unconverted photos (default on)
} PhotoEntry;

static PhotoEntry g_photos[MAX_PHOTOS];
static int g_photo_count = 0;
static int g_hit_max = 0;   // set if DCIM held more photos than MAX_PHOTOS

static void build_list(void) {
    DIR *top = opendir(DCIM_DIR);
    if (!top) return;
    struct dirent *e;
    while ((e = readdir(top)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char sub[300];
        snprintf(sub, sizeof(sub), "%s/%s", DCIM_DIR, e->d_name);
        struct stat st;
        if (stat(sub, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        DIR *d = opendir(sub);
        if (!d) continue;
        struct dirent *fe;
        while ((fe = readdir(d)) != NULL) {
            if (!has_jpg_ext(fe->d_name)) continue;
            if (g_photo_count >= MAX_PHOTOS) { g_hit_max = 1; break; }
            PhotoEntry *p = &g_photos[g_photo_count];
            if (copy_name(p->subdir, sizeof(p->subdir), e->d_name) != 0) continue;
            if (copy_name(p->file,   sizeof(p->file),   fe->d_name) != 0) continue;
            char outname[400];
            make_out_name(p->subdir, p->file, outname, sizeof(outname));
            p->done = output_exists(outname);
            p->convert = !p->done;   // default: convert everything not yet done
            g_photo_count++;
        }
        closedir(d);
    }
    closedir(top);
}

// Convert a single entry. Returns 1 on success, 0 on failure (g_last_err set),
// -1 if skipped (already done).
static int convert_entry(PhotoEntry *p) {
    if (p->done) return -1;

    char fullpath[400];
    make_src_path(p->subdir, p->file, fullpath, sizeof(fullpath));

    g_last_err = 0;
    unsigned char *rgb = NULL; int w = 0, h = 0;
    int decoded = 0;

    // DSi photos are 640x480 and decode quickly at full resolution. Anything
    // too large to decode within the RAM budget is skipped cleanly (this app is
    // for DSi-taken photos; oversized camera photos aren't supported).
    if (decode_jpeg(fullpath, &rgb, &w, &h) == 0) decoded = 1;
    if (!decoded) return 0;

    static unsigned char small[OUT_W * OUT_H * 3];
    downscale(rgb, w, h, small);
    free(rgb);

    char outname[400];
    make_out_name(p->subdir, p->file, outname, sizeof(outname));
    if (write_png(outname, small, OUT_W, OUT_H) != 0) { g_last_err = ERR_PNG_WRITE; return 0; }

    return 1;
}

// ===========================================================================
// Preview cache: decode the highlighted photo's preview only when the
// selection settles, so fast scrolling stays responsive.
// ===========================================================================
static unsigned char *g_prev_rgb = NULL;
static int g_prev_w = 0, g_prev_h = 0;
static int g_prev_for = -1;   // index the cached preview belongs to (-1 = none)

static void preview_free(void) {
    if (g_prev_rgb) { free(g_prev_rgb); g_prev_rgb = NULL; }
    g_prev_w = g_prev_h = 0;
    g_prev_for = -1;
}

// Load a JPEG preview for an unconverted photo at list index i. Prefers the
// embedded EXIF thumbnail (near-instant); falls back to a reduce-mode decode.
// Load a JPEG hover preview (reduce mode, ~1/8 size) for unconverted photo i.
static void preview_load_jpeg(int i) {
    preview_free();
    char fullpath[400];
    make_src_path(g_photos[i].subdir, g_photos[i].file, fullpath, sizeof(fullpath));
    unsigned char *rgb = NULL; int w = 0, h = 0;
    if (decode_jpeg_preview(fullpath, &rgb, &w, &h) == 0) {
        g_prev_rgb = rgb; g_prev_w = w; g_prev_h = h; g_prev_for = i;
    }
}

// Load a converted-PNG preview for library index i.
static void preview_load_png(int i) {
    preview_free();
    char outname[400];
    make_out_name(g_photos[i].subdir, g_photos[i].file, outname, sizeof(outname));
    unsigned char *rgb = NULL; int w = 0, h = 0;
    if (read_png(outname, &rgb, &w, &h) == 0) {
        g_prev_rgb = rgb; g_prev_w = w; g_prev_h = h; g_prev_for = i;
    }
}

// ---------------------------------------------------------------------------
// Gallery (grid) view: a 3x2 page of thumbnails. Up to GRID_CELLS previews are
// decoded into slots and blitted. Decoding is done one slot per frame
// (progressive) so flipping pages stays responsive even when six source JPEGs
// must be decoded.
// ---------------------------------------------------------------------------
#define GRID_COLS   3
#define GRID_ROWS   2
#define GRID_CELLS  (GRID_COLS * GRID_ROWS)
#define GRID_TOP    46
#define GRID_CELL_W 80
#define GRID_MARGIN_X ((SCR_W - GRID_COLS * GRID_CELL_W) / 2)   // 8
#define THUMB_W     64
#define THUMB_H     48
#define CELL_GAP_Y  12
#define LABEL_GAP   3
#define GRID_CELL_H (THUMB_H + LABEL_GAP + 8)                    // 59
#define GRID_INFO_Y 182                                          // bottom info line

// Gallery thumbnail cache. Thumbnails are pre-scaled to THUMB_W x THUMB_H at
// load time (so redraws blit without rescaling, and the cache stays tiny), and
// keyed by photo index so revisiting a page is instant -- no re-decode. Entries
// are kept in an LRU ring; we hold a sliding window of ~50 thumbnails (24 behind
// + 24 ahead of the current view) and a little headroom so the window doesn't
// evict itself. 56 * 64*48*2 bytes ~= 344KB, comfortably within RAM.
#define THUMB_CACHE  56
#define THUMB_WINDOW 24          // how many to keep cached behind/ahead of view
typedef struct {
    int   photo_idx;             // which photo (-1 = empty slot)
    unsigned long stamp;         // LRU stamp
    u16   px[THUMB_W * THUMB_H]; // pre-scaled ABGR1555 thumbnail
    int   ok;                    // decoded successfully?
} ThumbEntry;
static ThumbEntry g_thumbs[THUMB_CACHE];
static unsigned long g_thumb_clock = 0;

static void grid_free(void) {
    for (int i = 0; i < THUMB_CACHE; i++) g_thumbs[i].photo_idx = -1;
}

// Drop a single photo's cached thumbnail (e.g. after it's deleted) without
// disturbing the rest of the cache.
static void thumb_invalidate(int photo_idx) {
    for (int i = 0; i < THUMB_CACHE; i++)
        if (g_thumbs[i].photo_idx == photo_idx) g_thumbs[i].photo_idx = -1;
}

// Find the cache entry for photo_idx, or NULL.
static ThumbEntry *thumb_find(int photo_idx) {
    for (int i = 0; i < THUMB_CACHE; i++)
        if (g_thumbs[i].photo_idx == photo_idx) return &g_thumbs[i];
    return NULL;
}

// Decode + scale photo_idx into the cache (evicting the LRU entry if needed),
// unless it's already cached. `is_jpeg` selects source (JPEG vs converted PNG).
static ThumbEntry *thumb_load(int photo_idx, int is_jpeg) {
    ThumbEntry *e = thumb_find(photo_idx);
    if (e) { e->stamp = ++g_thumb_clock; return e; }

    // Pick an empty or least-recently-used slot.
    e = &g_thumbs[0];
    for (int i = 0; i < THUMB_CACHE; i++) {
        if (g_thumbs[i].photo_idx < 0) { e = &g_thumbs[i]; break; }
        if (g_thumbs[i].stamp < e->stamp) e = &g_thumbs[i];
    }

    char path[400];
    unsigned char *rgb = NULL; int w = 0, h = 0; int ok;
    if (is_jpeg) {
        make_src_path(g_photos[photo_idx].subdir, g_photos[photo_idx].file,
                      path, sizeof(path));
        ok = (decode_jpeg_preview(path, &rgb, &w, &h) == 0);
    } else {
        make_out_name(g_photos[photo_idx].subdir, g_photos[photo_idx].file,
                      path, sizeof(path));
        ok = (read_png(path, &rgb, &w, &h) == 0);
    }
    e->photo_idx = photo_idx;
    e->stamp = ++g_thumb_clock;
    e->ok = ok;
    if (ok) {
        // Nearest-neighbour scale straight into the stored ABGR1555 thumbnail.
        for (int y = 0; y < THUMB_H; y++) {
            int sy = (int)((long)y * h / THUMB_H);
            for (int x = 0; x < THUMB_W; x++) {
                int sx = (int)((long)x * w / THUMB_W);
                const unsigned char *s = rgb + ((size_t)sy * w + sx) * 3;
                e->px[y * THUMB_W + x] = rgb15(s[0], s[1], s[2]);
            }
        }
        free(rgb);
    }
    return e;
}

// Incrementally warm the thumbnail cache: decode at most `budget` thumbnails
// that aren't cached yet, working outward from `center` within the sliding
// window (THUMB_WINDOW behind/ahead). Returns how many were decoded this call so
// the caller knows whether a repaint is worthwhile. Called once per frame so the
// window fills in the background without ever stalling input.
static int grid_preload_window(int *ids, int n, int center, int is_jpeg, int budget) {
    int done = 0;
    int lo = center - THUMB_WINDOW; if (lo < 0) lo = 0;
    int hi = center + THUMB_WINDOW; if (hi > n - 1) hi = n - 1;
    // Sweep by increasing distance from center so the nearest (most likely to be
    // viewed next) are decoded first.
    for (int d = 0; d <= THUMB_WINDOW && done < budget; d++) {
        int a = center + d, b = center - d;
        if (a <= hi && !thumb_find(ids[a])) { thumb_load(ids[a], is_jpeg); if (++done >= budget) break; }
        if (d != 0 && b >= lo && !thumb_find(ids[b])) { thumb_load(ids[b], is_jpeg); ++done; }
    }
    return done;
}



// Draw the preview box (right side) with whatever is cached, or a placeholder.
#define PREV_X 178
#define PREV_Y 34
#define PREV_W 72
#define PREV_H 54

// ===========================================================================
// Screen rendering. Colors.
// ===========================================================================
#define COL_BG       rgb15(16, 16, 20)
#define COL_TEXT     rgb15(220, 220, 220)
#define COL_DIM      rgb15(130, 130, 140)
#define COL_HILITE   rgb15(40, 70, 110)
#define COL_HEADER   rgb15(130, 220, 200)
#define COL_BTN      rgb15(140, 190, 235)
#define COL_OK       rgb15(130, 210, 130)
#define COL_FAIL     rgb15(230, 120, 120)
#define COL_DIVIDER  rgb15(50, 50, 58)

#define LIST_X    6
#define PAGE_Y    30          // "Page 1/X" line + L/R glyphs (space below header)
#define LIST_Y    43          // first row of the box (clear of the L/R glyphs)
#define ROW_H     11
#define VIS_ROWS  8           // rows per page (fixed box)
#define ROW_W     168         // width of a highlighted row band
#define INFO_Y    (LIST_Y + VIS_ROWS * ROW_H + 3)   // position / total line (below box)

// The repainted band spans the page indicator + L/R glyphs (which sit ~2px
// above the text), the row box, and the info line below it.
#define LIST_BAND_Y  (PAGE_Y - 3)
#define LIST_BAND_H  (INFO_Y + 9 - (PAGE_Y - 3) + 1)

static void draw_header(const char *title, const char *right) {
    fb_text(6, 6, title, COL_HEADER);
    if (right) fb_text(SCR_W - (int)strlen(right) * 6 - 6, 6, right, COL_DIM);
    fb_rect(0, 20, SCR_W, 1, COL_DIVIDER);
}

// Draw a small rectangle button with a single TOP corner notched, holding one
// character. Used for the L / R page buttons: L notches its top-left corner,
// R its top-right, making them directional (back / forward). `corner` is 0 for
// top-left, 1 for top-right. Returns the pixel width.
#define PILL_H   (BTN_D - 2)             // 11, matches the round button's fill
static int fb_corner_btn(int x, int y, char ch, int corner, u16 col, u16 bg) {
    int tw = 6 - 1;                      // one glyph, 5px ink + spacing
    int w = tw + 8;
    int h = PILL_H;
    static const int steps[2] = { 2, 1 };
    for (int yy = 0; yy < h; yy++) {
        int clip = 0;
        if (yy < 2) clip = steps[yy];    // notch the top two rows only
        for (int xx = 0; xx < w; xx++) {
            int cut = (corner == 0) ? (xx < clip) : (xx >= w - clip);
            if (!cut) fb_px(x + xx, y + yy, col);
        }
    }
    // Center the letter ink.
    int oy = (h - GLYPH_H) / 2;
    int ox = (w - tw) / 2;
    const unsigned char *g = font6x8[(int)ch - 32];
    for (int row = 0; row < 8; row++) {
        unsigned char bits = g[row];
        for (int c = 0; c < 6; c++) {
            if (bits & (0x80 >> c)) fb_px(x + ox + c, y + oy + row, bg);
        }
    }
    return w;
}

// Draw a rounded-rectangle button with a short text label inside (for START /
// SELECT). Uses the SAME corner treatment as the round face button: each
// corner has a small notch clipping 2 cells off the first row and 1 off the
// second, so the rectangle matches the button's clipped-square look. Height
// matches the button's visible fill (PILL_H), drawn 1px down to share its rows.
static int fb_pill(int x, int y, const char *label, u16 col, u16 bg) {
    int len = (int)strlen(label);
    int tw = len * 6 - 1;                // 6px per char, minus trailing 1px gap
    int h = PILL_H;
    int w = tw + 8;                      // horizontal padding
    int py = y + 1;                      // align to the circle's fill rows
    // Per-row horizontal clip matching the button corner: 2,1 at top, 1,2 at bottom.
    static const int steps[2] = { 2, 1 };
    for (int yy = 0; yy < h; yy++) {
        int clip = 0;
        for (int i = 0; i < 2; i++) {
            if (yy == i || yy == h - 1 - i) { clip = steps[i]; break; }
        }
        for (int xx = clip; xx < w - clip; xx++) fb_px(x + xx, py + yy, col);
    }
    // Center the label ink (7px tall) within the rectangle.
    int oy = (h - GLYPH_H) / 2;
    int ox = (w - tw) / 2;
    int cx = x + ox;
    for (const char *p = label; *p; p++) {
        const unsigned char *g = font6x8[(int)*p - 32];
        for (int row = 0; row < 8; row++) {
            unsigned char bits = g[row];
            for (int c = 0; c < 6; c++) {
                if (bits & (0x80 >> c)) fb_px(cx + c, py + oy + row, bg);
            }
        }
        cx += 6;
    }
    return w;
}

// Footer with up to a few (button, label) hints, e.g.  (A) Select  (SELECT) Exit.
// `pill` is set for oblong buttons (START/SELECT); otherwise `btn` is a single
// char drawn as a round face button. `arrows` (when set) draws an up/down pair.
typedef struct { const char *pill; char btn; const char *label; int arrows; } Hint;

// Draw a single hint's glyph at (x, by); returns the glyph width. `arrows` 1 =
// up/down pair, 2 = left/right pair; otherwise a pill or round button.
static int draw_hint_glyph(int x, int by, const Hint *h) {
    if (h->arrows == 1) {
        int w = fb_button_arrow(x, by, 0, COL_BTN, COL_BG);
        fb_button_arrow(x + w + 2, by, 1, COL_BTN, COL_BG);
        return w * 2 + 2;
    }
    if (h->arrows == 2) {
        int w = fb_button_arrow(x, by, 2, COL_BTN, COL_BG);
        fb_button_arrow(x + w + 2, by, 3, COL_BTN, COL_BG);
        return w * 2 + 2;
    }
    if (h->pill) return fb_pill(x, by, h->pill, COL_BTN, COL_BG);
    return fb_button(x, by, h->btn, COL_BTN, COL_BG);
}

#define FOOTER_H (BTN_D + 6)             // footer band height
static void draw_footer_hints(const Hint *hints, int count) {
    int fy = SCR_H - FOOTER_H;
    fb_rect(0, fy - 1, SCR_W, 1, COL_DIVIDER);
    fb_rect(0, fy, SCR_W, FOOTER_H, COL_BG);   // clear band
    int x = 8;
    int by = fy + 3;                     // button top
    int ty = by + (BTN_D - GLYPH_H) / 2; // label ink aligned to glyph ink
    for (int i = 0; i < count; i++) {
        x += draw_hint_glyph(x, by, &hints[i]);
        x += 4;
        fb_text(x, ty, hints[i].label, COL_DIM);
        x += (int)strlen(hints[i].label) * 6 + 10;
    }
}

// Draw a single hint's glyph at (x, by); returns the glyph width.
// A two-column, two-row footer: left[0]/left[1] stacked on the left, right[0]/
// right[1] stacked on the right. Row spacing matches the legend's; each label
// sits just past its own glyph. NULL labels are skipped.
#define FOOTER2_H 30
static void draw_footer_2col(const Hint *left, int nleft,
                             const Hint *right, int nright) {
    int fy = SCR_H - FOOTER2_H;
    fb_rect(0, fy - 1, SCR_W, 1, COL_DIVIDER);
    fb_rect(0, fy, SCR_W, FOOTER2_H, COL_BG);
    int rowy[2] = { fy + 2, fy + 16 };       // 14px apart, as in the legend
    for (int i = 0; i < nleft && i < 2; i++) {
        int by = rowy[i];
        int gw = draw_hint_glyph(8, by, &left[i]);
        fb_text(8 + gw + 4, by + (BTN_D - GLYPH_H) / 2, left[i].label, COL_DIM);
    }
    for (int i = 0; i < nright && i < 2; i++) {
        int by = rowy[i];
        int gw = draw_hint_glyph(132, by, &right[i]);
        fb_text(132 + gw + 4, by + (BTN_D - GLYPH_H) / 2, right[i].label, COL_DIM);
    }
}

// Draw the static chrome (full clear + header + optional preview frame).
// Footer is drawn separately by each caller (string or button hints). Called
// once when a screen is entered; per-frame updates avoid touching this.
static void draw_chrome(const char *title, const char *right, int show_preview) {
    fb_rect(0, 0, SCR_W, SCR_H, COL_BG);
    draw_header(title, right);
    if (show_preview) {
        u16 border = rgb15(90, 90, 100);
        fb_rect(PREV_X - 1, PREV_Y - 1, PREV_W + 2, 1, border);
        fb_rect(PREV_X - 1, PREV_Y + PREV_H, PREV_W + 2, 1, border);
        fb_rect(PREV_X - 1, PREV_Y - 1, 1, PREV_H + 2, border);
        fb_rect(PREV_X + PREV_W, PREV_Y - 1, 1, PREV_H + 2, border);
        fb_text(PREV_X, PREV_Y + PREV_H + 4, "preview", COL_DIM);
    }
}

// Repaint only the preview image area (inside the frame). Used when the
// selection settles on a new photo, so the rest of the screen never flashes.
static void redraw_preview(void) {
    if (g_prev_rgb) {
        fb_blit_rgb(g_prev_rgb, g_prev_w, g_prev_h, PREV_X, PREV_Y, PREV_W, PREV_H);
    } else {
        fb_rect(PREV_X, PREV_Y, PREV_W, PREV_H, rgb15(24, 24, 28));
        fb_text(PREV_X + 8, PREV_Y + PREV_H / 2 - 4, "...", rgb15(140,140,140));
    }
}

// List display modes.
typedef enum { MODE_ADD, MODE_LIBRARY } ListMode;

// Repaint only the list band: a "page/total" indicator above a fixed box of
// rows, then a position/total line below. Header, footer, and preview frame
// are untouched, eliminating flicker.
//   ids[]: indices into g_photos. n: count. sel: 0..n-1. top: first row shown.
static void redraw_rows(int *ids, int n, int sel, int top, ListMode mode,
                        const char *empty_msg) {
    fb_rect(0, LIST_BAND_Y, ROW_W + 4, LIST_BAND_H, COL_BG);  // clear band only

    if (n == 0) {
        if (empty_msg) fb_text(LIST_X, LIST_Y, empty_msg, COL_DIM);
        return;
    }

    int pages = (n + VIS_ROWS - 1) / VIS_ROWS;
    int page  = top / VIS_ROWS;

    // Page indicator above the box: "Page 1/12"
    char pg[32];
    snprintf(pg, sizeof(pg), "Page %d/%d", page + 1, pages);
    fb_text(LIST_X, PAGE_Y, pg, COL_DIM);

    // L / R page buttons on the right of the indicator line. L notches its
    // top-left corner, R its top-right, so the pair reads as back / forward.
    // Drawn 2px up so the 11px glyph is roughly centered on the text baseline.
    int gy = PAGE_Y - 2;
    int rx = LIST_X + ROW_W - 4;          // right edge of the list box
    int rw = 6 - 1 + 8;                   // width fb_corner_btn returns
    fb_corner_btn(rx - rw, gy, 'R', 1, COL_BTN, COL_BG);
    fb_corner_btn(rx - rw - rw - 3, gy, 'L', 0, COL_BTN, COL_BG);

    // The fixed box of rows.
    for (int r = 0; r < VIS_ROWS; r++) {
        int idx = top + r;
        if (idx >= n) break;
        int y = LIST_Y + r * ROW_H;
        int is_sel = (idx == sel);
        if (is_sel) fb_rect(LIST_X - 2, y - 1, ROW_W, ROW_H, COL_HILITE);
        PhotoEntry *p = &g_photos[ids[idx]];
        char line[44];
        // Add shows the source JPEG name; Library represents the converted
        // file, so display it with a .png extension.
        char name[40];
        snprintf(name, sizeof(name), "%s", p->file);
        if (mode != MODE_ADD) {
            char *dot = strrchr(name, '.');
            if (dot && (int)(dot - name) < (int)sizeof(name) - 5)
                snprintf(dot, sizeof(name) - (dot - name), ".png");
        }
        if (mode == MODE_LIBRARY)
            snprintf(line, sizeof(line), "%s", name);
        else
            snprintf(line, sizeof(line), "[%c] %s", p->convert ? 'x' : ' ', name);
        fb_text_clip(LIST_X, y, line, is_sel ? COL_TEXT : COL_DIM, 26);
    }

    // Below the box: "<pos> of <total>" on the left, the cap on the right.
    char posbuf[32];
    snprintf(posbuf, sizeof(posbuf), "%d of %d", sel + 1, n);
    fb_text(LIST_X, INFO_Y, posbuf, COL_DIM);

    char maxbuf[24];
    snprintf(maxbuf, sizeof(maxbuf), "Max: %d", MAX_PHOTOS);
    int tw = (int)strlen(maxbuf) * 6;
    fb_text(LIST_X + ROW_W - 4 - tw, INFO_Y, maxbuf, g_hit_max ? COL_FAIL : COL_DIM);
}

// --- Gallery (grid) rendering -------------------------------------------------

// Pixel position of grid cell `slot` (0..GRID_CELLS-1): thumbnail top-left.
static void grid_cell_xy(int slot, int *tx, int *ty) {
    int r = slot / GRID_COLS, c = slot % GRID_COLS;
    int cell_x = GRID_MARGIN_X + c * GRID_CELL_W;
    *tx = cell_x + (GRID_CELL_W - THUMB_W) / 2;
    *ty = GRID_TOP + r * (GRID_CELL_H + CELL_GAP_Y);
}

// Draw one grid cell: thumbnail (or placeholder), filename beneath, and a blue
// highlight box (covering the thumbnail AND the label/checkbox) when selected.
static void grid_draw_cell(int slot, int photo_idx, int selected, ListMode mode) {
    int tx, ty; grid_cell_xy(slot, &tx, &ty);

    int ly = ty + THUMB_H + LABEL_GAP;       // label baseline
    // Highlight (or clear) the whole cell: thumbnail area plus the label line.
    u16 bandcol = selected ? COL_HILITE : COL_BG;
    fb_rect(tx - 2, ty - 2, THUMB_W + 4, (ly + 9) - (ty - 2), bandcol);

    // Blit the pre-scaled thumbnail straight from the cache (no rescaling).
    ThumbEntry *te = thumb_find(photo_idx);
    if (te && te->ok) {
        for (int yy = 0; yy < THUMB_H; yy++)
            for (int xx = 0; xx < THUMB_W; xx++)
                fb_px(tx + xx, ty + yy, te->px[yy * THUMB_W + xx]);
    } else {
        fb_rect(tx, ty, THUMB_W, THUMB_H, rgb15(24, 24, 28));
        fb_text(tx + THUMB_W / 2 - 9, ty + THUMB_H / 2 - 4, "...", COL_DIM);
    }

    // Filename (extension dropped, tail kept if long). On the Add screen it is
    // prefixed with a [x]/[ ] checkbox like the list view. The whole label is
    // centered under the thumbnail.
    char name[40];
    snprintf(name, sizeof(name), "%s", g_photos[photo_idx].file);
    char *dot = strrchr(name, '.');
    if (dot) *dot = '\0';
    int maxchars = (THUMB_W + 4) / 6;            // label width ~ thumbnail width
    int prefix = (mode == MODE_LIBRARY) ? 0 : 4; // "[x] " on Add
    int avail = maxchars - prefix;
    if (avail < 1) avail = 1;
    const char *show = name;
    int len = (int)strlen(name);
    if (len > avail) show = name + (len - avail);  // tail end (keeps the number)
    char label[48];
    if (mode == MODE_LIBRARY)
        snprintf(label, sizeof(label), "%s", show);
    else
        snprintf(label, sizeof(label), "[%c] %s",
                 g_photos[photo_idx].convert ? 'x' : ' ', show);
    // Center the label under the thumbnail.
    int lw = (int)strlen(label) * 6;
    int lx = tx + (THUMB_W - lw) / 2;
    if (lx < tx - 2) lx = tx - 2;
    fb_text(lx, ly, label, selected ? COL_TEXT : COL_DIM);
}

// Repaint the whole gallery band: page indicator + L/R glyphs, then the cells.
// `top` is the first photo index on this page (a multiple of GRID_CELLS).
// The gallery's bottom info line: "<pos> of <total>" left, "Max: N" right.
// Drawn on its own so a selection move can refresh just the position counter
// without repainting the whole grid (avoids flicker).
static void redraw_grid_info(int n, int sel) {
    fb_rect(0, GRID_INFO_Y, SCR_W, 8, COL_BG);
    char posbuf[32];
    snprintf(posbuf, sizeof(posbuf), "%d of %d", sel + 1, n);
    fb_text(GRID_MARGIN_X, GRID_INFO_Y, posbuf, COL_DIM);
    char maxbuf[24];
    snprintf(maxbuf, sizeof(maxbuf), "Max: %d", MAX_PHOTOS);
    int tw = (int)strlen(maxbuf) * 6;
    fb_text(SCR_W - GRID_MARGIN_X - tw, GRID_INFO_Y, maxbuf,
            g_hit_max ? COL_FAIL : COL_DIM);
}

static void redraw_grid(int *ids, int n, int sel, int top, ListMode mode,
                        const char *empty_msg) {
    // Clear only the page-indicator strip at the top; each cell clears its own
    // area as it draws (see grid_draw_cell), so we avoid a big full-band clear
    // followed by a redraw -- that gap is visible as a flicker when the display
    // scans out mid-draw.
    fb_rect(0, PAGE_Y - 3, SCR_W, (GRID_TOP - 2) - (PAGE_Y - 3), COL_BG);

    if (n == 0) {
        fb_rect(0, GRID_TOP - 2, SCR_W, SCR_H - (GRID_TOP - 2), COL_BG);
        if (empty_msg) fb_text(GRID_MARGIN_X, GRID_TOP, empty_msg, COL_DIM);
        return;
    }

    int pages = (n + GRID_CELLS - 1) / GRID_CELLS;
    int page  = top / GRID_CELLS;

    char pg[32];
    snprintf(pg, sizeof(pg), "Page %d/%d", page + 1, pages);
    fb_text(GRID_MARGIN_X, PAGE_Y, pg, COL_DIM);

    int gy = PAGE_Y - 2;
    int rx = SCR_W - GRID_MARGIN_X;
    int rw = 6 - 1 + 8;
    fb_corner_btn(rx - rw, gy, 'R', 1, COL_BTN, COL_BG);
    fb_corner_btn(rx - rw - rw - 3, gy, 'L', 0, COL_BTN, COL_BG);

    // Draw each cell. Cells past the end of the list are blanked so a shorter
    // final page doesn't leave the previous page's thumbnails behind.
    for (int s = 0; s < GRID_CELLS; s++) {
        int idx = top + s;
        if (idx < n) {
            grid_draw_cell(s, ids[idx], idx == sel, mode);
        } else {
            int tx, ty; grid_cell_xy(s, &tx, &ty);
            fb_rect(tx - 2, ty - 2, THUMB_W + 4, (THUMB_H + LABEL_GAP + 9) + 2, COL_BG);
        }
    }

    redraw_grid_info(n, sel);
}

// Per-frame gallery update shared by Add and Library. Handles the three cases
// (page change, selection move/toggle, idle) and warms the sliding-window cache
// in the background, repainting any visible cell whose thumbnail just arrived.
// `page_changed`/`redraw_cells` tell it what input did this frame.
static void grid_frame_update(int *ids, int n, int sel, int prev_sel, int top,
                              int prev_top, ListMode mode, const char *empty,
                              int is_jpeg, int page_changed, int redraw_cells) {
    // Tracks, per visible slot, the photo we last drew *with its thumbnail* (not
    // a placeholder). Lets us repaint a cell exactly once when its thumbnail
    // becomes available -- whoever decoded it -- instead of waiting for a hover.
    static int shown[GRID_CELLS];
    static int shown_valid = 0;
    if (!shown_valid) { for (int i = 0; i < GRID_CELLS; i++) shown[i] = -1; shown_valid = 1; }

    if (page_changed) {
        redraw_grid(ids, n, sel, top, mode, empty);
        for (int s = 0; s < GRID_CELLS; s++) {           // record what's now shown
            int idx = top + s;
            ThumbEntry *te = (idx < n) ? thumb_find(ids[idx]) : NULL;
            shown[s] = (te && te->ok) ? ids[idx] : -1;   // -1 == placeholder/empty
        }
    } else if (redraw_cells) {
        if (sel != prev_sel) {
            if (prev_sel >= top && prev_sel < top + GRID_CELLS) {
                grid_draw_cell(prev_sel - top, ids[prev_sel], 0, mode);
                shown[prev_sel - top] = -2;   // force re-check below
            }
            grid_draw_cell(sel - top, ids[sel], 1, mode);
            shown[sel - top] = -2;
        } else {
            grid_draw_cell(sel - top, ids[sel], 1, mode);   // toggle redraw
            shown[sel - top] = -2;
        }
        redraw_grid_info(n, sel);
    }
    (void)prev_top;

    if (n <= 0) return;

    // Decoding a source JPEG (Add view) is far more expensive than a converted
    // PNG (Library). Doing several in one frame stalls visibly, so for JPEGs we
    // decode just ONE not-yet-cached cell per frame -- placeholders show until
    // each arrives, but input never freezes. PNGs are cheap, so a few per frame
    // is fine and fills the page almost instantly.
    int vis_budget = is_jpeg ? 1 : 3;
    int decoded_visible = 0;
    for (int s = 0; s < GRID_CELLS && top + s < n && vis_budget > 0; s++) {
        ThumbEntry *te = thumb_find(ids[top + s]);
        if (te && te->ok) continue;             // already decoded
        thumb_load(ids[top + s], is_jpeg);
        vis_budget--;
        decoded_visible++;
    }

    // Trickle the sliding window so paging is instant later. Skip it on any frame
    // we already spent a JPEG decode on the visible page, so we never do two
    // expensive decodes in one frame.
    int win_budget = is_jpeg ? (decoded_visible ? 0 : 1) : 2;
    if (win_budget > 0) grid_preload_window(ids, n, sel, is_jpeg, win_budget);

    // Finally, repaint any VISIBLE cell whose thumbnail is now ready but is still
    // showing a placeholder (or was just decoded by the window preloader). This
    // is the key fix: a cell decoded in the background gets drawn without needing
    // the user to hover over it.
    for (int s = 0; s < GRID_CELLS && top + s < n; s++) {
        int idx = top + s;
        ThumbEntry *te = thumb_find(ids[idx]);
        int want = (te && te->ok) ? ids[idx] : -1;
        if (want != -1 && shown[s] != want) {
            grid_draw_cell(s, ids[idx], idx == sel, mode);
            shown[s] = ids[idx];
        }
    }
}

static const char *err_text(int code) {
    switch (code) {
        case ERR_NOMEM_RGB:    return "out of memory";
        case ERR_SHORT_DECODE: return "truncated JPEG";
        case ERR_FILE_OPEN:    return "cannot open file";
        case ERR_FILE_READ:    return "file read error";
        case ERR_PNG_WRITE:    return "cannot write PNG";
        case PJPG_NOTENOUGHMEM:return "decoder low memory";
        case PJPG_UNSUPPORTED_MODE: return "progressive JPEG";
        case PJPG_NOT_JPEG:    return "not a JPEG";
        default:               return "decode error";
    }
}

// ---- Conversion screen with a graphical progress bar.
static void run_conversion(int *ids, int n) {
    draw_chrome("Syncing", NULL, 0);
    fb_text(6, SCR_H - 10, "Working...", COL_DIM);

    int total = 0;
    for (int i = 0; i < n; i++) if (g_photos[ids[i]].convert) total++;

    int done = 0, ok = 0, fail = 0, faily = 70;
    int barx = 10, bary = 34, barw = SCR_W - 20, barh = 12;
    fb_rect(barx, bary, barw, barh, rgb15(40,40,48));

    for (int i = 0; i < n; i++) {
        PhotoEntry *p = &g_photos[ids[i]];
        if (!p->convert) continue;
        int r = convert_entry(p);
        if (r == 1) { ok++; p->done = 1; p->convert = 0; }
        else if (r == 0) {
            fail++;
            if (faily < SCR_H - 24) {
                char msg[44];
                snprintf(msg, sizeof(msg), "x %s: %s", p->file, err_text(g_last_err));
                fb_text_clip(8, faily, msg, COL_FAIL, 40);
                faily += 10;
            }
        }
        done++;
        int fillw = total ? (done * barw / total) : barw;
        fb_rect(barx, bary, fillw, barh, rgb15(60,150,90));
        char cnt[28];
        snprintf(cnt, sizeof(cnt), "%d / %d", done, total);
        fb_rect(barx, bary + barh + 4, 120, 9, COL_BG);
        fb_text(barx, bary + barh + 4, cnt, COL_TEXT);
        swiWaitForVBlank();
    }

    int sy = SCR_H - FOOTER_H - 14;
    char part[24];
    snprintf(part, sizeof(part), "Completed: %d", ok);
    fb_text(8, sy, part, COL_OK);
    int cx = 8 + (int)strlen(part) * 6 + 12;     // gap after the first part
    snprintf(part, sizeof(part), "Failed: %d", fail);
    fb_text(cx, sy, part, fail ? COL_FAIL : COL_DIM);
}

// ---- Removal action: delete checked converted PNGs from the slideshow folder.
// Wait for any key in mask; returns the pressed keys.
static int wait_keys(int mask) {
    while (1) {
        swiWaitForVBlank(); scanKeys();
        int k = keysDown();
        if (k & mask) return k;
    }
}

// Simple full-screen message (SD failure / no photos / etc).
static void message_screen(const char *l1, const char *l2, const char *l3) {
    draw_chrome("DSiPhotoSync", NULL, 0);
    Hint h[1] = { { "START", 0, "Exit", 0 } };
    draw_footer_hints(h, 1);
    if (l1) fb_text(8, 40, l1, COL_TEXT);
    if (l2) fb_text(8, 54, l2, COL_DIM);
    if (l3) fb_text(8, 68, l3, COL_DIM);
}

// ---- Root menu band repaint.
#define MENU_ITEMS 4

static void redraw_menu(int sel, int n_unconv, int n_conv) {
    fb_rect(0, LIST_BAND_Y, SCR_W, LIST_BAND_H, COL_BG);   // clear menu band
    char l0[32], l1[32];
    snprintf(l0, sizeof(l0), "Add photos (%d new)", n_unconv);
    snprintf(l1, sizeof(l1), "View gallery (%d)", n_conv);
    const char *labels[MENU_ITEMS] = { l0, l1, "Use with custom theme (beta)", "Settings" };
    for (int i = 0; i < MENU_ITEMS; i++) {
        int y = LIST_Y + i * (ROW_H + 6);
        if (i == sel) fb_rect(LIST_X - 2, y - 1, ROW_W, ROW_H, COL_HILITE);
        fb_text(LIST_X, y, labels[i], i == sel ? COL_TEXT : COL_DIM);
    }
}


// Persistent settings (full load/save defined later, near main()).
#define SETTINGS_PATH "sd:/_nds/TWiLightMenu/dsimenu/dsiphotosync.ini"
typedef struct {
    int default_grid;  // 1 = gallery (grid) is the default view, 0 = list
    int quick_mode;    // 1 = on boot, auto-sync all new photos then exit
} Settings;
static Settings g_settings = { 0, 0 };   // defaults: list view, quick mode off

// ---- Settings screen band repaint. `changed` marks options edited since the
// screen was opened (an asterisk), so it's clear what will be saved.
#define SETTINGS_ITEMS 2
static void redraw_settings(int sel, int default_grid_changed, int quick_changed) {
    fb_rect(0, LIST_BAND_Y, SCR_W, LIST_BAND_H, COL_BG);
    char l0[44], l1[44];
    snprintf(l0, sizeof(l0), "Default view:  < %s >%s",
             g_settings.default_grid ? "Grid" : "List",
             default_grid_changed ? "  *" : "");
    snprintf(l1, sizeof(l1), "Quick mode:  < %s >%s",
             g_settings.quick_mode ? "On" : "Off",
             quick_changed ? "  *" : "");
    const char *labels[SETTINGS_ITEMS] = { l0, l1 };
    for (int i = 0; i < SETTINGS_ITEMS; i++) {
        int y = LIST_Y + i * (ROW_H + 6);
        if (i == sel) fb_rect(LIST_X - 2, y - 1, ROW_W, ROW_H, COL_HILITE);
        fb_text(LIST_X, y, labels[i], i == sel ? COL_TEXT : COL_DIM);
    }
    // Explain only Quick mode (the less obvious option) when it's highlighted.
    if (sel == 1)
        fb_text(LIST_X, LIST_Y + SETTINGS_ITEMS * (ROW_H + 6) + 6,
                "On: sync new photos at boot, then exit.", COL_DIM);
}

// ===========================================================================
// Persistent settings (stored on SD so they survive restarts).
// ===========================================================================
// Read settings from SD. Missing file or keys leave defaults in place.
static void settings_load(void) {
    FILE *f = fopen(SETTINGS_PATH, "r");
    if (!f) return;
    char line[64];
    while (fgets(line, sizeof(line), f)) {
        int v;
        if (sscanf(line, "default_grid=%d", &v) == 1) g_settings.default_grid = v ? 1 : 0;
        else if (sscanf(line, "quick_mode=%d", &v) == 1) g_settings.quick_mode = v ? 1 : 0;
    }
    fclose(f);
}

static void settings_save(void) {
    FILE *f = fopen(SETTINGS_PATH, "w");
    if (!f) return;
    fprintf(f, "default_grid=%d\n", g_settings.default_grid);
    fprintf(f, "quick_mode=%d\n", g_settings.quick_mode);
    fclose(f);
}

// ===========================================================================
// Main: framebuffer video on the bottom screen + UI state machine.
// ===========================================================================
typedef enum { SCREEN_MENU, SCREEN_ADD, SCREEN_LIBRARY,
               SCREEN_EXPAND, SCREEN_SETTINGS, SCREEN_CONVERT,
               SCREEN_CONFIRM_DELETE, SCREEN_CONFIRM_REMOVE_ALL,
               SCREEN_THEMES,            // split compatible/incompatible list
               SCREEN_THEME_EXPLAIN,     // theme builder intro (screen 1/2)
               SCREEN_THEME_ADJUST,      // pick adjustments (screen 2/2)
               SCREEN_THEME_APPLY,       // pick which variant to apply
               SCREEN_THEME_SETPROMPT,   // after make: set as selected theme?
               SCREEN_EXIT } Screen;

static int g_unconv[MAX_PHOTOS], g_n_unconv = 0;
static int g_conv[MAX_PHOTOS],   g_n_conv   = 0;

static void repartition(void) {
    g_n_unconv = 0; g_n_conv = 0;
    for (int i = 0; i < g_photo_count; i++) {
        if (g_photos[i].done) g_conv[g_n_conv++] = i;
        else                  g_unconv[g_n_unconv++] = i;
    }
}

// ===========================================================================
// "Use with custom theme": make a photo-enabled copy of a TWiLight theme.
//
// TWiLight only renders the top-screen photo when a theme's settings file has
// "Render Photo = 1", and when on it draws "top_photo.png" instead of "top.png".
// So for a chosen theme we duplicate its folder as "<name> - with photo", flip
// that line on (adding it if absent), and produce a top_photo.png (optionally
// with a frame composited over it -- added in a later step).
// ===========================================================================
#define MAX_THEMES   128
#define THEME_NAMELEN 128
#define MAX_VARIANTS  8        // distinct adjustment-combos per base theme
#define VARSUF_LEN    16       // e.g. " - pfsLR"

// Adjustment letters, applied in this fixed order to build a suffix:
//   p = photo (always), f = frame, s = shadow, then "LR" = shoulder buttons.
// Suffix examples: " - p", " - pf", " - pfs", " - pfsLR", " - psLR".

// One base theme plus the list of photo-enabled variants we've generated for it.
typedef struct {
    char name[THEME_NAMELEN];          // base theme folder name (no suffix)
    int  compatible;                   // 1 if it has variants or RenderPhoto=1
    int  nvariants;                    // how many variant suffixes exist
    char variant[MAX_VARIANTS][VARSUF_LEN];  // e.g. " - pf" (includes the " - ")
} ThemeEntry;
static ThemeEntry g_themes[MAX_THEMES];
static int g_n_themes = 0;

// Does `name` end with a generated-variant suffix of the form " - p[f][s][LR]"?
// If so, returns a pointer to the " - " within `name` (start of the suffix);
// else NULL. The suffix must start with "p" (photo) right after " - ".
static const char *variant_suffix(const char *name) {
    const char *dash = NULL, *p = name;
    // Find the LAST " - " in the name (variant separator).
    while ((p = strstr(p, " - ")) != NULL) { dash = p; p += 3; }
    if (!dash) return NULL;
    const char *s = dash + 3;
    if (*s != 'p') return NULL;        // every variant starts with photo
    s++;
    // Remaining must be only from {f, s, L, R}.
    while (*s) {
        if (*s != 'f' && *s != 's' && *s != 'L' && *s != 'R') return NULL;
        s++;
    }
    return dash;
}

// Forward declarations (defined later, near the ini helpers).
static int twl_get_theme(char *out, size_t outsz);

// How many generated variants a base theme has.
static int theme_variant_count(const ThemeEntry *t) {
    return t->nvariants;
}

// Find the base-theme index whose name matches the current DSI_THEME value
// (either the base name itself, or any "<base> - variant" form). Returns -1 if
// none match / no current theme.
static int theme_applied_base(void) {
    char cur[THEME_NAMELEN];
    if (twl_get_theme(cur, sizeof(cur)) != 0) return -1;
    for (int i = 0; i < g_n_themes; i++) {
        const char *bn = g_themes[i].name;
        size_t bl = strlen(bn);
        if (strcmp(cur, bn) == 0 ||
            (strncmp(cur, bn, bl) == 0 && cur[bl] == ' '))
            return i;
    }
    return -1;
}

// Virtual row marker for the pinned "Default theme" entry on the compatible page.
#define THEME_DEFAULT_ROW (-2)

// Build the ordered list of theme indices for a page (page 0 = compatible,
// page 1 = incompatible). Compatible themes are sorted alphabetically (case-
// insensitive) and the virtual "Default theme" row is pinned at the very top.
// Returns the count; fills `out` (which may contain THEME_DEFAULT_ROW).
static int build_page_list(int page, int *out) {
    int n = 0;
    if (page == 0) out[n++] = THEME_DEFAULT_ROW;     // pinned Default at top
    int start = n;
    for (int i = 0; i < g_n_themes; i++) {
        // The stock default theme folder is represented by the pinned Default row
        // only -- never listed as a regular theme on either page.
        if (strcasecmp(g_themes[i].name, DEFAULT_THEME_NAME) == 0) continue;
        if ((page == 0) == (g_themes[i].compatible != 0)) out[n++] = i;
    }
    // Alphabetical (case-insensitive) sort of the real entries.
    for (int a = start; a < n; a++)
        for (int b = a + 1; b < n; b++)
            if (strcasecmp(g_themes[out[a]].name, g_themes[out[b]].name) > 0) {
                int t = out[a]; out[a] = out[b]; out[b] = t;
            }
    return n;
}

// Find or add a base-name entry; returns its index (or -1 if full).
static int theme_find_or_add(const char *base) {
    for (int i = 0; i < g_n_themes; i++)
        if (strcmp(g_themes[i].name, base) == 0) return i;

    if (g_n_themes >= MAX_THEMES) return -1;
    int idx = g_n_themes++;
    snprintf(g_themes[idx].name, THEME_NAMELEN, "%s", base);
    g_themes[idx].compatible = 0;
    g_themes[idx].nvariants = 0;
    return idx;
}

// Record that base index `idx` has a variant with suffix `suf` (e.g. " - pf").
static void theme_add_variant(int idx, const char *suf) {
    if (idx < 0 || g_themes[idx].nvariants >= MAX_VARIANTS) return;
    for (int i = 0; i < g_themes[idx].nvariants; i++)
        if (strcmp(g_themes[idx].variant[i], suf) == 0) return;   // dup
    snprintf(g_themes[idx].variant[g_themes[idx].nvariants], VARSUF_LEN, "%s", suf);
    g_themes[idx].nvariants++;
}

// Build a human-readable label for a variant suffix (" - pfsLR" -> "Photo, Frame,
// Shadow, L/R"). Photo is always present.
static void variant_label(const char *suf, char *out, size_t outsz) {
    // suf looks like " - p[f][s][LR]"; skip " - ".
    const char *s = suf;
    const char *dash = strstr(s, " - ");
    s = dash ? dash + 3 : s;
    char buf[64]; int n = 0;
    n += snprintf(buf + n, sizeof(buf) - n, "Photo");
    for (const char *c = s; *c; c++) {
        if (*c == 'f')      n += snprintf(buf + n, sizeof(buf) - n, ", Frame");
        else if (*c == 's') n += snprintf(buf + n, sizeof(buf) - n, ", Shadow");
        else if (*c == 'L') n += snprintf(buf + n, sizeof(buf) - n, ", L/R");
        // 'R' is part of "LR"; skip it
    }
    snprintf(out, outsz, "%s", buf);
}

// Forward declaration (defined later, near the ini helpers).
static int theme_render_photo(const char *theme_dir);

// Scan THEMES_DIR and build the grouped model: each base theme appears once,
// with flags for which generated variants exist. A base is "compatible" if its
// own theme.ini has RenderPhoto = 1 (or it has generated variants).
static void build_theme_list(void) {
    g_n_themes = 0;
    DIR *d = opendir(THEMES_DIR);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[400];
        snprintf(path, sizeof(path), "%s/%s", THEMES_DIR, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        const char *suf = variant_suffix(e->d_name);
        if (suf) {
            // A generated variant: attribute it to its base name (the part before
            // the " - <adjustments>" suffix).
            char base[THEME_NAMELEN];
            int blen = (int)(suf - e->d_name);
            if (blen <= 0 || blen >= THEME_NAMELEN) continue;
            memcpy(base, e->d_name, blen); base[blen] = '\0';
            int idx = theme_find_or_add(base);
            if (idx < 0) continue;
            g_themes[idx].compatible = 1;
            theme_add_variant(idx, suf);
        } else {
            // A plain theme folder. It's "compatible" if its own theme.ini has
            // RenderPhoto = 1.
            int idx = theme_find_or_add(e->d_name);
            if (idx >= 0 && theme_render_photo(path) == 1)
                g_themes[idx].compatible = 1;
        }
    }
    closedir(d);
}


// Copy a single file byte-for-byte. Returns 0 on success.
static int copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    static unsigned char cbuf[8192];
    size_t n;
    int rc = 0;
    while ((n = fread(cbuf, 1, sizeof(cbuf), in)) > 0) {
        if (fwrite(cbuf, 1, n, out) != n) { rc = -1; break; }
    }
    fclose(in);
    fclose(out);
    return rc;
}

// Recursively copy a directory tree (files + subfolders at any depth).
// Returns 0 on success, -1 on any failure.
static int copy_dir_recursive(const char *src, const char *dst) {
    if (mkdir(dst, 0777) != 0) {
        struct stat st;
        if (stat(dst, &st) != 0 || !S_ISDIR(st.st_mode)) return -1; // exists as file
    }
    DIR *d = opendir(src);
    if (!d) return -1;
    struct dirent *e;
    int rc = 0;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char sp[512], dp[512];
        snprintf(sp, sizeof(sp), "%s/%s", src, e->d_name);
        snprintf(dp, sizeof(dp), "%s/%s", dst, e->d_name);
        struct stat st;
        if (stat(sp, &st) != 0) { rc = -1; break; }
        if (S_ISDIR(st.st_mode)) {
            if (copy_dir_recursive(sp, dp) != 0) { rc = -1; break; }
        } else {
            if (copy_file(sp, dp) != 0) { rc = -1; break; }
        }
    }
    closedir(d);
    return rc;
}

// Recursively search `dir` for a file named `target`. On success, writes its
// full path into `out` and returns 0; returns -1 if not found.
static int find_file(const char *dir, const char *target, char *out, size_t outsz) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e;
    int found = -1;
    while ((e = readdir(d)) && found != 0) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char p[600];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (find_file(p, target, out, outsz) == 0) found = 0;
        } else if (strcmp(e->d_name, target) == 0) {
            snprintf(out, outsz, "%s", p);
            found = 0;
        }
    }
    closedir(d);
    return found;
}

// Recursively search `dir` for a subdirectory named `target`. On success, writes
// its full path into `out` and returns 0; returns -1 if not found.
static int find_dir(const char *dir, const char *target, char *out, size_t outsz) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e;
    int found = -1;
    while ((e = readdir(d)) && found != 0) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char p[600];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        if (strcmp(e->d_name, target) == 0) {
            snprintf(out, outsz, "%s", p);
            found = 0;
        } else if (find_dir(p, target, out, outsz) == 0) {
            found = 0;
        }
    }
    closedir(d);
    return found;
}
// theme's settings ini. TWiLight's theme settings file is "theme.ini". We flip
// an existing line (any spacing/case around the value) or append it if missing.
// The rest of the file is preserved verbatim. Returns 0 on success.
static int theme_enable_photo(const char *theme_dir) {
    char ini[512];
    snprintf(ini, sizeof(ini), "%s/theme.ini", theme_dir);

    FILE *f = fopen(ini, "rb");
    long sz = 0;
    char *buf = NULL;
    if (f) {
        fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
        if (sz > 0 && sz < 256 * 1024) {
            buf = (char *)malloc(sz + 1);
            if (buf) {
                if (fread(buf, 1, sz, f) != (size_t)sz) { free(buf); buf = NULL; }
                else buf[sz] = '\0';
            }
        }
        fclose(f);
    }

    FILE *out = fopen(ini, "wb");
    if (!out) { free(buf); return -1; }

    int wrote_line = 0;
    if (buf) {
        // Walk line by line; rewrite the Render Photo line, keep everything else.
        char *p = buf;
        while (*p) {
            char *nl = strchr(p, '\n');
            int linelen = nl ? (int)(nl - p) : (int)strlen(p);
            // Detect a "Render Photo" key (case-insensitive, ignore spaces).
            int is_render = 0;
            {
                const char *key = "renderphoto";
                int ki = 0; int i = 0;
                for (; i < linelen && key[ki]; i++) {
                    char c = p[i];
                    if (c == ' ' || c == '\t') continue;
                    char lc = (c >= 'A' && c <= 'Z') ? c + 32 : c;
                    if (lc != key[ki]) break;
                    ki++;
                }
                if (key[ki] == '\0') is_render = 1;
            }
            if (is_render) {
                fputs("RenderPhoto = 1", out);
                if (nl) fputc('\n', out);
                wrote_line = 1;
            } else {
                fwrite(p, 1, linelen, out);
                if (nl) fputc('\n', out);
            }
            if (!nl) break;
            p = nl + 1;
        }
    }
    if (!wrote_line) {
        // No existing line (or no file): append it. Ensure it starts on its own line.
        if (sz > 0 && buf && buf[sz - 1] != '\n') fputc('\n', out);
        fputs("RenderPhoto = 1\n", out);
    }
    fclose(out);
    free(buf);
    return 0;
}

// Read the current DSI_THEME value from TWiLight's settings.ini into `out`.
// Returns 0 on success, -1 if the file or key isn't found.
static int twl_get_theme(char *out, size_t outsz) {
    FILE *f = fopen(TWL_SETTINGS_PATH, "rb");
    if (!f) return -1;
    char line[256];
    int rc = -1;
    while (fgets(line, sizeof(line), f)) {
        // Match "DSI_THEME" (case-insensitive, ignore leading spaces).
        const char *key = "dsi_theme";
        int ki = 0, i = 0;
        while (line[i] == ' ' || line[i] == '\t') i++;
        while (line[i] && key[ki]) {
            char c = line[i];
            char lc = (c >= 'A' && c <= 'Z') ? c + 32 : c;
            if (lc != key[ki]) break;
            i++; ki++;
        }
        if (key[ki] != '\0') continue;            // not this key
        // skip spaces and '='
        while (line[i] == ' ' || line[i] == '\t') i++;
        if (line[i] == '=') i++;
        while (line[i] == ' ' || line[i] == '\t') i++;
        // copy the rest, trimming trailing newline/space
        int n = (int)strlen(line);
        while (n > i && (line[n-1] == '\n' || line[n-1] == '\r' ||
                         line[n-1] == ' '  || line[n-1] == '\t')) n--;
        int len = n - i; if (len < 0) len = 0;
        if ((size_t)len >= outsz) len = outsz - 1;
        memcpy(out, line + i, len); out[len] = '\0';
        rc = 0;
        break;
    }
    fclose(f);
    return rc;
}

// Set DSI_THEME in TWiLight's settings.ini to `theme_name` (the folder name,
// unquoted, spaces allowed -- matching TWiLight's own format). Rewrites just that
// line, preserving the rest of the file verbatim. Returns 0 on success.
static int twl_set_theme(const char *theme_name) {
    FILE *f = fopen(TWL_SETTINGS_PATH, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 1024 * 1024) { fclose(f); return -1; }
    char *buf = (char *)malloc(sz + 1);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, sz, f) != (size_t)sz) { fclose(f); free(buf); return -1; }
    buf[sz] = '\0';
    fclose(f);

    FILE *out = fopen(TWL_SETTINGS_PATH, "wb");
    if (!out) { free(buf); return -1; }
    int wrote = 0;
    char *p = buf;
    while (*p) {
        char *nl = strchr(p, '\n');
        int linelen = nl ? (int)(nl - p) : (int)strlen(p);
        // Is this the DSI_THEME line? (case-insensitive, ignore leading spaces)
        int is_key = 0;
        {
            const char *key = "dsi_theme";
            int ki = 0, i = 0;
            while (i < linelen && (p[i] == ' ' || p[i] == '\t')) i++;
            while (i < linelen && key[ki]) {
                char c = p[i];
                char lc = (c >= 'A' && c <= 'Z') ? c + 32 : c;
                if (lc != key[ki]) break;
                i++; ki++;
            }
            if (key[ki] == '\0') is_key = 1;
        }
        if (is_key) {
            fprintf(out, "DSI_THEME = %s", theme_name);
            if (nl) fputc('\n', out);
            wrote = 1;
        } else {
            fwrite(p, 1, linelen, out);
            if (nl) fputc('\n', out);
        }
        if (!nl) break;
        p = nl + 1;
    }
    if (!wrote) {
        if (sz > 0 && buf[sz-1] != '\n') fputc('\n', out);
        fprintf(out, "DSI_THEME = %s\n", theme_name);
    }
    fclose(out);
    free(buf);
    return 0;
}

// Read a theme's RenderPhoto value from its theme.ini. Returns 1 (photo on),
// 0 (off), or -1 if the file/key isn't found.
static int theme_render_photo(const char *theme_dir) {
    char ini[512];
    snprintf(ini, sizeof(ini), "%s/theme.ini", theme_dir);
    FILE *f = fopen(ini, "rb");
    if (!f) return -1;
    char line[256];
    int rc = -1;
    while (fgets(line, sizeof(line), f)) {
        const char *key = "renderphoto";
        int ki = 0, i = 0;
        while (line[i] == ' ' || line[i] == '\t') i++;
        while (line[i] && key[ki]) {
            char c = line[i];
            char lc = (c >= 'A' && c <= 'Z') ? c + 32 : c;
            if (lc != key[ki]) break;
            i++; ki++;
        }
        if (key[ki] != '\0') continue;          // not the RenderPhoto key
        // The next non-space char after the key must start the value (skip '=').
        while (line[i] == ' ' || line[i] == '\t' || line[i] == '=') i++;
        rc = (line[i] == '1') ? 1 : 0;
        break;
    }
    fclose(f);
    return rc;
}

// Load a theme's top-screen image (preferring top_photo.png, else top.png) and
// blit it scaled into the preview pane. Searches background/ first, then the
// tree. If nothing is found, shows a placeholder. `theme_folder` is the full
// folder name (may include a variant suffix).
static void draw_theme_preview(const char *theme_folder) {
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/%s", THEMES_DIR, theme_folder);

    char img[600]; int got = 0;
    // Always use top.png (the theme's plain top-screen image), background/ first.
    {
        char cand[600];
        snprintf(cand, sizeof(cand), "%s/background/top.png", dir);
        struct stat st;
        if (stat(cand, &st) == 0) { snprintf(img, sizeof(img), "%s", cand); got = 1; }
        else if (find_file(dir, "top.png", cand, sizeof(cand)) == 0) {
            snprintf(img, sizeof(img), "%s", cand); got = 1;
        }
    }
    if (!got) {
        fb_rect(PREV_X, PREV_Y, PREV_W, PREV_H, rgb15(24, 24, 28));
        fb_text(PREV_X + 6, PREV_Y + PREV_H / 2 - 4, "no img", rgb15(140,140,140));
        return;
    }

    // One-entry cache: previews are decoded from a 256x192 PNG (slow), so if the
    // same image was just decoded, blit the cached pane pixels instead. This
    // makes scrolling back over themes -- and the variant screen (whose variants
    // all share one top.png) -- instant.
    static char cache_path[600] = "";
    static unsigned char cache_px[PREV_W * PREV_H * 3];
    static int cache_valid = 0;
    if (cache_valid && strcmp(cache_path, img) == 0) {
        for (int yy = 0; yy < PREV_H; yy++)
            for (int xx = 0; xx < PREV_W; xx++) {
                const unsigned char *s = cache_px + ((size_t)yy * PREV_W + xx) * 3;
                fb_px(PREV_X + xx, PREV_Y + yy, rgb15(s[0], s[1], s[2]));
            }
        return;
    }

    unsigned char *rgba = NULL; int iw = 0, ih = 0;
    if (read_png_full(img, &rgba, &iw, &ih) != 0 || !rgba) {
        fb_rect(PREV_X, PREV_Y, PREV_W, PREV_H, rgb15(24, 24, 28));
        fb_text(PREV_X + 6, PREV_Y + PREV_H / 2 - 4, "...", rgb15(140,140,140));
        return;
    }
    // RGBA -> RGB (preview pane is opaque; drop alpha).
    unsigned char *rgb = (unsigned char *)malloc((size_t)iw * ih * 3);
    if (rgb) {
        for (int i = 0; i < iw * ih; i++) {
            rgb[i*3+0] = rgba[i*4+0]; rgb[i*3+1] = rgba[i*4+1]; rgb[i*3+2] = rgba[i*4+2];
        }
        fb_blit_rgb(rgb, iw, ih, PREV_X, PREV_Y, PREV_W, PREV_H);
        // Populate the cache by sampling the same downscale into cache_px.
        for (int yy = 0; yy < PREV_H; yy++) {
            int sy = (int)((long)yy * ih / PREV_H);
            for (int xx = 0; xx < PREV_W; xx++) {
                int sx = (int)((long)xx * iw / PREV_W);
                const unsigned char *s = rgb + ((size_t)sy * iw + sx) * 3;
                unsigned char *d = cache_px + ((size_t)yy * PREV_W + xx) * 3;
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
            }
        }
        snprintf(cache_path, sizeof(cache_path), "%s", img);
        cache_valid = 1;
        free(rgb);
    }
    free(rgba);
}

// Draw the embedded "Default theme" preview (a fixed PNG baked into the binary)
// into the preview pane. Used for the pinned Default row, which has no folder.
static void draw_default_preview(void) {
    unsigned char *rgba = NULL; int iw = 0, ih = 0;
    if (decode_png_mem(g_default_preview_png, (long)g_default_preview_png_len,
                       &rgba, &iw, &ih) != 0 || !rgba) {
        fb_rect(PREV_X, PREV_Y, PREV_W, PREV_H, rgb15(24, 24, 28));
        fb_text(PREV_X + 8, PREV_Y + PREV_H/2 - 4, "Default", rgb15(140,140,140));
        return;
    }
    unsigned char *rgb = (unsigned char *)malloc((size_t)iw * ih * 3);
    if (rgb) {
        for (int i = 0; i < iw * ih; i++) {
            rgb[i*3+0] = rgba[i*4+0]; rgb[i*3+1] = rgba[i*4+1]; rgb[i*3+2] = rgba[i*4+2];
        }
        fb_blit_rgb(rgb, iw, ih, PREV_X, PREV_Y, PREV_W, PREV_H);
        free(rgb);
    }
    free(rgba);
}

// Destination folder for a theme variant: "<THEMES_DIR>/<name><suffix>"
static void theme_copy_path(const char *name, const char *suffix,
                            char *out, size_t outsz) {
    snprintf(out, outsz, "%s/%s%s", THEMES_DIR, name, suffix);
}

// Composite the embedded frame over a theme's top.png and write top_photo.png.
// The frame image encodes two layers by alpha: fully-opaque pixels (alpha 255)
// are the frame BORDER; partially-transparent pixels are the soft drop SHADOW.
// `add_frame` draws the border; `add_shadow` blends the shadow. Either/both may
// be on. Where a layer is off (or the pixel is fully transparent), top.png shows
// through. Dimensions must match (256x192); on read failure / mismatch we fall
// back to a plain copy. Returns 0 on success (incl. fallback).
static int bake_frame_onto_top(const char *top_path, const char *out_path,
                               int add_frame, int add_shadow) {
    unsigned char *top = NULL, *frame = NULL;
    int tw = 0, th = 0, fw = 0, fh = 0;
    int ok = 0;

    if (read_png_full(top_path, &top, &tw, &th) == 0 &&
        decode_png_mem(g_frame_png, (long)g_frame_png_len, &frame, &fw, &fh) == 0 &&
        tw == fw && th == fh) {
        unsigned char *rgb = (unsigned char *)malloc((size_t)tw * th * 3);
        if (rgb) {
            for (int i = 0; i < tw * th; i++) {
                int a = frame[i*4 + 3];
                int use_border = (a == 255) && add_frame;
                int use_shadow = (a > 0 && a < 255) && add_shadow;
                if (use_border) {                // solid frame border
                    rgb[i*3+0] = frame[i*4+0];
                    rgb[i*3+1] = frame[i*4+1];
                    rgb[i*3+2] = frame[i*4+2];
                } else if (use_shadow) {         // soft shadow -> alpha blend
                    for (int c = 0; c < 3; c++) {
                        int fpx = frame[i*4 + c];
                        int bpx = top[i*4 + c];
                        rgb[i*3 + c] = (unsigned char)((fpx * a + bpx * (255 - a) + 127) / 255);
                    }
                } else {                         // layer off / transparent -> top
                    rgb[i*3+0] = top[i*4+0];
                    rgb[i*3+1] = top[i*4+1];
                    rgb[i*3+2] = top[i*4+2];
                }
            }
            ok = (write_png(out_path, rgb, tw, th) == 0);
            free(rgb);
        }
    }
    free(top); free(frame);

    if (ok) return 0;
    // Fallback: couldn't read / sizes differ -- copy top.png as-is.
    return copy_file(top_path, out_path);
}

// Write an embedded byte blob (e.g. one of the shoulder PNGs) to a file.
static int write_blob(const char *path, const unsigned char *data, unsigned long len) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t w = fwrite(data, 1, len, f);
    fclose(f);
    return (w == len) ? 0 : -1;
}

// Place the cleaned L/R shoulder photo overlays into the copied theme's "ui"
// folder (which is where TWiLight looks for Lshoulder_photo.png etc. when photos
// are on). The ui folder already exists in the copy; if for some reason it does
// not, create it. Best-effort: a failure here doesn't fail the whole copy.
static void place_shoulder_photos(const char *theme_dir) {
    char uidir[512];
    snprintf(uidir, sizeof(uidir), "%s/ui", theme_dir);
    struct stat st;
    if (stat(uidir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        // No ui folder in this theme -- try to locate one anywhere in the tree.
        char found[600];
        if (find_dir(theme_dir, "ui", found, sizeof(found)) == 0)
            snprintf(uidir, sizeof(uidir), "%s", found);
        else
            mkdir(uidir, 0777);   // fall back to creating ui/ at the theme root
    }
    char p[600];
    snprintf(p, sizeof(p), "%s/Lshoulder_photo.png", uidir);
    write_blob(p, g_lshoulder_photo_png, g_lshoulder_photo_png_len);
    snprintf(p, sizeof(p), "%s/Rshoulder_photo.png", uidir);
    write_blob(p, g_rshoulder_photo_png, g_rshoulder_photo_png_len);
    snprintf(p, sizeof(p), "%s/Lshoulder_photo_greyed.png", uidir);
    write_blob(p, g_lshoulder_photo_greyed_png, g_lshoulder_photo_greyed_png_len);
    snprintf(p, sizeof(p), "%s/Rshoulder_photo_greyed.png", uidir);
    write_blob(p, g_rshoulder_photo_greyed_png, g_rshoulder_photo_greyed_png_len);
}

// Make one photo-enabled variant of theme `name` into "<name><suffix>":
//   1. recursively duplicate the folder
// Build the variant suffix " - p[f][s][LR]" from the selected adjustments into
// `out`. Photo (p) is always present.
static void build_variant_suffix(int frame, int shadow, int lr, char *out, size_t outsz) {
    char adj[8]; int n = 0;
    adj[n++] = 'p';
    if (frame)  adj[n++] = 'f';
    if (shadow) adj[n++] = 's';
    adj[n] = '\0';
    if (lr) snprintf(out, outsz, " - %sLR", adj);
    else    snprintf(out, outsz, " - %s", adj);
}

// --- Theme build, split into phases so the loading bar can reflect real work ---

// Phase 1: duplicate the theme folder as "<name><suffix>" and flip RenderPhoto=1.
// Returns 0 and writes the destination path into `dst_out` on success.
static int tv_copy(const char *name, const char *suffix, char *dst_out, size_t dstsz) {
    char dst[512];
    theme_copy_path(name, suffix, dst, sizeof(dst));
    char src[512];
    snprintf(src, sizeof(src), "%s/%s", THEMES_DIR, name);
    if (copy_dir_recursive(src, dst) != 0) return -1;
    if (theme_enable_photo(dst) != 0) return -1;
    snprintf(dst_out, dstsz, "%s", dst);
    return 0;
}

// Resolve the copied theme's top.png and the matching top_photo.png path.
static int tv_top_paths(const char *dst, char *top, size_t topsz,
                        char *topphoto, size_t tpsz) {
    char topdir[512];
    snprintf(topdir, sizeof(topdir), "%s/background", dst);
    snprintf(top, topsz, "%s/top.png", topdir);
    struct stat st;
    if (stat(top, &st) != 0) {
        if (find_file(dst, "top.png", top, topsz) != 0) return -1;
    }
    snprintf(topphoto, tpsz, "%s", top);
    char *slash = strrchr(topphoto, '/');
    if (slash) snprintf(slash + 1, tpsz - (slash + 1 - topphoto), "top_photo.png");
    else snprintf(topphoto, tpsz, "%s/top_photo.png", dst);
    return 0;
}

// Phase 2: write top_photo.png as a plain copy of top.png (the photo baseline).
static int tv_photo(const char *dst) {
    char top[600], topphoto[600];
    if (tv_top_paths(dst, top, sizeof(top), topphoto, sizeof(topphoto)) != 0) return 0;
    return copy_file(top, topphoto);
}

// Phase 3: apply the chosen adjustments -- re-bake top_photo.png with the frame
// border and/or shadow, and drop in the L/R shoulder overlays. No-op if none.
static int tv_adjust(const char *dst, int add_frame, int add_shadow, int add_lr) {
    if (add_frame || add_shadow) {
        char top[600], topphoto[600];
        if (tv_top_paths(dst, top, sizeof(top), topphoto, sizeof(topphoto)) == 0)
            bake_frame_onto_top(top, topphoto, add_frame, add_shadow);
    }
    if (add_lr) place_shoulder_photos(dst);
    return 0;
}

// ---- Quick mode: convert every unconverted photo as fast as possible, then
// return so the app can exit to TWiLightMenu. Draws a single static "Syncing..."
// line once (so a multi-second sync doesn't look like a freeze), then runs a
// tight decode loop with no per-frame UI work at all -- the screen never changes
// until it's done. Returns the number successfully synced.
static int run_quick_sync(void) {
    fb_rect(0, 0, SCR_W, SCR_H, COL_BG);
    fb_text(8, SCR_H / 2 - 4, "Syncing...", COL_TEXT);
    swiWaitForVBlank();   // make sure the line is on screen before we get busy

    int ok = 0;
    for (int i = 0; i < g_n_unconv; i++) {
        PhotoEntry *p = &g_photos[g_unconv[i]];
        if (convert_entry(p) == 1) { ok++; p->done = 1; p->convert = 0; }
    }
    return ok;
}

static void enter_menu_chrome(void) {
    draw_chrome("DSiPhotoSync", NULL, 0);
    Hint h[3] = { { NULL, 'A', "Select", 0 },
                  { "START", 0, "Sync new", 0 },
                  { "SELECT", 0, "Quit", 0 } };
    draw_footer_hints(h, 3);
}

// Draw the "Riddle of the day" on the sub (second) screen. Used in list view,
// where the second screen is otherwise idle.
static void draw_riddle(void) {
    g_draw_fb = g_fb_sub;
    fb_rect(0, 0, SCR_W, SCR_H, COL_BG);
    u16 head = rgb15(150, 200, 245);
    u16 txt  = rgb15(205, 205, 215);

    fb_text(8, 8, "Riddle of the day", head);
    fb_rect(0, 20, SCR_W, 1, COL_DIVIDER);

    const Riddle *r = &g_riddles[g_riddle_idx];
    // Word-wrap the question to the screen width (about 40 chars per line).
    const int MAXCH = 40;
    const char *p = r->q;
    int y = 40;
    char lineb[44];
    while (*p) {
        int take = 0, lastspace = -1;
        while (p[take] && take < MAXCH) {
            if (p[take] == ' ') lastspace = take;
            take++;
        }
        if (p[take] && lastspace > 0) take = lastspace;   // break on a space
        int n = take; if (n > (int)sizeof(lineb) - 1) n = sizeof(lineb) - 1;
        memcpy(lineb, p, n); lineb[n] = '\0';
        fb_text(8, y, lineb, txt);
        y += 11;
        p += take;
        while (*p == ' ') p++;
    }

    g_draw_fb = g_fb;
}

// ---------------------------------------------------------------------------
// Controls legend, drawn on the sub (opposite) screen in gallery view. Lists
// every control for the current screen so nothing is unmarked.
// ---------------------------------------------------------------------------
static void draw_legend(int screen, int view_grid) {
    (void)view_grid;   // legend only renders in gallery view; kept for symmetry
    // Redirect all drawing primitives to the sub (legend) framebuffer.
    g_draw_fb = g_fb_sub;
    fb_rect(0, 0, SCR_W, SCR_H, COL_BG);
    u16 head = rgb15(150, 200, 245);
    u16 txt  = rgb15(160, 160, 170);

    fb_text(8, 8, "Controls", head);
    fb_rect(0, 20, SCR_W, 1, COL_DIVIDER);

    // A legend row is a key "kind" (which glyph[s] to draw) plus a description.
    enum { K_UPDOWN, K_LR, K_LEFTRIGHT, K_A, K_B, K_X, K_Y, K_AB,
           K_A_MOVE, K_START, K_SELECT };
    struct { int kind; const char *d; } rows[12];
    int nr = 0;
    #define ADD_ROW(K,D) do { rows[nr].kind=(K); rows[nr].d=(D); nr++; } while(0)

    if (screen == SCREEN_MENU) {
        ADD_ROW(K_UPDOWN, "Move selection");
        ADD_ROW(K_A,      "Open item");
        ADD_ROW(K_START,  "Sync all new photos");
        ADD_ROW(K_SELECT, "Quit app");
    } else if (screen == SCREEN_ADD) {
        ADD_ROW(K_A,      "Toggle");
        ADD_ROW(K_Y,      "Toggle all");
        ADD_ROW(K_X,      "Expand");
        ADD_ROW(K_SELECT, "Switch view");
        ADD_ROW(K_START,  "Sync selected");
    } else if (screen == SCREEN_LIBRARY) {
        ADD_ROW(K_A,      "Expand / collapse");
        ADD_ROW(K_SELECT, "Switch view");
        ADD_ROW(K_X,      "Remove");
        ADD_ROW(K_Y,      "Remove all");
    } else if (screen == SCREEN_SETTINGS) {
        ADD_ROW(K_LEFTRIGHT, "Change setting");
        ADD_ROW(K_START,  "Save & back to menu");
        ADD_ROW(K_B,      "Back to menu");
    } else if (screen == SCREEN_EXPAND) {
        ADD_ROW(K_AB,     "Collapse photo");
    } else if (screen == SCREEN_CONFIRM_DELETE) {
        ADD_ROW(K_A,      "Confirm");
        ADD_ROW(K_B,      "Cancel");
    }
    #undef ADD_ROW

    int y = 40;
    const int KEYX = 8;       // key column x
    const int DESCX = 104;    // description column x
    for (int i = 0; i < nr; i++) {
        int by = y - 2;       // glyphs sit ~2px above the text baseline
        int kx = KEYX;
        switch (rows[i].kind) {
            case K_UPDOWN:
                kx += fb_button_arrow(kx, by, 0, COL_BTN, COL_BG) + 3;   // up
                kx += fb_button_arrow(kx, by, 1, COL_BTN, COL_BG);        // down
                break;
            case K_LEFTRIGHT:
                kx += fb_button_arrow(kx, by, 2, COL_BTN, COL_BG) + 3;    // left
                kx += fb_button_arrow(kx, by, 3, COL_BTN, COL_BG);        // right
                break;
            case K_LR:
                kx += fb_corner_btn(kx, by, 'L', 0, COL_BTN, COL_BG) + 3;
                kx += fb_corner_btn(kx, by, 'R', 1, COL_BTN, COL_BG);
                break;
            case K_A:      fb_button(kx, by, 'A', COL_BTN, COL_BG); break;
            case K_B:      fb_button(kx, by, 'B', COL_BTN, COL_BG); break;
            case K_X:      fb_button(kx, by, 'X', COL_BTN, COL_BG); break;
            case K_Y:      fb_button(kx, by, 'Y', COL_BTN, COL_BG); break;
            case K_AB:
                kx += fb_button(kx, by, 'A', COL_BTN, COL_BG) + 3;
                kx += fb_button(kx, by, 'B', COL_BTN, COL_BG);
                break;
            case K_A_MOVE:
                kx += fb_button(kx, by, 'A', COL_BTN, COL_BG) + 4;
                fb_text(kx, y, "+", txt); kx += 9;
                kx += fb_button_arrow(kx, by, 0, COL_BTN, COL_BG) + 2;
                fb_button_arrow(kx, by, 1, COL_BTN, COL_BG);
                break;
            case K_START:  fb_pill(kx, by, "START", COL_BTN, COL_BG); break;
            case K_SELECT: fb_pill(kx, by, "SELECT", COL_BTN, COL_BG); break;
        }
        fb_text(DESCX, y, rows[i].d, txt);
        y += 14;
    }

    g_draw_fb = g_fb;        // restore main-screen drawing
}

// Decide what the second (opposite) screen shows: the controls legend when a
// photo page is in gallery view, otherwise the riddle of the day.
static void update_second_screen(int screen, int view_grid) {
    int gallery = (screen == SCREEN_ADD || screen == SCREEN_LIBRARY) && view_grid;
    if (gallery) draw_legend(screen, view_grid);
    else         draw_riddle();
}


// Shared list-screen behavior, used by both Add and Library.
// Operating on the caller's loop state by pointer keeps the three screens
// provably consistent (a scroll/preview fix lands in one place, not three).
// ---------------------------------------------------------------------------

// Handle navigation within a paged list. Up/Down move the selection one row
// (crossing pages as needed); L/R jump a whole page. `top` is kept aligned to
// a page boundary so the fixed box always shows a clean page.
static void list_navigate(int *sel, int *top, int *settle, int *redraw_list,
                          int down, int n) {
    if (n <= 0) return;
    int moved = 0;
    if (down & KEY_UP)    { if (*sel > 0)       { (*sel)--; moved = 1; } }
    if (down & KEY_DOWN)  { if (*sel < n - 1)   { (*sel)++; moved = 1; } }
    if (down & (KEY_LEFT | KEY_L)) {
        if (*sel - VIS_ROWS >= 0) *sel -= VIS_ROWS;
        else if (*sel > 0)        *sel = 0;
        moved = 1;
    }
    if (down & (KEY_RIGHT | KEY_R)) {
        if (*sel + VIS_ROWS <= n - 1) *sel += VIS_ROWS;
        else if (*sel < n - 1)        *sel = n - 1;
        moved = 1;
    }
    if (moved) { *settle = 0; *redraw_list = 1; }
    // Keep the page (top) aligned to the selection.
    int new_top = (*sel / VIS_ROWS) * VIS_ROWS;
    if (new_top != *top) { *top = new_top; *redraw_list = 1; }
}

// 2D navigation for the gallery grid. Right from the last cell of a page rolls
// to the next page; Left from the first cell rolls to the previous page. Up/Down
// move a row but stay within the current page (they do NOT flip pages). L/R
// shoulders page directly. *top stays aligned to a page boundary.
static void grid_navigate(int *sel, int *top, int *settle, int *redraw, int down, int n) {
    if (n <= 0) return;
    int moved = 0;
    int page_lo = *top;
    int page_hi = *top + GRID_CELLS - 1;
    if (page_hi > n - 1) page_hi = n - 1;

    if (down & KEY_RIGHT) {           // next cell; rolls to next page past the end
        if (*sel < n - 1) { (*sel)++; moved = 1; }
    }
    if (down & KEY_LEFT) {            // prev cell; rolls to previous page
        if (*sel > 0) { (*sel)--; moved = 1; }
    }
    if (down & KEY_DOWN) {            // down a row, clamped to this page
        int t = *sel + GRID_COLS;
        if (t <= page_hi) { *sel = t; moved = 1; }
    }
    if (down & KEY_UP) {              // up a row, clamped to this page
        int t = *sel - GRID_COLS;
        if (t >= page_lo) { *sel = t; moved = 1; }
    }
    if (down & KEY_R) {              // page forward
        if (*sel + GRID_CELLS <= n - 1) *sel += GRID_CELLS;
        else if (*sel < n - 1)          *sel = n - 1;
        moved = 1;
    }
    if (down & KEY_L) {              // page back
        if (*sel - GRID_CELLS >= 0) *sel -= GRID_CELLS;
        else if (*sel > 0)          *sel = 0;
        moved = 1;
    }
    if (moved) { *settle = 0; *redraw = 1; }
    int new_top = (*sel / GRID_CELLS) * GRID_CELLS;
    if (new_top != *top) { *top = new_top; *redraw = 1; }
}

// Once the selection has settled for a few frames, (re)load the hover preview
// for the current row. `ids` maps row -> photo index; `mode` picks the decoder
// (Add previews the source JPEG, Library previews the converted PNG).
static void list_update_preview(int *last_preview_sel, int *settle, int *redraw_prev,
                                int sel, int n, const int *ids, ListMode mode) {
    (*settle)++;
    if (sel != *last_preview_sel && *settle >= 4) {
        if (n > 0) {
            if (mode == MODE_ADD) preview_load_jpeg(ids[sel]);
            else                  preview_load_png(ids[sel]);
        } else {
            preview_free();
        }
        *last_preview_sel = sel;
        *redraw_prev = 1;
    }
}

// Return from a sub-screen to the root menu, restoring its chrome.
static void go_to_menu(Screen *screen, int *menu_sel, int *last_preview_sel,
                       int *redraw_list, int new_menu_sel) {
    preview_free();
    *last_preview_sel = -2;
    *screen = SCREEN_MENU;
    *menu_sel = new_menu_sel;
    enter_menu_chrome();
    *redraw_list = 1;
}

int main(void) {
    // Hold both LCDs fully black for the entire boot (brightness floored). We
    // draw everything -- engines, framebuffers, the menu, the second screen --
    // while it's black, then snap the brightness back to normal in one step so
    // the very first thing the user sees is the finished menu. No fade, no
    // partial fills, no intermediate "Starting..." flash.
    REG_MASTER_BRIGHT     = (2 << 14) | 16;   // fade-to-black, full
    REG_MASTER_BRIGHT_SUB = (2 << 14) | 16;

    srand((unsigned)time(NULL));
    g_riddle_idx = rand() % N_RIDDLES;

    videoSetMode(MODE_5_2D);
    videoSetModeSub(MODE_5_2D);
    vramSetBankA(VRAM_A_MAIN_BG_0x06000000);
    vramSetBankC(VRAM_C_SUB_BG_0x06200000);
    REG_POWERCNT |= POWER_SWAP_LCDS;   // default: main engine on bottom screen
    int bg = bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
    g_fb = (u16 *)bgGetGfxPtr(bg);
    g_draw_fb = g_fb;
    int bgs = bgInitSub(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
    g_fb_sub = (u16 *)bgGetGfxPtr(bgs);
    setBackdropColor(0x0000);
    setBackdropColorSub(0x0000);
    {
        u32 two = ((u32)COL_BG << 16) | COL_BG;
        u32 *m = (u32 *)g_fb, *s = (u32 *)g_fb_sub;
        for (int i = 0; i < SCR_W * SCR_H / 2; i++) { m[i] = two; s[i] = two; }
    }

    if (!fatInitDefault()) {
        // No SD: reveal the screen so the error is visible.
        REG_MASTER_BRIGHT = 0; REG_MASTER_BRIGHT_SUB = 0;
        message_screen("SD card init failed.", "Is this reading the right SD?", NULL);
        wait_keys(KEY_START);
        return 0;
    }

    // SD is up: load saved settings (default view).
    settings_load();

    mkdir("sd:/_nds", 0777);
    mkdir("sd:/_nds/TWiLightMenu", 0777);
    mkdir("sd:/_nds/TWiLightMenu/dsimenu", 0777);
    mkdir(PHOTOS_DIR, 0777);

    build_list();
    repartition();

    if (g_photo_count == 0) {
        REG_MASTER_BRIGHT = 0; REG_MASTER_BRIGHT_SUB = 0;
        message_screen("No photos found in sd:/DCIM/.",
                       "Take a photo to SD first,", "then run this again.");
        wait_keys(KEY_START | KEY_SELECT);
        return 0;
    }

    // Quick mode: if enabled, auto-sync all new photos at boot and exit straight
    // back to TWiLightMenu -- unless SELECT is held, which drops to the normal
    // menu instead. Checked once, early, before any UI is built.
    scanKeys(); swiWaitForVBlank(); scanKeys();   // let the key state settle
    if (g_settings.quick_mode && !(keysHeld() & KEY_SELECT)) {
        REG_MASTER_BRIGHT = 0; REG_MASTER_BRIGHT_SUB = 0;   // reveal the screen
        if (g_n_unconv > 0) {
            run_quick_sync();
        } else {
            fb_rect(0, 0, SCR_W, SCR_H, COL_BG);
            fb_text(8, SCR_H / 2 - 4, "Nothing new to sync.", COL_TEXT);
            for (int i = 0; i < 45; i++) swiWaitForVBlank();   // ~0.75s glimpse
        }
        return 0;   // back to TWiLightMenu
    }

    Screen screen = SCREEN_MENU;
    int menu_sel = 0;
    int sel = 0, top = 0;            // active list selection + scroll
    int settle = 0, last_preview_sel = -2;
    int redraw_all = 0;             // full chrome redraw needed
    int redraw_list = 1;            // list/menu band needs repaint
    int redraw_prev = 0;            // preview area needs repaint
    Screen expand_from = SCREEN_LIBRARY;   // screen to return to when collapsing
    int paint_val = 0;                     // value the A-drag paints onto rows
    int painting = 0;                      // a hold-A drag is in progress
    int view_grid = 0;                     // 0 = list view, 1 = gallery grid
    int theme_sel = 0, theme_top = 0;      // theme-list selection + scroll
    int theme_apply_sel = 0;               // variant picker selection (0/1/2)
    int theme_made_idx = -1;               // base theme just made compatible
    int last_theme_preview = -2;           // preview-pane cache key
    int theme_preview_pending = 0;         // a preview decode is queued
    int theme_preview_wait = 0;            // frames to wait before decoding
    int theme_hint_state = -1;             // last drawn A-hint (compat) state
    int theme_want_current = 0;            // one-shot: select active theme on entry
    int adj_frame = 0, adj_shadow = 0, adj_lr = 0;  // builder adjustment toggles
    int adj_sel = 1;                       // adjustment row cursor (0=photo locked)
    char theme_made_suffix[VARSUF_LEN] = "";   // suffix of the variant just built
    int adj_from_create = 0;               // ADJUST entered via "Create new variant"
    int theme_page = 0;                    // 0 = Compatible page, 1 = Incompatible
    int theme_applied_idx = -1;            // base index of the currently-applied theme

    grid_free();   // mark the thumbnail cache empty
    enter_menu_chrome();

    // Auto-repeat for held directions (hold Up/Down to scroll). Initial delay
    // then a faster repeat. Discrete actions still use keysDown() (one per tap).
    keysSetRepeat(12, 4);

    // The second screen (riddle in list view, controls in gallery view) is
    // refreshed when the screen or view changes -- except for transient
    // overlays (expand, delete confirm) which leave it as it was.
    Screen legend_screen = (Screen)-1; int legend_grid = -1;
    update_second_screen(screen, view_grid);
    legend_screen = screen; legend_grid = view_grid;

    // Everything is now drawn while the display is still held black: menu chrome,
    // menu rows, and the second screen. Reveal both screens in one step so the
    // first thing the user sees is the finished menu -- no fade, no partial fill,
    // no "Starting..." flash.
    redraw_menu(menu_sel, g_n_unconv, g_n_conv);
    redraw_list = 0;
    swiWaitForVBlank();
    REG_MASTER_BRIGHT     = 0;
    REG_MASTER_BRIGHT_SUB = 0;

    while (screen != SCREEN_EXIT) {
        swiWaitForVBlank();
        scanKeys();
        int down = keysDown();
        int rep  = keysDownRepeat();   // includes the first press + auto-repeats

        if ((screen != legend_screen || view_grid != legend_grid) &&
            screen != SCREEN_EXPAND && screen != SCREEN_CONFIRM_DELETE &&
            screen != SCREEN_CONFIRM_REMOVE_ALL &&
            screen != SCREEN_THEME_EXPLAIN && screen != SCREEN_THEME_SETPROMPT) {
            update_second_screen(screen, view_grid);
            legend_screen = screen; legend_grid = view_grid;
        }

        if (screen == SCREEN_MENU) {
            if (down & KEY_SELECT) { screen = SCREEN_EXIT; continue; }
            if (rep & KEY_UP)   { if (menu_sel > 0) { menu_sel--; redraw_list = 1; } }
            if (rep & KEY_DOWN) { if (menu_sel < MENU_ITEMS - 1) { menu_sel++; redraw_list = 1; } }
            if (down & KEY_A) {
                sel = 0; top = 0; settle = 0; last_preview_sel = -2;
                if (menu_sel == 0) {            // Add: default all on
                    for (int i = 0; i < g_n_unconv; i++) g_photos[g_unconv[i]].convert = 1;
                    view_grid = g_settings.default_grid;   // start in default view
                    screen = SCREEN_ADD;
                } else if (menu_sel == 1) {     // Gallery
                    view_grid = g_settings.default_grid;   // start in default view
                    screen = SCREEN_LIBRARY;
                } else if (menu_sel == 2) {     // Use with custom theme
                    build_theme_list();
                    theme_sel = 0; theme_top = 0;
                    theme_page = 0;             // always open to Compatible
                    theme_want_current = 1;     // default-select the active theme
                    screen = SCREEN_THEMES;
                } else {                        // Settings
                    screen = SCREEN_SETTINGS;
                }
                redraw_all = 1;
                continue;
            }
            if (down & KEY_START) {
                // One-tap: convert every new photo without visiting the Add list.
                if (g_n_unconv > 0) {
                    for (int i = 0; i < g_n_unconv; i++) g_photos[g_unconv[i]].convert = 1;
                    sel = 0; top = 0; settle = 0; last_preview_sel = -2;
                    screen = SCREEN_CONVERT;
                    continue;
                } else {
                    // Nothing to do: briefly say so, then stay on the menu.
                    fb_rect(0, LIST_BAND_Y, SCR_W, LIST_BAND_H, COL_BG);
                    fb_text(LIST_X, LIST_Y + 2, "No new photos to sync.", COL_DIM);
                    for (int f = 0; f < 70; f++) swiWaitForVBlank();
                    redraw_list = 1;   // restore the menu rows
                }
            }
            if (redraw_list) { redraw_menu(menu_sel, g_n_unconv, g_n_conv); redraw_list = 0; }

            // Gently warm the first converted (PNG) thumbnails in the background
            // while the menu is up, so opening the gallery is instant. We do NOT
            // pre-decode the Add view's source JPEGs here -- those are expensive
            // and would make the menu hitch right after boot; the Add grid decodes
            // them progressively (one per frame) when actually opened.
            static int warm_done = 0;
            static int idle_frames = 0;
            if (down || rep) idle_frames = 0; else if (idle_frames < 1000) idle_frames++;
            if (idle_frames > 20 && warm_done < GRID_CELLS &&
                g_n_conv > 0 && !(keysHeld() & 0xFFFF)) {
                warm_done += grid_preload_window(g_conv, g_n_conv, 0, 0, 1);
            }
        }
        else if (screen == SCREEN_ADD) {
            int *ids   = g_unconv;
            int n      = g_n_unconv;
            ListMode m = MODE_ADD;
            int is_jpeg = 1;
            const char *empty = "Nothing new to convert.";

            int redraw_all_was = redraw_all;
            if (redraw_all) {
                draw_chrome("Add photos", NULL, !view_grid);   // no side preview in grid
                if (!view_grid) {
                    Hint left[2]  = { { NULL, 'A', "Toggle", 0 },
                                      { NULL, 'Y', "Toggle all", 0 } };
                    Hint right[2] = { { NULL, 'X', "Expand", 0 },
                                      { "START", 0, "Sync", 0 } };
                    draw_footer_2col(left, 2, right, 2);
                }
                redraw_all = 0; redraw_list = 1; redraw_prev = 1;
            }

            if (down & KEY_SELECT) {                 // toggle list <-> grid
                view_grid = !view_grid;
                if (!view_grid) { preview_free(); last_preview_sel = -2; }
                top = (sel / (view_grid ? GRID_CELLS : VIS_ROWS))
                      * (view_grid ? GRID_CELLS : VIS_ROWS);
                redraw_all = 1;
                continue;
            }
            if (down & KEY_B) {
                go_to_menu(&screen, &menu_sel, &last_preview_sel, &redraw_list, 0);
                continue;
            }
            if ((down & KEY_X) && n > 0) {           // expand current photo
                expand_from = screen;
                screen = SCREEN_EXPAND; redraw_all = 1;
                continue;
            }
            if ((down & KEY_Y) && n > 0) {           // toggle ALL on/off
                int any_on = 0;
                for (int j = 0; j < n; j++) if (g_photos[ids[j]].convert) { any_on = 1; break; }
                int v = any_on ? 0 : 1;
                for (int j = 0; j < n; j++) g_photos[ids[j]].convert = v;
                redraw_list = 1;
            }
            if (down & KEY_A) {                       // tap A: toggle current
                if (sel < n) {
                    int nv = g_photos[ids[sel]].convert ^ 1;
                    g_photos[ids[sel]].convert = nv;
                    paint_val = nv; painting = 1;
                    redraw_list = 1;
                }
            }
            if ((down & KEY_START) && n > 0) {
                grid_free();
                screen = SCREEN_CONVERT;
                continue;
            }

            // Navigation + drag-select (shared by both views).
            int held_a = (keysHeld() & KEY_A) != 0;
            if (!held_a) painting = 0;
            int prev_sel = sel, prev_top = top;
            if (view_grid) grid_navigate(&sel, &top, &settle, &redraw_list, rep, n);
            else           list_navigate(&sel, &top, &settle, &redraw_list, rep, n);
            if (painting && held_a && sel != prev_sel && sel < n) {
                if (g_photos[ids[sel]].convert != paint_val) {
                    g_photos[ids[sel]].convert = paint_val;
                    redraw_list = 1;
                }
            }

            if (view_grid) {
                int page_changed = (top != prev_top) || redraw_all_was;
                grid_frame_update(ids, n, sel, prev_sel, top, prev_top, m, empty,
                                  is_jpeg, page_changed, redraw_list);
                redraw_list = 0;
            } else {
                list_update_preview(&last_preview_sel, &settle, &redraw_prev, sel, n, ids, m);
                if (redraw_list) { redraw_rows(ids, n, sel, top, m, empty); redraw_list = 0; }
                if (redraw_prev) { redraw_preview(); redraw_prev = 0; }
            }
        }
        else if (screen == SCREEN_LIBRARY) {
            int n = g_n_conv;
            int redraw_all_was = redraw_all;
            if (redraw_all) {
                char hdr[32]; snprintf(hdr, sizeof(hdr), "%d", n);
                draw_chrome("Gallery", hdr, !view_grid);
                if (!view_grid && n > 0) {
                    Hint left[2]  = { { NULL, 'A', "Expand", 0 },
                                      { NULL, 'X', "Remove", 0 } };
                    Hint right[2] = { { NULL, 'Y', "Remove all", 0 },
                                      { "SELECT", 0, "Switch view", 0 } };
                    draw_footer_2col(left, 2, right, 2);
                }
                redraw_all = 0; redraw_list = 1; redraw_prev = 1;
            }
            if (down & KEY_SELECT) {                 // toggle list <-> grid
                view_grid = !view_grid;
                if (!view_grid) { preview_free(); last_preview_sel = -2; }
                top = (sel / (view_grid ? GRID_CELLS : VIS_ROWS))
                      * (view_grid ? GRID_CELLS : VIS_ROWS);
                redraw_all = 1;
                continue;
            }
            if (down & KEY_B) {
                go_to_menu(&screen, &menu_sel, &last_preview_sel, &redraw_list, 1);
                continue;
            }
            if ((down & KEY_A) && n > 0) {
                expand_from = SCREEN_LIBRARY;
                screen = SCREEN_EXPAND; redraw_all = 1;
                continue;
            }
            if ((down & KEY_X) && n > 0) {           // delete current photo
                screen = SCREEN_CONFIRM_DELETE; redraw_all = 1;
                continue;
            }
            if ((down & KEY_Y) && n > 0) {           // remove ALL (confirm first)
                screen = SCREEN_CONFIRM_REMOVE_ALL; redraw_all = 1;
                continue;
            }

            int prev_sel = sel, prev_top = top;
            if (view_grid) grid_navigate(&sel, &top, &settle, &redraw_list, rep, n);
            else           list_navigate(&sel, &top, &settle, &redraw_list, rep, n);

            if (view_grid) {
                int page_changed = (top != prev_top) || redraw_all_was;
                grid_frame_update(g_conv, n, sel, prev_sel, top, prev_top,
                                  MODE_LIBRARY, "No converted photos yet.",
                                  0, page_changed, redraw_list);
                redraw_list = 0;
            } else {
                list_update_preview(&last_preview_sel, &settle, &redraw_prev, sel, n,
                                    g_conv, MODE_LIBRARY);
                if (redraw_list) { redraw_rows(g_conv, n, sel, top, MODE_LIBRARY,
                                               "No converted photos yet."); redraw_list = 0; }
                if (redraw_prev) { redraw_preview(); redraw_prev = 0; }
            }
        }
        else if (screen == SCREEN_EXPAND) {
            if (redraw_all) {
                unsigned char *rgb = NULL; int iw = 0, ih = 0;
                int ok = 0;
                if (expand_from == SCREEN_ADD) {
                    // Unconverted source photo: full-decode the JPEG for a sharp
                    // fullscreen view (DSi photos are small). Fall back to reduce
                    // mode only if the image is too large to decode fully.
                    int idx = g_unconv[sel];
                    char src[400];
                    make_src_path(g_photos[idx].subdir, g_photos[idx].file,
                                  src, sizeof(src));
                    ok = (decode_jpeg(src, &rgb, &iw, &ih) == 0);
                    if (!ok) {
                        g_last_err = 0;
                        ok = (decode_jpeg_preview(src, &rgb, &iw, &ih) == 0);
                    }
                } else {
                    // Converted photo (from Library): blit its PNG.
                    int idx = g_conv[sel];
                    char outname[400];
                    make_out_name(g_photos[idx].subdir, g_photos[idx].file,
                                  outname, sizeof(outname));
                    ok = (read_png(outname, &rgb, &iw, &ih) == 0);
                }
                if (ok) {
                    // Compose the whole frame off-screen, then push it in one
                    // fast pass so the photo appears all at once (no split).
                    scratch_blit_rgb(rgb, iw, ih);
                    free(rgb);
                    scratch_present();
                } else {
                    fb_rect(0, 0, SCR_W, SCR_H, COL_BG);
                    fb_text(8, SCR_H / 2 - 4, "Could not load image.", COL_FAIL);
                }
                redraw_all = 0;
            }
            // A or B collapses back to the screen we came from.
            if (down & (KEY_A | KEY_B | KEY_X)) {
                screen = expand_from; redraw_all = 1;
                last_preview_sel = -2;     // force preview reload on return
                continue;
            }
        }
        else if (screen == SCREEN_SETTINGS) {
            static int saved_default_grid;   // baselines captured on entry
            static int saved_quick;
            if (redraw_all) {
                saved_default_grid = g_settings.default_grid;
                saved_quick = g_settings.quick_mode;
                sel = 0;
                draw_chrome("Settings", NULL, 0);
                Hint h[3] = { { NULL, 0, "Move", 1 },
                              { NULL, 0, "Change", 2 },
                              { "START", 0, "Save", 0 } };
                draw_footer_hints(h, 3);
                redraw_all = 0; redraw_list = 1;
            }
            if (down & KEY_START) {
                settings_save();
                go_to_menu(&screen, &menu_sel, &last_preview_sel, &redraw_list, 2);
                continue;
            }
            if (down & KEY_B) {     // back without saving
                go_to_menu(&screen, &menu_sel, &last_preview_sel, &redraw_list, 2);
                continue;
            }
            if (rep & KEY_UP)   { if (sel > 0) { sel--; redraw_list = 1; } }
            if (rep & KEY_DOWN) { if (sel < SETTINGS_ITEMS - 1) { sel++; redraw_list = 1; } }
            if (down & (KEY_LEFT | KEY_RIGHT)) {   // toggle the selected option
                if (sel == 0) g_settings.default_grid = !g_settings.default_grid;
                else          g_settings.quick_mode   = !g_settings.quick_mode;
                redraw_list = 1;
            }
            if (redraw_list) {
                redraw_settings(sel,
                                g_settings.default_grid != saved_default_grid,
                                g_settings.quick_mode != saved_quick);
                redraw_list = 0;
            }
        }
        else if (screen == SCREEN_CONVERT) {
            preview_free();
            run_conversion(g_unconv, g_n_unconv);
            { Hint h[2] = { { "START", 0, "Menu", 0 }, { "SELECT", 0, "Quit", 0 } };
              draw_footer_hints(h, 2); }
            int k = wait_keys(KEY_START | KEY_SELECT);
            if (k & KEY_SELECT) { screen = SCREEN_EXIT; continue; }
            repartition();
            go_to_menu(&screen, &menu_sel, &last_preview_sel, &redraw_list, 0);
        }
        else if (screen == SCREEN_THEMES) {
            // Two pages flipped with L/R: page 0 = Compatible (with a pinned
            // "Default theme" row at top, alphabetical after), page 1 = Incompatible.
            int list[MAX_THEMES + 2];
            int ln = build_page_list(theme_page, list);

            if (redraw_all) {
                draw_chrome("Use with custom theme", NULL, 1);
                theme_applied_idx = theme_applied_base();
                if (theme_want_current) {
                    theme_want_current = 0;
                    // Land on the currently-applied theme's row (compatible page).
                    if (theme_applied_idx >= 0 && g_themes[theme_applied_idx].compatible) {
                        for (int k = 0; k < ln; k++)
                            if (list[k] == theme_applied_idx) { theme_sel = k; break; }
                    }
                }
                if (theme_sel >= ln) theme_sel = ln > 0 ? ln - 1 : 0;
                theme_top = (theme_sel / VIS_ROWS) * VIS_ROWS;
                redraw_all = 0; redraw_list = 1; last_theme_preview = -2;
                theme_preview_pending = 1; theme_preview_wait = 0;
                theme_hint_state = -1;
            }
            if (down & KEY_B) {
                go_to_menu(&screen, &menu_sel, &last_preview_sel, &redraw_list, 2);
                continue;
            }

            // L/R flip between the Compatible and Incompatible pages.
            int flipped = 0;
            if ((down & (KEY_L | KEY_LEFT))  && theme_page != 0) { theme_page = 0; flipped = 1; }
            if ((down & (KEY_R | KEY_RIGHT)) && theme_page != 1) { theme_page = 1; flipped = 1; }
            if (flipped) {
                theme_sel = 0; theme_top = 0; redraw_list = 1; last_theme_preview = -2;
                theme_preview_pending = 1; theme_preview_wait = 0; theme_hint_state = -1;
                ln = build_page_list(theme_page, list);
            }

            int moved = 0;
            if (ln > 0) {
                if (rep & KEY_UP)   { if (theme_sel > 0) { theme_sel--; moved = 1; } }
                if (rep & KEY_DOWN) { if (theme_sel < ln - 1) { theme_sel++; moved = 1; } }
                if (theme_sel < theme_top) { theme_top = theme_sel; moved = 1; }
                if (theme_sel >= theme_top + VIS_ROWS) { theme_top = theme_sel - VIS_ROWS + 1; moved = 1; }
            }
            if (moved) {
                redraw_list = 1;
                theme_preview_pending = 1; theme_preview_wait = 10; last_theme_preview = -2;
            }

            if ((down & KEY_A) && ln > 0) {
                int cur_theme = list[theme_sel];
                if (cur_theme == THEME_DEFAULT_ROW) {
                    // Move the mark to Default and repaint the rows immediately,
                    // pushing to screen BEFORE the (slower) settings.ini write so
                    // the X feels instant.
                    theme_applied_idx = -1;
                    redraw_list = 1;
                    // Force the row repaint right now.
                    {
                        fb_rect(0, LIST_BAND_Y, ROW_W + 4, LIST_BAND_H, COL_BG);
                        fb_text(LIST_X, PAGE_Y, "Compatible Themes", COL_HEADER);
                        int gy = PAGE_Y - 2, rx = LIST_X + ROW_W - 4, rw = 6 - 1 + 8;
                        fb_corner_btn(rx - rw, gy, 'R', 1, COL_BTN, COL_BG);
                        fb_corner_btn(rx - rw - rw - 3, gy, 'L', 0, COL_BTN, COL_BG);
                        for (int r = 0; r < VIS_ROWS && theme_top + r < ln; r++) {
                            int idx2 = theme_top + r, ti2 = list[idx2];
                            int yy = LIST_Y + r * ROW_H;
                            int sel2 = (idx2 == theme_sel);
                            if (sel2) fb_rect(LIST_X - 2, yy - 1, ROW_W, ROW_H, COL_HILITE);
                            char l2[48];
                            if (ti2 == THEME_DEFAULT_ROW)
                                snprintf(l2, sizeof(l2), "[%c] Default", theme_applied_idx == -1 ? 'X' : ' ');
                            else {
                                int nv2 = theme_variant_count(&g_themes[ti2]) + 1;
                                char nm2[30]; snprintf(nm2, sizeof(nm2), "%s", g_themes[ti2].name);
                                if ((int)strlen(g_themes[ti2].name) > 20) { nm2[19]='.'; nm2[20]='\0'; }
                                snprintf(l2, sizeof(l2), "[%c] %s (%d)", ti2 == theme_applied_idx ? 'X':' ', nm2, nv2);
                            }
                            fb_text_clip(LIST_X, yy, l2, sel2 ? COL_TEXT : COL_DIM, 26);
                        }
                        char pos[20];
                        snprintf(pos, sizeof(pos), "%d of %d", theme_sel + 1, ln);
                        fb_rect(LIST_X, INFO_Y - 1, ROW_W, 10, COL_BG);
                        fb_text(LIST_X, INFO_Y, pos, COL_DIM);
                        swiWaitForVBlank();
                    }
                    // Show "Applying theme..." then "Theme applied." in the preview
                    // pane (the main list's counter line is reserved for # of #).
                    fb_rect(PREV_X, PREV_Y, PREV_W, PREV_H, rgb15(24, 24, 28));
                    fb_text(PREV_X + 4, PREV_Y + PREV_H/2 - 4, "Applying...", rgb15(220,220,220));
                    swiWaitForVBlank();
                    twl_set_theme(DEFAULT_THEME_NAME);
                    fb_rect(PREV_X, PREV_Y, PREV_W, PREV_H, rgb15(24, 24, 28));
                    fb_text(PREV_X + 4, PREV_Y + PREV_H/2 - 4, "Applied.", rgb15(120,200,140));
                    for (int f = 0; f < 90; f++) swiWaitForVBlank();
                    // Restore the Default preview afterwards.
                    draw_default_preview();
                    redraw_list = 0;   // rows already painted
                } else {
                    theme_made_idx = cur_theme;
                    if (theme_page == 0) {
                        theme_apply_sel = 0; screen = SCREEN_THEME_APPLY; redraw_all = 1; continue;
                    } else {
                        screen = SCREEN_THEME_EXPLAIN; redraw_all = 1; continue;
                    }
                }
            }

            if (redraw_list) {
                fb_rect(0, LIST_BAND_Y, ROW_W + 4, LIST_BAND_H, COL_BG);
                fb_text(LIST_X, PAGE_Y,
                        theme_page == 0 ? "Compatible Themes" : "Incompatible Themes",
                        COL_HEADER);
                int gy = PAGE_Y - 2;
                int rx = LIST_X + ROW_W - 4;
                int rw = 6 - 1 + 8;
                fb_corner_btn(rx - rw, gy, 'R', 1, COL_BTN, COL_BG);
                fb_corner_btn(rx - rw - rw - 3, gy, 'L', 0, COL_BTN, COL_BG);

                if (ln == 0) {
                    fb_text(LIST_X, LIST_Y, "-", COL_DIM);
                } else {
                    for (int r = 0; r < VIS_ROWS && theme_top + r < ln; r++) {
                        int idx = theme_top + r;
                        int ti = list[idx];
                        int y = LIST_Y + r * ROW_H;
                        int is_sel = (idx == theme_sel);
                        if (is_sel) fb_rect(LIST_X - 2, y - 1, ROW_W, ROW_H, COL_HILITE);
                        char line[48];
                        if (ti == THEME_DEFAULT_ROW) {
                            // Pinned Default row (no mark column needed, no count).
                            snprintf(line, sizeof(line), "[%c] Default",
                                     theme_applied_idx == -1 ? 'X' : ' ');
                        } else if (theme_page == 0) {
                            // Variant count includes the photo/original options too:
                            // each generated variant + the base (1). Always shown.
                            int nv = theme_variant_count(&g_themes[ti]) + 1;
                            char cnt[10];
                            snprintf(cnt, sizeof(cnt), " (%d)", nv);
                            char nm[30];
                            snprintf(nm, sizeof(nm), "%s", g_themes[ti].name);
                            if ((int)strlen(g_themes[ti].name) > 20) { nm[19]='.'; nm[20]='\0'; }
                            snprintf(line, sizeof(line), "[%c] %s%s",
                                     ti == theme_applied_idx ? 'X' : ' ', nm, cnt);
                        } else {
                            char nm[40];
                            snprintf(nm, sizeof(nm), "%s", g_themes[ti].name);
                            if ((int)strlen(g_themes[ti].name) > 30) { nm[29]='.'; nm[30]='\0'; }
                            snprintf(line, sizeof(line), "%s", nm);
                        }
                        fb_text_clip(LIST_X, y, line, is_sel ? COL_TEXT : COL_DIM, 26);
                    }
                }
                // Position counter "# of #" at LIST_X/INFO_Y, matching the Add
                // and Library list screens.
                {
                    char pos[20];
                    snprintf(pos, sizeof(pos), "%d of %d", ln > 0 ? theme_sel + 1 : 0, ln);
                    fb_rect(LIST_X, INFO_Y - 1, ROW_W, 10, COL_BG);
                    fb_text(LIST_X, INFO_Y, pos, COL_DIM);
                }
                redraw_list = 0;
            }

            // Context A-button hint.
            {
                int comp = (theme_page == 0);
                if (comp != theme_hint_state) {
                    Hint hh[2] = { { NULL, 'A', comp ? "Select" : "Make compatible", 0 },
                                   { NULL, 'B', "Back", 0 } };
                    draw_footer_hints(hh, 2);
                    theme_hint_state = comp;
                }
            }

            // Settled preview load. Skip the (blocking) decode on any frame where
            // a key is held/pressed, so a slow decode never swallows the next tap;
            // the preview loads once the user pauses. The Default row has no preview.
            if (theme_preview_pending && ln > 0 && !down && !rep && !keysHeld()) {
                if (theme_preview_wait > 0) theme_preview_wait--;
                else if (theme_sel != last_theme_preview) {
                    int ti = list[theme_sel];
                    if (ti == THEME_DEFAULT_ROW) {
                        draw_default_preview();
                    } else {
                        draw_theme_preview(g_themes[ti].name);
                    }
                    last_theme_preview = theme_sel;
                    theme_preview_pending = 0;
                    // A decode can take longer than a frame; a quick A tap during
                    // it could otherwise be missed. Re-scan and, if A is now down,
                    // act on it immediately so the press isn't dropped.
                    scanKeys();
                    if ((keysDown() & KEY_A) && ln > 0) {
                        int ct = list[theme_sel];
                        if (ct != THEME_DEFAULT_ROW) {
                            theme_made_idx = ct;
                            theme_apply_sel = 0;
                            screen = (theme_page == 0) ? SCREEN_THEME_APPLY : SCREEN_THEME_EXPLAIN;
                            redraw_all = 1;
                            continue;
                        }
                    }
                }
            }
        }
        else if (screen == SCREEN_THEME_EXPLAIN) {
            // Screen 1/2: Theme Builder intro.
            if (redraw_all) {
                draw_chrome("Theme Builder", "Page 1/2", 0);
                int ty = LIST_Y;
                fb_text(LIST_X, ty,      "To make a photo compatible version,", COL_TEXT); ty += 12;
                fb_text(LIST_X, ty,      "the theme builder will create a copy", COL_TEXT); ty += 12;
                fb_text(LIST_X, ty,      "of the original theme and apply any", COL_TEXT); ty += 12;
                fb_text(LIST_X, ty,      "selected adjustments.", COL_TEXT); ty += 20;
                fb_text(LIST_X, ty,      "Adjustments can be selected on the", COL_TEXT); ty += 12;
                fb_text(LIST_X, ty,      "next page.", COL_TEXT);
                Hint h[2] = { { NULL, 'A', "Next", 0 }, { NULL, 'B', "Cancel", 0 } };
                draw_footer_hints(h, 2);
                redraw_all = 0;
            }
            if (down & KEY_B) { screen = SCREEN_THEMES; redraw_all = 1; continue; }
            if (down & KEY_A) {
                adj_frame = adj_shadow = adj_lr = 0; adj_sel = 1;
                adj_from_create = 0;   // normal make-compatible flow
                screen = SCREEN_THEME_ADJUST; redraw_all = 1; continue;
            }
        }
        else if (screen == SCREEN_THEME_ADJUST) {
            // Screen 2/2: choose adjustments. Row 0 = Photo (locked on).
            const char *arows[4] = { "Photo", "Frame", "Shadow", "L/R Buttons" };
            if (redraw_all) {
                // No "Page 2/2" indicator when entered via "Create new variant"
                // (it's a one-screen flow there, not the 2-screen make-compatible).
                draw_chrome("Apply adjustments", adj_from_create ? NULL : "Page 2/2", 0);
                fb_text(LIST_X, LIST_Y - 2, "Select adjustments to apply:", COL_DIM);
                Hint h[2] = { { "START", 0, "Confirm", 0 }, { NULL, 'B', "Cancel", 0 } };
                draw_footer_hints(h, 2);
                redraw_all = 0; redraw_list = 1;
            }
            if (down & KEY_B) {
                // Cancel: from "Create new variant" go back to variant select;
                // from the normal flow go back to the intro screen.
                screen = adj_from_create ? SCREEN_THEME_APPLY : SCREEN_THEME_EXPLAIN;
                redraw_all = 1; continue;
            }
            if (rep & KEY_UP)   { if (adj_sel > 1) { adj_sel--; redraw_list = 1; } }
            if (rep & KEY_DOWN) { if (adj_sel < 3) { adj_sel++; redraw_list = 1; } }
            if (down & KEY_A) {            // toggle (photo row locked)
                if (adj_sel == 1) adj_frame  = !adj_frame;
                else if (adj_sel == 2) adj_shadow = !adj_shadow;
                else if (adj_sel == 3) adj_lr     = !adj_lr;
                redraw_list = 1;
            }
            if (redraw_list) {
                // Read the toggle state HERE (after any toggle above) so the
                // checkbox reflects the current press immediately.
                int achecked[4] = { 1, adj_frame, adj_shadow, adj_lr };
                int ry = LIST_Y + 14;
                fb_rect(0, ry - 2, ROW_W + 4, 4 * (ROW_H + 4) + 4, COL_BG);
                for (int r = 0; r < 4; r++) {
                    int y = ry + r * (ROW_H + 4);
                    int locked = (r == 0);
                    if (locked) {
                        fb_rect(LIST_X - 2, y - 1, ROW_W, ROW_H, rgb15(70, 70, 78));
                    } else if (r == adj_sel) {
                        fb_rect(LIST_X - 2, y - 1, ROW_W, ROW_H, COL_HILITE);
                    }
                    char line[40];
                    snprintf(line, sizeof(line), "[%c] %s", achecked[r] ? 'X' : ' ', arows[r]);
                    u16 col = locked ? rgb15(205,205,205)
                                     : (r == adj_sel ? COL_TEXT : COL_DIM);
                    fb_text(LIST_X, y, line, col);
                    if (locked) fb_text(LIST_X + ROW_W - 4 - (int)strlen("(required)")*6, y,
                                        "(required)", rgb15(205,205,205));
                }
                redraw_list = 0;
            }
            if (down & KEY_START) {
                const char *base = g_themes[theme_made_idx].name;
                char suffix[VARSUF_LEN];
                build_variant_suffix(adj_frame, adj_shadow, adj_lr, suffix, sizeof(suffix));

                // If this exact variant already exists, tell the user and stay.
                char existpath[600];
                snprintf(existpath, sizeof(existpath), "%s/%s%s", THEMES_DIR, base, suffix);
                struct stat exst;
                if (stat(existpath, &exst) == 0 && S_ISDIR(exst.st_mode)) {
                    // Popup: variant exists -- offer to select it instead.
                    fb_dim_rect(0, 0, SCR_W, SCR_H);
                    const int PW = 224, PH = 78;
                    int px = (SCR_W - PW)/2, py = (SCR_H - PH)/2;
                    fb_rect(px-1, py-1, PW+2, PH+2, rgb15(90,90,105));
                    fb_rect(px, py, PW, PH, COL_BG);
                    const char *m1 = "Variant already exists";
                    const char *m2 = "Select the existing variant?";
                    fb_text(px + (PW - (int)strlen(m1)*6)/2, py + 12, m1, COL_TEXT);
                    fb_text(px + (PW - (int)strlen(m2)*6)/2, py + 26, m2, COL_DIM);
                    int by = py + PH - 20, tyb = by + (BTN_D - GLYPH_H)/2;
                    int wY=(int)strlen("Yes")*6, wN=(int)strlen("No")*6;
                    int roww = BTN_D+4+wY+16+BTN_D+4+wN;
                    int hx = px + (PW-roww)/2;
                    hx += fb_button(hx, by, 'A', COL_BTN, COL_BG)+4; fb_text(hx,tyb,"Yes",COL_DIM); hx+=wY+16;
                    hx += fb_button(hx, by, 'B', COL_BTN, COL_BG)+4; fb_text(hx,tyb,"No",COL_DIM);
                    int k = wait_keys(KEY_A | KEY_B);
                    if (k & KEY_A) {
                        // Apply the existing variant, then go to variant select.
                        char folder[THEME_NAMELEN + VARSUF_LEN];
                        snprintf(folder, sizeof(folder), "%s%s", base, suffix);
                        twl_set_theme(folder);
                        // theme_made_idx already points at this base.
                        theme_apply_sel = 0;
                        screen = SCREEN_THEME_APPLY; redraw_all = 1; continue;
                    }
                    redraw_all = 1;   // No: restore the adjust screen
                    continue;
                }

                int has_adj = (adj_frame || adj_shadow || adj_lr);
                int nstages = has_adj ? 3 : 2;   // +1 stage only if adjustments

                // Screen 3: loading bar (Sync style: bar on top, text below). Each
                // stage label is shown WHILE its real work runs, and the bar fills
                // to that stage's portion only AFTER the work completes -- so the
                // bar tracks the actual operation rather than a fixed timer.
                draw_chrome("Theme Builder", NULL, 0);
                int barx = 10, bary = 34, barw = SCR_W - 20, barh = 12;
                fb_rect(barx, bary, barw, barh, rgb15(40, 40, 48));

                char dst[512]; int rc = 0;
                int filled = 0;                 // current fill width in pixels
                for (int s = 0; s < nstages; s++) {
                    char label[40];
                    if (s == 0)      snprintf(label, sizeof(label), "%d / %d  Copying theme", s + 1, nstages);
                    else if (s == 1) snprintf(label, sizeof(label), "%d / %d  Adding photo", s + 1, nstages);
                    else             snprintf(label, sizeof(label), "%d / %d  Adding adjustments", s + 1, nstages);
                    fb_rect(barx, bary + barh + 4, SCR_W - 2*barx, 10, COL_BG);
                    fb_text(barx, bary + barh + 4, label, COL_TEXT);
                    swiWaitForVBlank();

                    int start  = barw * s / nstages;          // this stage's start
                    int target = barw * (s + 1) / nstages;    // this stage's end

                    // We can't update the bar during the (blocking) work, so we
                    // split the animation: creep slowly across most of the stage's
                    // span BEFORE the work (so it's visibly moving), run the work,
                    // then glide the remainder to the boundary. The creep stops
                    // short of the end so completion always lands the bar exactly.
                    int creep_to = start + (target - start) * 4 / 5;
                    while (filled < creep_to) {
                        filled += (creep_to - filled + 11) / 12;   // slow ease
                        if (filled > creep_to) filled = creep_to;
                        fb_rect(barx, bary, filled, barh, rgb15(60, 150, 90));
                        swiWaitForVBlank();
                    }

                    if (s == 0)      rc = tv_copy(base, suffix, dst, sizeof(dst));
                    else if (s == 1) rc = tv_photo(dst);
                    else             rc = tv_adjust(dst, adj_frame, adj_shadow, adj_lr);
                    if (rc != 0) break;

                    // Finish this stage: glide to the boundary.
                    while (filled < target) {
                        filled += (target - filled + 4) / 5;
                        if (filled > target) filled = target;
                        fb_rect(barx, bary, filled, barh, rgb15(60, 150, 90));
                        swiWaitForVBlank();
                    }
                }

                fb_rect(barx, bary + barh + 4, SCR_W - 2*barx, 10, COL_BG);
                if (rc == 0) fb_text(barx, bary + barh + 4, "Done", rgb15(120, 200, 140));
                else         fb_text(barx, bary + barh + 4, "Copy failed.", COL_FAIL);
                for (int f = 0; f < (rc == 0 ? 40 : 60); f++) swiWaitForVBlank();

                build_theme_list();
                theme_made_idx = -1;
                for (int i = 0; i < g_n_themes; i++)
                    if (strcmp(g_themes[i].name, base) == 0) { theme_made_idx = i; break; }
                snprintf(theme_made_suffix, sizeof(theme_made_suffix), "%s", suffix);
                screen = SCREEN_THEME_SETPROMPT; redraw_all = 1;
                continue;
            }
        }
        else if (screen == SCREEN_THEME_APPLY) {
            // Variant select: list every photo option for this base theme, an
            // optional "No photo" (the plain original), and a "Create new variant"
            // action that re-enters the Theme Builder on the ORIGINAL files.
            ThemeEntry *t = &g_themes[theme_made_idx];

            // Build the row list. kind: 0 = apply variant folder, 1 = apply base
            // (no-photo original), 2 = create-new-variant action.
            char dir[600];
            snprintf(dir, sizeof(dir), "%s/%s", THEMES_DIR, t->name);
            int base_is_photo = (theme_render_photo(dir) == 1);

            static int  vk[MAX_VARIANTS + 4];      // row kind
            static char vfolder[MAX_VARIANTS + 4][THEME_NAMELEN + VARSUF_LEN];
            static char vlabel_s[MAX_VARIANTS + 4][40];
            int nrows = 0;

            // If the base theme is itself already photo-enabled, list it as a
            // plain "Photo" option (folder = base name, no suffix).
            if (base_is_photo) {
                vk[nrows] = 0;
                snprintf(vfolder[nrows], sizeof(vfolder[0]), "%s", t->name);
                snprintf(vlabel_s[nrows], sizeof(vlabel_s[0]), "Photo");
                nrows++;
            }
            // Each generated variant.
            for (int v = 0; v < t->nvariants; v++) {
                vk[nrows] = 0;
                snprintf(vfolder[nrows], sizeof(vfolder[0]), "%s%s", t->name, t->variant[v]);
                variant_label(t->variant[v], vlabel_s[nrows], sizeof(vlabel_s[0]));
                nrows++;
            }
            // "No photo" = the plain original (only meaningful if the base is not
            // itself photo-enabled).
            if (!base_is_photo) {
                vk[nrows] = 1;
                snprintf(vfolder[nrows], sizeof(vfolder[0]), "%s", t->name);
                snprintf(vlabel_s[nrows], sizeof(vlabel_s[0]), "No photo");
                nrows++;
            }
            // "Create new variant" action.
            vk[nrows] = 2;
            snprintf(vlabel_s[nrows], sizeof(vlabel_s[0]), "Create new variant");
            nrows++;

            if (theme_apply_sel >= nrows) theme_apply_sel = 0;
            // Which row (if any) is the currently-applied theme? Compare each
            // applyable folder against the live DSI_THEME value.
            char curtheme[THEME_NAMELEN]; int applied_row = -1;
            if (twl_get_theme(curtheme, sizeof(curtheme)) == 0) {
                for (int r = 0; r < nrows; r++)
                    if (vk[r] != 2 && strcmp(curtheme, vfolder[r]) == 0) { applied_row = r; break; }
            }
            if (redraw_all) {
                draw_chrome("Variant select", NULL, 1);
                Hint h[2] = { { NULL, 'A', "Select", 0 }, { NULL, 'B', "Back", 0 } };
                draw_footer_hints(h, 2);
                redraw_all = 0;
                draw_theme_preview(t->name);   // all share the base top.png
                redraw_list = 1;
            }
            if (down & KEY_B) { screen = SCREEN_THEMES; redraw_all = 1; continue; }
            if (rep & KEY_UP)   { if (theme_apply_sel > 0) { theme_apply_sel--; redraw_list = 1; } }
            if (rep & KEY_DOWN) { if (theme_apply_sel < nrows-1) { theme_apply_sel++; redraw_list = 1; } }
            if (down & KEY_A) {
                int kind = vk[theme_apply_sel];
                if (kind == 2) {
                    // Create new variant: re-enter the builder on the ORIGINAL theme,
                    // skipping the intro screen and showing Cancel / no page number.
                    adj_frame = adj_shadow = adj_lr = 0; adj_sel = 1;
                    adj_from_create = 1;
                    screen = SCREEN_THEME_ADJUST; redraw_all = 1; continue;
                }
                // Move the mark to this row and repaint immediately (before the
                // slower settings.ini write) so the X feels instant.
                applied_row = theme_apply_sel;
                for (int r = 0; r < nrows; r++) {
                    int is_sel = (r == theme_apply_sel);
                    u16 col = is_sel ? COL_TEXT : COL_DIM;
                    int y = LIST_Y + r * (ROW_H + 2);
                    if (vk[r] == 2) {
                        y += 2 * (ROW_H + 2);
                        if (is_sel) fb_rect(LIST_X - 2, y - 1, ROW_W, ROW_H, COL_HILITE);
                        int bx = fb_button(LIST_X, y - 1, '+', COL_BTN, is_sel ? COL_HILITE : COL_BG);
                        fb_text(LIST_X + bx + 4, y + (BTN_D - GLYPH_H) / 2 - 1, vlabel_s[r], col);
                    } else {
                        if (is_sel) fb_rect(LIST_X - 2, y - 1, ROW_W, ROW_H, COL_HILITE);
                        char line[48];
                        snprintf(line, sizeof(line), "[%c] %s", r == applied_row ? 'X' : ' ', vlabel_s[r]);
                        fb_text_clip(LIST_X, y, line, col, 26);
                    }
                }
                swiWaitForVBlank();   // push the moved mark to screen now

                // Message sits below the (down-shifted) Create new variant row.
                int msg_y = LIST_Y + (nrows + 2) * (ROW_H + 2) + 10;
                fb_rect(0, msg_y - 2, ROW_W + 4, 12, COL_BG);
                fb_text(LIST_X, msg_y, "Applying theme...", COL_TEXT);
                swiWaitForVBlank();
                int rc = twl_set_theme(vfolder[theme_apply_sel]);
                fb_rect(0, msg_y - 2, ROW_W + 4, 12, COL_BG);
                fb_text(LIST_X, msg_y, rc == 0 ? "Theme applied." : "Could not update settings.",
                        rc == 0 ? rgb15(120,200,140) : COL_FAIL);
                for (int f = 0; f < 150; f++) swiWaitForVBlank();   // ~2.5s
                fb_rect(0, msg_y - 2, ROW_W + 4, 12, COL_BG);       // then clear it
                redraw_list = 1;                                   // refresh marks
            }
            if (redraw_list) {
                fb_rect(0, LIST_BAND_Y, ROW_W + 4, LIST_BAND_H, COL_BG);
                fb_text(LIST_X, PAGE_Y, t->name, COL_HEADER);
                for (int r = 0; r < nrows; r++) {
                    int is_sel = (r == theme_apply_sel);
                    u16 col = is_sel ? COL_TEXT : COL_DIM;
                    int y = LIST_Y + r * (ROW_H + 2);
                    if (vk[r] == 2) {
                        // "Create new variant": left-aligned with the variant rows,
                        // pushed down ~2 rows for separation. The + glyph and the
                        // label are vertically centered on each other.
                        y += 2 * (ROW_H + 2);
                        if (is_sel) fb_rect(LIST_X - 2, y - 1, ROW_W, ROW_H, COL_HILITE);
                        int bx = fb_button(LIST_X, y - 1, '+', COL_BTN, is_sel ? COL_HILITE : COL_BG);
                        fb_text(LIST_X + bx + 4, y + (BTN_D - GLYPH_H) / 2 - 1, vlabel_s[r], col);
                    } else {
                        if (is_sel) fb_rect(LIST_X - 2, y - 1, ROW_W, ROW_H, COL_HILITE);
                        char line[48];
                        snprintf(line, sizeof(line), "[%c] %s",
                                 r == applied_row ? 'X' : ' ', vlabel_s[r]);
                        fb_text_clip(LIST_X, y, line, col, 26);
                    }
                }
                redraw_list = 0;
            }
        }
        else if (screen == SCREEN_THEME_SETPROMPT) {
            // After building: offer to set the new variant as the selected theme.
            ThemeEntry *t = (theme_made_idx >= 0) ? &g_themes[theme_made_idx] : NULL;
            if (!t) { screen = SCREEN_THEMES; redraw_all = 1; continue; }
            if (redraw_all) {
                fb_dim_rect(0, 0, SCR_W, SCR_H);
                const int PANEL_W = 224, PANEL_H = 50;   // tight: text + buttons centered
                int px = (SCR_W - PANEL_W) / 2, py = (SCR_H - PANEL_H) / 2;
                fb_rect(px - 1, py - 1, PANEL_W + 2, PANEL_H + 2, rgb15(90, 90, 105));
                fb_rect(px, py, PANEL_W, PANEL_H, COL_BG);
                const char *l1 = "Set as your selected theme?";
                fb_text(px + (PANEL_W - (int)strlen(l1)*6)/2, py + 11, l1, COL_TEXT);
                int by = py + 26;
                int tyb = by + (BTN_D - GLYPH_H) / 2;
                int wY = (int)strlen("Yes")*6, wN = (int)strlen("No")*6;
                int roww = BTN_D + 4 + wY + 16 + BTN_D + 4 + wN;
                int hx = px + (PANEL_W - roww)/2;
                hx += fb_button(hx, by, 'A', COL_BTN, COL_BG) + 4;
                fb_text(hx, tyb, "Yes", COL_DIM); hx += wY + 16;
                hx += fb_button(hx, by, 'B', COL_BTN, COL_BG) + 4;
                fb_text(hx, tyb, "No", COL_DIM);
                redraw_all = 0;
            }
            if (down & KEY_B) {
                theme_page = 0; theme_sel = 0; theme_top = 0;  // compatible list
                screen = SCREEN_THEMES; redraw_all = 1; continue;
            }
            if (down & KEY_A) {
                // Apply the variant we just built.
                char folder[THEME_NAMELEN + VARSUF_LEN];
                snprintf(folder, sizeof(folder), "%s%s", t->name, theme_made_suffix);
                int rc = twl_set_theme(folder);
                fb_dim_rect(0, 0, SCR_W, SCR_H);
                const int PW = 224, PH = 60;
                int px = (SCR_W - PW)/2, py = (SCR_H - PH)/2;
                fb_rect(px-1, py-1, PW+2, PH+2, rgb15(90,90,105));
                fb_rect(px, py, PW, PH, COL_BG);
                if (rc == 0) {
                    fb_text(px + (PW - (int)strlen("Theme applied!")*6)/2, py + 18, "Theme applied!", rgb15(120,200,140));
                    fb_text(px + (PW - (int)strlen("Theme Builder will now close.")*6)/2, py + 34, "Theme Builder will now close.", COL_DIM);
                    // Hold for ~2 seconds, then return to the themes screen.
                    for (int f = 0; f < 120; f++) swiWaitForVBlank();
                } else {
                    fb_text(px + (PW - (int)strlen("Could not update settings.")*6)/2, py + 26, "Could not update settings.", COL_FAIL);
                    for (int f = 0; f < 90; f++) swiWaitForVBlank();
                }
                theme_page = 0; theme_sel = 0; theme_top = 0;  // back to compatible list
                theme_want_current = 1;                        // highlight the applied theme
                screen = SCREEN_THEMES; redraw_all = 1;
                continue;
            }
        }
        else if (screen == SCREEN_CONFIRM_DELETE) {
            // Modal popup: the library view stays behind, dimmed, with a
            // centered panel showing the photo and the delete question.
            if (redraw_all) {
                // Dim the whole screen (the library render from the prior frame
                // is still in the framebuffer behind us).
                fb_dim_rect(0, 0, SCR_W, SCR_H);

                const int PANEL_W = 160, PANEL_H = 140;
                int px = (SCR_W - PANEL_W) / 2;
                int py = (SCR_H - PANEL_H) / 2;
                // Panel body + border.
                fb_rect(px - 1, py - 1, PANEL_W + 2, PANEL_H + 2, rgb15(90, 90, 105));
                fb_rect(px, py, PANEL_W, PANEL_H, COL_BG);

                // Photo preview at the top of the panel.
                const int MPV_W = 104, MPV_H = 78;
                int prev_x = px + (PANEL_W - MPV_W) / 2;
                int prev_y = py + 10;
                int idx = g_conv[sel];
                char outname[400];
                make_out_name(g_photos[idx].subdir, g_photos[idx].file,
                              outname, sizeof(outname));
                unsigned char *rgb = NULL; int iw = 0, ih = 0;
                if (read_png(outname, &rgb, &iw, &ih) == 0) {
                    fb_blit_rgb(rgb, iw, ih, prev_x, prev_y, MPV_W, MPV_H);
                    free(rgb);
                } else {
                    fb_rect(prev_x, prev_y, MPV_W, MPV_H, rgb15(24, 24, 28));
                }
                // Thin frame around the preview.
                fb_rect(prev_x - 1, prev_y - 1, MPV_W + 2, 1, rgb15(70,70,80));
                fb_rect(prev_x - 1, prev_y + MPV_H, MPV_W + 2, 1, rgb15(70,70,80));
                fb_rect(prev_x - 1, prev_y - 1, 1, MPV_H + 2, rgb15(70,70,80));
                fb_rect(prev_x + MPV_W, prev_y - 1, 1, MPV_H + 2, rgb15(70,70,80));

                // Question + filename, centered under the preview.
                char name[40];
                snprintf(name, sizeof(name), "%s", g_photos[idx].file);
                char *dot = strrchr(name, '.'); if (dot) *dot = '\0';
                char fn[48];
                snprintf(fn, sizeof(fn), "%s.png", name);
                const char *q = "Remove this photo?";
                int qx = px + (PANEL_W - (int)strlen(q) * 6) / 2;
                int fx = px + (PANEL_W - (int)strlen(fn) * 6) / 2;
                int ty = prev_y + MPV_H + 9;
                fb_text(qx, ty, q, COL_TEXT);
                fb_text(fx, ty + 13, fn, COL_DIM);

                // A Confirm / B Cancel, centered near the panel bottom.
                int by = py + PANEL_H - 17;
                int tyb = by + (BTN_D - GLYPH_H) / 2;
                int wConfirm = (int)strlen("Confirm") * 6;
                int wCancel  = (int)strlen("Cancel") * 6;
                int roww = BTN_D + 4 + wConfirm + 16 + BTN_D + 4 + wCancel;
                int hx = px + (PANEL_W - roww) / 2;
                hx += fb_button(hx, by, 'A', COL_BTN, COL_BG) + 4;
                fb_text(hx, tyb, "Confirm", COL_DIM); hx += wConfirm + 16;
                hx += fb_button(hx, by, 'B', COL_BTN, COL_BG) + 4;
                fb_text(hx, tyb, "Cancel", COL_DIM);
                redraw_all = 0;
            }
            if (down & KEY_A) {                  // confirm delete
                int idx = g_conv[sel];
                char outname[400];
                make_out_name(g_photos[idx].subdir, g_photos[idx].file,
                              outname, sizeof(outname));
                if (remove(outname) == 0) { g_photos[idx].done = 0; g_photos[idx].convert = 0; }
                thumb_invalidate(idx);            // only the removed photo
                repartition();
                if (sel >= g_n_conv) sel = g_n_conv > 0 ? g_n_conv - 1 : 0;
                top = (sel / (view_grid ? GRID_CELLS : VIS_ROWS))
                      * (view_grid ? GRID_CELLS : VIS_ROWS);
                last_preview_sel = -2;
                screen = SCREEN_LIBRARY; redraw_all = 1;
                continue;
            }
            if (down & KEY_B) {                  // cancel: nothing changed
                last_preview_sel = -2;
                screen = SCREEN_LIBRARY; redraw_all = 1;
                continue;
            }
        }
        else if (screen == SCREEN_CONFIRM_REMOVE_ALL) {
            // Modal popup over the dimmed gallery: confirm removing everything.
            if (redraw_all) {
                fb_dim_rect(0, 0, SCR_W, SCR_H);
                const int PANEL_W = 220, PANEL_H = 108;
                int px = (SCR_W - PANEL_W) / 2;
                int py = (SCR_H - PANEL_H) / 2;
                fb_rect(px - 1, py - 1, PANEL_W + 2, PANEL_H + 2, rgb15(90, 90, 105));
                fb_rect(px, py, PANEL_W, PANEL_H, COL_BG);

                char q[40];
                snprintf(q, sizeof(q), "Remove all %d photos?", g_n_conv);
                int qx = px + (PANEL_W - (int)strlen(q) * 6) / 2;
                fb_text(qx, py + 14, q, COL_TEXT);

                // Reassuring note, wrapped to the panel width.
                const char *lines[3] = {
                    "Photos can be added back to",
                    "the gallery if the original .jpg",
                    "files still exist on the SD card."
                };
                for (int i = 0; i < 3; i++) {
                    int lx = px + (PANEL_W - (int)strlen(lines[i]) * 6) / 2;
                    fb_text(lx, py + 32 + i * 12, lines[i], COL_DIM);
                }

                // Center the A Confirm / B Cancel row as a unit.
                int by = py + PANEL_H - 21;
                int tyb = by + (BTN_D - GLYPH_H) / 2;
                int wConfirm = (int)strlen("Confirm") * 6;
                int wCancel  = (int)strlen("Cancel") * 6;
                int roww = BTN_D + 4 + wConfirm + 16 + BTN_D + 4 + wCancel;
                int hx = px + (PANEL_W - roww) / 2;
                hx += fb_button(hx, by, 'A', COL_BTN, COL_BG) + 4;
                fb_text(hx, tyb, "Confirm", COL_DIM); hx += wConfirm + 16;
                hx += fb_button(hx, by, 'B', COL_BTN, COL_BG) + 4;
                fb_text(hx, tyb, "Cancel", COL_DIM);
                redraw_all = 0;
            }
            if (down & KEY_A) {                  // confirm: remove every photo
                for (int j = 0; j < g_n_conv; j++) {
                    int idx = g_conv[j];
                    char outname[400];
                    make_out_name(g_photos[idx].subdir, g_photos[idx].file,
                                  outname, sizeof(outname));
                    if (remove(outname) == 0) { g_photos[idx].done = 0; g_photos[idx].convert = 0; }
                }
                repartition();
                sel = 0; top = 0;
                grid_free(); last_preview_sel = -2;
                screen = SCREEN_LIBRARY; redraw_all = 1;
                continue;
            }
            if (down & KEY_B) {                  // cancel: nothing changed
                last_preview_sel = -2;
                screen = SCREEN_LIBRARY; redraw_all = 1;
                continue;
            }
        }
    }

    preview_free();
    return 0;
}
