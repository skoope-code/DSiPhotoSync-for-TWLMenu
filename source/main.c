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
//   - framebuffer primitives (pixels, rects, blit, text, button/pill glyphs)
//   - photo list build + single-photo conversion
//   - preview cache (settle-gated)
//   - UI: colors, chrome, list rows, footer hints, screens
//   - settings (load/save, apply UI screen)
//   - shared list-screen helpers (navigate / preview / return-to-menu)
//   - main(): video setup, then the screen state machine

#include <nds.h>
#include <fat.h>
#include <filesystem.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

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
                    for (int y = 0; y < 8; y++) {
                        int py = my * mcuH + by + y;
                        if (py >= H) break;
                        for (int x = 0; x < 8; x++) {
                            int px = mx * mcuW + bx + x;
                            if (px >= W) continue;
                            int s = src + y * 8 + x;
                            unsigned char *d = rgb + ((size_t)py * W + px) * 3;
                            d[0] = info.m_pMCUBufR[s];
                            d[1] = info.m_pMCUBufG[s];
                            d[2] = info.m_pMCUBufB[s];
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

static u16 *g_fb = NULL;   // points at the active VRAM framebuffer

static inline u16 rgb15(unsigned char r, unsigned char g, unsigned char b) {
    return 0x8000 | ((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3);
}

static inline void fb_px(int x, int y, u16 c) {
    if ((unsigned)x < SCR_W && (unsigned)y < SCR_H) g_fb[y * SCR_W + x] = c;
}

static void fb_rect(int x, int y, int w, int h, u16 c) {
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++)
            fb_px(x + xx, y + yy, c);
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
// char drawn as a round face button.
typedef struct { const char *pill; char btn; const char *label; } Hint;

#define FOOTER_H (BTN_D + 6)             // footer band height
static void draw_footer_hints(const Hint *hints, int count) {
    int fy = SCR_H - FOOTER_H;
    fb_rect(0, fy - 1, SCR_W, 1, COL_DIVIDER);
    fb_rect(0, fy, SCR_W, FOOTER_H, COL_BG);   // clear band
    int x = 8;
    int by = fy + 3;                     // button top
    int ty = by + (BTN_D - GLYPH_H) / 2; // label ink aligned to glyph ink
    for (int i = 0; i < count; i++) {
        if (hints[i].pill)
            x += fb_pill(x, by, hints[i].pill, COL_BTN, COL_BG);
        else
            x += fb_button(x, by, hints[i].btn, COL_BTN, COL_BG);
        x += 4;
        fb_text(x, ty, hints[i].label, COL_DIM);
        x += (int)strlen(hints[i].label) * 6 + 10;
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
typedef enum { MODE_ADD, MODE_REMOVE, MODE_LIBRARY } ListMode;

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
        if (mode == MODE_LIBRARY)
            snprintf(line, sizeof(line), "%s", p->file);
        else
            snprintf(line, sizeof(line), "[%c] %s", p->convert ? 'x' : ' ', p->file);
        fb_text_clip(LIST_X, y, line, is_sel ? COL_TEXT : COL_DIM, 26);
    }

    // Below the box: "<pos> OF <total>" on the left, the cap on the right.
    // Caps (not lowercase "of") keep the left text full-height so it sits level
    // with "Max:" on the right — lowercase letters sit lower and read misaligned.
    char posbuf[32];
    snprintf(posbuf, sizeof(posbuf), "%d OF %d", sel + 1, n);
    fb_text(LIST_X, INFO_Y, posbuf, COL_DIM);

    char maxbuf[24];
    snprintf(maxbuf, sizeof(maxbuf), "Max: %d", MAX_PHOTOS);
    int tw = (int)strlen(maxbuf) * 6;
    fb_text(LIST_X + ROW_W - 4 - tw, INFO_Y, maxbuf, g_hit_max ? COL_FAIL : COL_DIM);
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
    draw_chrome("Converting", NULL, 0);
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
static int run_removal(int *ids, int n) {
    int removed = 0;
    for (int i = 0; i < n; i++) {
        PhotoEntry *p = &g_photos[ids[i]];
        if (!p->convert) continue;   // 'convert' flag doubles as the checkbox
        char outname[400];
        make_out_name(p->subdir, p->file, outname, sizeof(outname));
        if (remove(outname) == 0) { p->done = 0; p->convert = 0; removed++; }
    }
    return removed;
}

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
    Hint h[1] = { { "START", 0, "Exit" } };
    draw_footer_hints(h, 1);
    if (l1) fb_text(8, 40, l1, COL_TEXT);
    if (l2) fb_text(8, 54, l2, COL_DIM);
    if (l3) fb_text(8, 68, l3, COL_DIM);
}

// ---- Root menu band repaint.
static void redraw_menu(int sel, int n_unconv, int n_conv) {
    fb_rect(0, LIST_BAND_Y, SCR_W, LIST_BAND_H, COL_BG);   // clear menu band
    char l0[32], l1[32], l2[32];
    snprintf(l0, sizeof(l0), "Add photos (%d new)", n_unconv);
    snprintf(l1, sizeof(l1), "Remove photos (%d)", n_conv);
    snprintf(l2, sizeof(l2), "View library (%d)", n_conv);
    const char *labels[4] = { l0, l1, l2, "Settings" };
    for (int i = 0; i < 4; i++) {
        int y = LIST_Y + i * (ROW_H + 6);
        if (i == sel) fb_rect(LIST_X - 2, y - 1, ROW_W, ROW_H, COL_HILITE);
        fb_text(LIST_X, y, labels[i], i == sel ? COL_TEXT : COL_DIM);
    }
}

#define MENU_ITEMS 4

// Persistent settings (full load/save defined later, near main()).
#define SETTINGS_PATH "sd:/_nds/TWiLightMenu/dsimenu/dsiphotosync.ini"
typedef struct {
    int ui_bottom;   // 1 = UI on bottom physical screen, 0 = top
} Settings;
static Settings g_settings = { 1 };   // default: bottom screen

// ---- Settings screen band repaint. One option for now: UI screen choice.
static void redraw_settings(int sel) {
    fb_rect(0, LIST_BAND_Y, SCR_W, LIST_BAND_H, COL_BG);
    char l0[40];
    snprintf(l0, sizeof(l0), "Display:  < %s >",
             g_settings.ui_bottom ? "Top" : "Bottom");
    const char *labels[1] = { l0 };
    for (int i = 0; i < 1; i++) {
        int y = LIST_Y + i * (ROW_H + 6);
        if (i == sel) fb_rect(LIST_X - 2, y - 1, ROW_W, ROW_H, COL_HILITE);
        fb_text(LIST_X, y, labels[i], i == sel ? COL_TEXT : COL_DIM);
    }
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
        if (sscanf(line, "ui_bottom=%d", &v) == 1) g_settings.ui_bottom = v ? 1 : 0;
    }
    fclose(f);
}

static void settings_save(void) {
    FILE *f = fopen(SETTINGS_PATH, "w");
    if (!f) return;
    fprintf(f, "ui_bottom=%d\n", g_settings.ui_bottom);
    fclose(f);
}

// Apply the chosen physical screen for the UI. The main graphics engine drives
// the bottom screen when POWER_SWAP_LCDS is set, the top screen when cleared.
static void apply_ui_screen(void) {
    if (g_settings.ui_bottom) REG_POWERCNT |=  POWER_SWAP_LCDS;
    else                      REG_POWERCNT &= ~POWER_SWAP_LCDS;
}

// ===========================================================================
// Main: framebuffer video on the bottom screen + UI state machine.
// ===========================================================================
typedef enum { SCREEN_MENU, SCREEN_ADD, SCREEN_REMOVE, SCREEN_LIBRARY,
               SCREEN_EXPAND, SCREEN_SETTINGS, SCREEN_CONVERT, SCREEN_REMOVING,
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

static void enter_menu_chrome(void) {
    draw_chrome("DSiPhotoSync", NULL, 0);
    Hint h[3] = { { NULL, 'A', "Select" },
                  { "START", 0, "Sync" },
                  { "SELECT", 0, "Quit" } };
    draw_footer_hints(h, 3);
}

// ---------------------------------------------------------------------------
// Shared list-screen behavior, used identically by Add / Remove / Library.
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

// Once the selection has settled for a few frames, (re)load the hover preview
// for the current row. `ids` maps row -> photo index; `mode` picks the decoder
// (Add previews the source JPEG, Remove/Library preview the converted PNG).
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
    // Set up the main (graphics) engine and its framebuffer. Which physical
    // screen it drives is applied from settings once SD is readable (below);
    // until then it defaults to the bottom screen.
    videoSetMode(MODE_5_2D);
    videoSetModeSub(MODE_0_2D);
    vramSetBankA(VRAM_A_MAIN_BG_0x06000000);
    REG_POWERCNT |= POWER_SWAP_LCDS;   // default: main engine on bottom screen
    int bg = bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
    g_fb = (u16 *)bgGetGfxPtr(bg);
    setBackdropColor(0x0000);       // main screen backdrop black
    setBackdropColorSub(0x0000);    // other screen black

    // Clear the framebuffer to the UI background immediately. bgInit hands us a
    // VRAM buffer that still holds whatever was there before (leftovers from
    // TWiLight or the previous app), and the screen is already displaying it.
    // Without this, that garbage shows for the whole SD-mount delay below.
    fb_rect(0, 0, SCR_W, SCR_H, COL_BG);
    swiWaitForVBlank();             // make sure the cleared frame is shown first

    if (!fatInitDefault()) {
        // No SD: keep default screen, show the error.
        message_screen("SD card init failed.", "Is this reading the right SD?", NULL);
        wait_keys(KEY_START);
        return 0;
    }

    // SD is up: load the saved UI-screen preference and apply it before drawing.
    settings_load();
    apply_ui_screen();

    fb_rect(0, 0, SCR_W, SCR_H, COL_BG);
    fb_text(8, 8, "DSiPhotoSync", COL_HEADER);
    fb_text(8, 24, "Starting...", COL_DIM);

    mkdir("sd:/_nds", 0777);
    mkdir("sd:/_nds/TWiLightMenu", 0777);
    mkdir("sd:/_nds/TWiLightMenu/dsimenu", 0777);
    mkdir(PHOTOS_DIR, 0777);

    build_list();
    repartition();

    if (g_photo_count == 0) {
        message_screen("No photos found in sd:/DCIM/.",
                       "Take a photo to SD first,", "then run this again.");
        wait_keys(KEY_START | KEY_SELECT);
        return 0;
    }

    Screen screen = SCREEN_MENU;
    int menu_sel = 0;
    int sel = 0, top = 0;            // active list selection + scroll
    int settle = 0, last_preview_sel = -2;
    int redraw_all = 0;             // full chrome redraw needed
    int redraw_list = 1;            // list/menu band needs repaint
    int redraw_prev = 0;            // preview area needs repaint
    Screen expand_from = SCREEN_LIBRARY;   // screen to return to when collapsing

    enter_menu_chrome();

    while (screen != SCREEN_EXIT) {
        swiWaitForVBlank();
        scanKeys();
        int down = keysDown();

        if (screen == SCREEN_MENU) {
            if (down & KEY_SELECT) { screen = SCREEN_EXIT; continue; }
            if (down & KEY_UP)   { if (menu_sel > 0) { menu_sel--; redraw_list = 1; } }
            if (down & KEY_DOWN) { if (menu_sel < MENU_ITEMS - 1) { menu_sel++; redraw_list = 1; } }
            if (down & KEY_A) {
                sel = 0; top = 0; settle = 0; last_preview_sel = -2;
                if (menu_sel == 0) {            // Add: default all on
                    for (int i = 0; i < g_n_unconv; i++) g_photos[g_unconv[i]].convert = 1;
                    screen = SCREEN_ADD;
                } else if (menu_sel == 1) {     // Remove: default all off
                    for (int i = 0; i < g_n_conv; i++) g_photos[g_conv[i]].convert = 0;
                    screen = SCREEN_REMOVE;
                } else if (menu_sel == 2) {     // Library
                    screen = SCREEN_LIBRARY;
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
        }
        else if (screen == SCREEN_ADD || screen == SCREEN_REMOVE) {
            int *ids   = (screen == SCREEN_ADD) ? g_unconv : g_conv;
            int n      = (screen == SCREEN_ADD) ? g_n_unconv : g_n_conv;
            ListMode m = (screen == SCREEN_ADD) ? MODE_ADD : MODE_REMOVE;
            const char *empty = (screen == SCREEN_ADD)
                ? "Nothing new to convert." : "No converted photos.";

            if (redraw_all) {
                const char *title = (screen == SCREEN_ADD) ? "Add photos" : "Remove photos";
                draw_chrome(title, NULL, 1);
                Hint h[3] = { { NULL, 'A', "Toggle" }, { NULL, 'X', "Expand" },
                              { "START", 0, "Confirm" } };
                draw_footer_hints(h, 3);
                redraw_all = 0; redraw_list = 1; redraw_prev = 1;
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
                // If any are currently on, turn all off; otherwise turn all on.
                int any_on = 0;
                for (int j = 0; j < n; j++) if (g_photos[ids[j]].convert) { any_on = 1; break; }
                int v = any_on ? 0 : 1;
                for (int j = 0; j < n; j++) g_photos[ids[j]].convert = v;
                redraw_list = 1;
            }
            if (down & KEY_A) {                       // tap A: toggle current row
                if (sel < n) { g_photos[ids[sel]].convert ^= 1; redraw_list = 1; }
            }
            if ((down & KEY_START) && n > 0) {
                screen = (screen == SCREEN_ADD) ? SCREEN_CONVERT : SCREEN_REMOVING;
                continue;
            }

            // Navigation. If A is HELD while moving, paint-toggle each row we
            // enter (drag-select); the row we land on gets flipped.
            int held_a = (keysHeld() & KEY_A) != 0;
            int prev_sel = sel;
            list_navigate(&sel, &top, &settle, &redraw_list, down, n);
            if (held_a && sel != prev_sel && sel < n) {
                g_photos[ids[sel]].convert ^= 1;
                redraw_list = 1;
            }

            list_update_preview(&last_preview_sel, &settle, &redraw_prev, sel, n, ids, m);

            if (redraw_list) { redraw_rows(ids, n, sel, top, m, empty); redraw_list = 0; }
            if (redraw_prev) { redraw_preview(); redraw_prev = 0; }
        }
        else if (screen == SCREEN_LIBRARY) {
            int n = g_n_conv;
            if (redraw_all) {
                char hdr[32]; snprintf(hdr, sizeof(hdr), "%d", n);
                draw_chrome("Library", hdr, 1);
                if (n > 0) {
                    Hint h[1] = { { NULL, 'A', "Expand / Collapse" } };
                    draw_footer_hints(h, 1);
                }
                redraw_all = 0; redraw_list = 1; redraw_prev = 1;
            }
            if (down & KEY_B) {
                go_to_menu(&screen, &menu_sel, &last_preview_sel, &redraw_list, 0);
                continue;
            }
            if ((down & KEY_A) && n > 0) {
                expand_from = SCREEN_LIBRARY;
                screen = SCREEN_EXPAND; redraw_all = 1;
                continue;
            }

            list_navigate(&sel, &top, &settle, &redraw_list, down, n);
            list_update_preview(&last_preview_sel, &settle, &redraw_prev, sel, n,
                                g_conv, MODE_LIBRARY);

            if (redraw_list) { redraw_rows(g_conv, n, sel, top, MODE_LIBRARY,
                                           "No converted photos yet."); redraw_list = 0; }
            if (redraw_prev) { redraw_preview(); redraw_prev = 0; }
        }
        else if (screen == SCREEN_EXPAND) {
            if (redraw_all) {
                fb_rect(0, 0, SCR_W, SCR_H, COL_BG);
                unsigned char *rgb = NULL; int iw = 0, ih = 0;
                int ok = 0;
                if (expand_from == SCREEN_ADD) {
                    // Unconverted source photo: decode the JPEG (reduce mode is
                    // plenty for a fullscreen DSi-photo preview) and scale it up.
                    int idx = g_unconv[sel];
                    char src[400];
                    make_src_path(g_photos[idx].subdir, g_photos[idx].file,
                                  src, sizeof(src));
                    ok = (decode_jpeg_preview(src, &rgb, &iw, &ih) == 0);
                } else {
                    // Converted photo (Library or Remove): blit its PNG.
                    int idx = g_conv[sel];
                    char outname[400];
                    make_out_name(g_photos[idx].subdir, g_photos[idx].file,
                                  outname, sizeof(outname));
                    ok = (read_png(outname, &rgb, &iw, &ih) == 0);
                }
                if (ok) {
                    fb_blit_rgb(rgb, iw, ih, 0, 0, SCR_W, SCR_H);
                    free(rgb);
                } else {
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
            if (redraw_all) {
                draw_chrome("Settings", NULL, 0);
                Hint h[1] = { { "START", 0, "Save" } };
                draw_footer_hints(h, 1);
                redraw_all = 0; redraw_list = 1;
            }
            if (down & KEY_START) {
                settings_save();
                go_to_menu(&screen, &menu_sel, &last_preview_sel, &redraw_list, 3);
                continue;
            }
            // Only one option for now; Left/Right toggles the UI screen.
            if (down & (KEY_LEFT | KEY_RIGHT)) {
                if (sel == 0) {
                    g_settings.ui_bottom = !g_settings.ui_bottom;
                    apply_ui_screen();     // take effect immediately
                    redraw_list = 1;
                }
            }
            if (redraw_list) { redraw_settings(sel); redraw_list = 0; }
        }
        else if (screen == SCREEN_CONVERT) {
            preview_free();
            run_conversion(g_unconv, g_n_unconv);
            { Hint h[2] = { { "START", 0, "Menu" }, { "SELECT", 0, "Quit" } };
              draw_footer_hints(h, 2); }
            int k = wait_keys(KEY_START | KEY_SELECT);
            if (k & KEY_SELECT) { screen = SCREEN_EXIT; continue; }
            repartition();
            go_to_menu(&screen, &menu_sel, &last_preview_sel, &redraw_list, 0);
        }
        else if (screen == SCREEN_REMOVING) {
            preview_free();
            draw_chrome("Remove photos", NULL, 0);
            int removed = run_removal(g_conv, g_n_conv);
            char msg[40];
            snprintf(msg, sizeof(msg), "Removed %d photo(s).", removed);
            fb_text(8, 40, msg, COL_OK);
            { Hint h[2] = { { "START", 0, "Menu" }, { "SELECT", 0, "Quit" } };
              draw_footer_hints(h, 2); }
            int k = wait_keys(KEY_START | KEY_SELECT);
            if (k & KEY_SELECT) { screen = SCREEN_EXIT; continue; }
            repartition();
            go_to_menu(&screen, &menu_sel, &last_preview_sel, &redraw_list, 0);
        }
    }

    preview_free();
    return 0;
}
