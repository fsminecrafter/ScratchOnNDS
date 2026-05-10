/*
lodepng.h — Minimal PNG decode-only header for ScratchDS / NDS ARM9.

This is a stripped-down interface over the upstream LodePNG library
(https://github.com/lvandeve/lodepng).  Only the decode path is compiled;
encode, zlib-compress, and inspect APIs are excluded via the defines below
to save ~30 KB of ROM and avoid pulling in unused code on a 67 MHz ARM9.

Usage (identical to upstream for the decode path):
    #include "lodepng.h"

    unsigned char* pixels = nullptr;
    unsigned w = 0, h = 0;
    unsigned err = lodepng_decode32_file(&pixels, &w, &h, "fat:/scratch/.tmp/abc.png");
    if (err) { free(pixels); return false; }
    // pixels = RGBA8888, w*h*4 bytes, top-row first
    free(pixels);

NDS-specific notes:
  - Output is always RGBA8888 (use rgb555() helper below to convert).
  - The decoder mallocs a single contiguous buffer; free() it when done.
  - Decompressing a 64×64 sprite costs ~20 KB heap + a brief 16 KB zlib
    window; a 256×192 backdrop costs ~200 KB.  Call free() promptly.
  - Do NOT decode multiple large PNGs simultaneously; the NDS has 4 MB RAM.
  - Thread safety: none (no threads on bare-metal NDS anyway).

Compile-time knobs (set in your Makefile CXXFLAGS or before the #include):
    LODEPNG_NO_COMPILE_ENCODER   — omit encoder (set below, always)
    LODEPNG_NO_COMPILE_DISK      — omit FILE* helpers (we keep them)
    LODEPNG_NO_COMPILE_ANCILLARY_CHUNKS — skip tEXt/zTXt/etc metadata

Convenience helpers added here (not in upstream):
    u16  lodepng_rgb555(u8 r, u8 g, u8 b);
    bool lodepng_to_rgb555(const u8* rgba, unsigned w, unsigned h,
                           u16* dst, unsigned dstStride);
*/

#pragma once

/* ── Strip unused upstream features to save code size ── */
#ifndef LODEPNG_NO_COMPILE_ENCODER
#define LODEPNG_NO_COMPILE_ENCODER
#endif
#ifndef LODEPNG_NO_COMPILE_ANCILLARY_CHUNKS
#define LODEPNG_NO_COMPILE_ANCILLARY_CHUNKS
#endif

/* ── Pull in the real upstream header ── */
/*
   The actual LodePNG implementation is in lodepng_impl.h / lodepng.cpp.
   We keep this thin wrapper so the rest of the codebase only includes
   "lodepng.h" and never has to touch the upstream files directly.
*/

#include <stdint.h>
#include <stddef.h>

typedef uint8_t  u8;
typedef uint16_t u16;

#ifdef __cplusplus
extern "C" {
#endif

/* ── Core decode API (subset of upstream LodePNG) ── */

/*
 * Decode a PNG file from disk into a freshly malloc'd RGBA8888 buffer.
 * Returns 0 on success, non-zero LodePNG error code on failure.
 * *out is set to NULL on failure.
 * The caller must free(*out).
 */
unsigned lodepng_decode32_file(unsigned char** out,
                               unsigned* w, unsigned* h,
                               const char* filename);

/*
 * Decode a PNG from an in-memory buffer.
 * Returns 0 on success.  The caller must free(*out).
 */
unsigned lodepng_decode32(unsigned char** out,
                          unsigned* w, unsigned* h,
                          const unsigned char* in, size_t insize);

/*
 * Human-readable error string for a LodePNG error code.
 */
const char* lodepng_error_text(unsigned code);

/* ── NDS convenience helpers ── */

/*
 * Pack an 8-bit-per-channel RGB triplet into NDS RGB555 format.
 * The top bit is always 0 (transparent flag handled separately).
 */
static inline u16 lodepng_rgb555(u8 r, u8 g, u8 b) {
    return (u16)(((u16)(r >> 3))
               | ((u16)(g >> 3) << 5)
               | ((u16)(b >> 3) << 10));
}

/*
 * Convert an RGBA8888 buffer (as returned by lodepng_decode32 / _file)
 * into a flat array of RGB555 u16 values in 'dst'.
 *
 * Pixels with alpha < 128 are written as 0 (transparent / palette index 0).
 * dstStride is the number of u16 values per row in dst (normally == w).
 *
 * Returns true on success, false if any pointer is NULL.
 *
 * Example — blit a decoded sprite straight to an NDS framebuffer:
 *
 *   unsigned char* px; unsigned pw, ph;
 *   if (lodepng_decode32_file(&px, &pw, &ph, path) == 0) {
 *       u16 buf[64*64];
 *       lodepng_to_rgb555(px, pw, ph, buf, pw);
 *       free(px);
 *       dmaCopy(buf, oamAllocateGfx(...), pw * ph * 2);
 *   }
 */
static inline int lodepng_to_rgb555(const u8* rgba,
                                     unsigned w, unsigned h,
                                     u16* dst, unsigned dstStride) {
    if (!rgba || !dst || w == 0 || h == 0) return 0;
    for (unsigned y = 0; y < h; y++) {
        const u8* row = rgba + y * w * 4;
        u16*      out = dst  + y * dstStride;
        for (unsigned x = 0; x < w; x++) {
            u8 r = row[x*4+0];
            u8 g = row[x*4+1];
            u8 b = row[x*4+2];
            u8 a = row[x*4+3];
            out[x] = (a < 128) ? 0u : lodepng_rgb555(r, g, b);
        }
    }
    return 1;
}

#ifdef __cplusplus
}
#endif
