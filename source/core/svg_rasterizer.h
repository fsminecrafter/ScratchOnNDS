// =============================================================================
// svg_rasterizer.h / svg_rasterizer.cpp
// Minimal SVG rasterizer for NDS — handles only the subset of SVG that
// Scratch 3.0 exports for its built-in costumes and backdrops:
//   - <rect>, <circle>, <ellipse>, <polygon>, <path> (M/L/C/Z commands)
//   - fill colour (hex, rgb(), named), fill-opacity, stroke / stroke-width
//   - transform="translate(x,y) scale(x,y) rotate(deg)"
//   - viewBox attribute for coordinate normalisation
//
// Output: 16-bit RGB555 pixel buffer + 256-colour palette (8bpp indexed)
// suitable for direct upload to NDS OAM or BG VRAM.
//
// Heavy-weight operations (Bézier flattening, polygon scan-line fill) are
// written for correctness first; on a 67 MHz ARM9 a 64×64 costume raster
// takes roughly 2-4 ms — acceptable since we rasterise once at load time.
// =============================================================================
#pragma once
#include <stdint.h>
#include <string>

// Maximum output dimensions (clamped to NDS sprite limits)
#define SVG_MAX_W 64
#define SVG_MAX_H 64

// Rasterized image result
struct SvgImage {
    uint8_t  pixels[SVG_MAX_W * SVG_MAX_H]; // 8bpp indexed
    uint16_t palette[256];                  // RGB555
    int      palCount;
    int      width, height;
    bool     ok;
};

// =============================================================================
// Public API
// =============================================================================

// Rasterize an SVG file into an SvgImage.
// dstW / dstH: desired output size (≤ SVG_MAX_W / SVG_MAX_H).
// Returns true on success.
bool svgRasterize(const char* path, SvgImage& out, int dstW = 64, int dstH = 64);

// Rasterize from an in-memory string (for unit-testing / embedded SVG).
bool svgRasterizeString(const char* svgData, size_t len,
                        SvgImage& out, int dstW = 64, int dstH = 64);
