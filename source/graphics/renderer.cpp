// =============================================================================
// renderer.cpp — Fixed BMP loading, backdrop detection, screen management
// =============================================================================
#include "renderer.h"
#include "../core/svg_rasterizer.h"
#include "../input/input_handler.h"
#include <nds.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <algorithm>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// -----------------------------------------------------------------------
// BG tile setup for backdrop rendering
// NDS BG in bitmap mode: we use BG2 in 256×192 8bpp bitmap mode (Mode 5).
// -----------------------------------------------------------------------
#define BG_BITMAP_VRAM ((uint8_t*)0x06000000)  // VRAM bank A mapped as main BG
#define BG_PAL_MAIN    ((uint16_t*)0x05000000)  // BG palette RAM (256 entries)

// -----------------------------------------------------------------------
// Init renderer state
// -----------------------------------------------------------------------
void Renderer::init() {
    memset(oamUsed, 0, sizeof(oamUsed));
    memset(gfxUsed, 0, sizeof(gfxUsed));
    nextOam   = 0;
    bgHandle  = -1;
    bgGfxPtr  = nullptr;
    bgPalPtr  = nullptr;
    backdropLoaded = false;

    oamInit(&oamMain, SpriteMapping_1D_32, false);

    // BG3 for backdrop (8bpp bitmap 256x256, map at slot 0, tiles at slot 0)
    bgHandle = bgInit(3, BgType_Bmp8, BgSize_B8_256x256, 0, 0);

    consoleInit(&bottomConsole, 0, BgType_Text4bpp, BgSize_T_256x256,
                2, 0, false, true);

    // Clear backdrop with dark grey
    memset(BG_BITMAP_VRAM, 0, 256 * 192);
}

// -----------------------------------------------------------------------
// Clear the bottom screen console fully
// Call this before writing new content to avoid ghost text
// -----------------------------------------------------------------------
void Renderer::clearBottomConsole() {
    consoleSelect(&bottomConsole);
    consoleClear();
}

// -----------------------------------------------------------------------
// Load all project costume assets
// -----------------------------------------------------------------------
void Renderer::loadSprites(ScratchProject& project) {
    for (auto& sprite : project.targets) {
        for (auto& costume : sprite.costumes) {
            loadCostume(costume, project.extractDir.c_str(), costume.dataFormat);
        }
    }
}

// -----------------------------------------------------------------------
// Load a single costume image into VRAM
// -----------------------------------------------------------------------
void Renderer::loadCostume(ScratchCostume& costume, const char* extractDir,
                            const std::string& format) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.%s",
             extractDir, costume.assetId.c_str(), format.c_str());

    // Detect backdrop: stage costumes in Scratch typically have rotation
    // centers at the stage center (240, 180 for 480×360 stage).
    // We detect more robustly by checking both rotation center AND if the
    // parent sprite's isStage flag — but we don't have the sprite here,
    // so use a generous threshold.
    bool isLikelyBackdrop = (costume.rotationCenterX >= 180 &&
                              costume.rotationCenterY >= 130);

    int dstW = isLikelyBackdrop ? 256 : 64;
    int dstH = isLikelyBackdrop ? 192 : 64;

    uint16_t* gfx = nullptr;
    uint16_t* pal = nullptr;
    int  w   = 0;
    int  h   = 0;
    bool ok  = false;

    if (format == "png") {
        ok = loadPng(path, &gfx, &pal, &w, &h);
    } else if (format == "bmp") {
        ok = loadBmp(path, &gfx, &pal, &w, &h,
                     isLikelyBackdrop ? 256 : 64,
                     isLikelyBackdrop ? 192 : 64);
    } else if (format == "svg") {
        ok = loadSvg(path, &gfx, &pal, &w, &h, dstW, dstH);
    }

    if (!ok || !gfx || !pal) {
        // Fallback: generate a visible placeholder
        w = isLikelyBackdrop ? 256 : 32;
        h = isLikelyBackdrop ? 192 : 32;
        size_t pixCount = (size_t)(w * h);
        gfx = (uint16_t*)malloc(pixCount);
        pal = (uint16_t*)calloc(256, 2);
        if (gfx && pal) {
            memset(gfx, 1, pixCount);
            pal[0] = 0;
            pal[1] = (format == "svg") ?
                     (uint16_t)RGB15(31, 0, 31) :   // magenta
                     (uint16_t)RGB15(0, 31, 31);     // cyan
            ok = true;
        } else {
            free(gfx); free(pal);
            return;
        }
    }

    costume.width  = w;
    costume.height = h;

    // Stage backdrops stored as heap buffer, blitted each frame to BG VRAM
    if (isLikelyBackdrop) {
        costume.gfxPtr    = gfx;
        costume.palPtr    = pal;
        costume.isBackdrop = true;
        return;
    }

    costume.isBackdrop = false;

    // Clamp to NDS max OAM sprite size
    if (w > 64) w = 64;
    if (h > 64) h = 64;

    SpriteSize sz = bestSpriteSize(w, h);
    int sw, sh;
    getSpriteSize(sz, sw, sh);

    uint16_t* vramGfx = oamAllocateGfx(&oamMain, sz, SpriteColorFormat_256Color);
    if (!vramGfx) { free(gfx); free(pal); return; }

    size_t byteCount = (size_t)(sw * sh);
    dmaCopy(gfx, vramGfx, byteCount);
    free(gfx);

    uint16_t* vramPal = SPRITE_PALETTE;
    dmaCopy(pal, vramPal, 256 * 2);
    free(pal);

    costume.gfxPtr = vramGfx;
    costume.palPtr = vramPal;
    costume.width  = sw;
    costume.height = sh;
}

// -----------------------------------------------------------------------
// SVG loader: rasterize to 8bpp indexed
// -----------------------------------------------------------------------
bool Renderer::loadSvg(const char* path, uint16_t** outGfx, uint16_t** outPal,
                       int* outW, int* outH, int dstW, int dstH) {
    SvgImage img;
    if (!svgRasterize(path, img, dstW, dstH)) return false;

    *outW = img.width;
    *outH = img.height;

    size_t pixBytes = (size_t)(img.width * img.height);
    *outGfx = (uint16_t*)malloc(pixBytes + 1);
    *outPal = (uint16_t*)malloc(256 * 2);
    if (!*outGfx || !*outPal) {
        free(*outGfx); free(*outPal);
        return false;
    }

    memcpy(*outGfx, img.pixels, pixBytes);
    memcpy(*outPal, img.palette, img.palCount * 2);
    if (img.palCount < 256)
        memset((uint8_t*)*outPal + img.palCount * 2, 0, (256 - img.palCount) * 2);

    return true;
}

// -----------------------------------------------------------------------
// Render one frame
// -----------------------------------------------------------------------
void Renderer::renderFrame(ScratchProject& project) {
    // ── Step 1: Render backdrop to BG ────────────────────────────────
    renderBackdrop(project);

    // ── Step 2: Render sprites to OAM ────────────────────────────────
    oamClear(&oamMain, 0, 0);
    int oamIdx = 0;

    // Sort by layer order
    std::vector<ScratchSprite*> sorted;
    for (auto& s : project.targets) {
        if (!s.isStage) sorted.push_back(&s);
    }
    std::sort(sorted.begin(), sorted.end(),
        [](ScratchSprite* a, ScratchSprite* b) {
            return a->layerOrder < b->layerOrder;
        });

    for (ScratchSprite* sprite : sorted) {
        if (!sprite->visible) continue;
        if (sprite->costumes.empty()) continue;
        if (oamIdx >= MAX_OAM_SPRITES) break;

        ScratchCostume& costume = sprite->costumes[sprite->currentCostume];
        if (!costume.gfxPtr || costume.isBackdrop) continue;

        double scaledX = sprite->x * STAGE_SCALE_X;
        double scaledY = -sprite->y * STAGE_SCALE_Y;
        int screenX = (int)(NDS_CENTER_X + scaledX) - costume.width  / 2;
        int screenY = (int)(NDS_CENTER_Y + scaledY) - costume.height / 2;

        bool scaled   = (sprite->size != 100.0);
        bool rotated  = (sprite->rotationStyle == "all around" && sprite->direction != 90);
        int  affineIdx = -1;

        if ((scaled || rotated) && oamIdx < 32) {
            affineIdx = oamIdx % 32;
            double sc = sprite->size / 100.0;
            double angle = (sprite->rotationStyle == "all around") ?
                           (sprite->direction - 90.0) * M_PI / 180.0 : 0.0;
            int32_t cosA = (int32_t)(cos(angle) / sc * 256);
            int32_t sinA = (int32_t)(sin(angle) / sc * 256);
            oamAffineTransformation(&oamMain, affineIdx, cosA, sinA, -sinA, cosA);
        }

        bool hFlip = (sprite->rotationStyle == "left-right" && sprite->x < 0);

        SpriteSize sz = bestSpriteSize(costume.width, costume.height);
        oamSet(&oamMain, oamIdx++,
               screenX, screenY,
               0, 0, sz,
               SpriteColorFormat_256Color,
               costume.gfxPtr,
               affineIdx,
               (affineIdx >= 0),
               false, hFlip, false, false);
    }
}

// -----------------------------------------------------------------------
// Render backdrop to BG layer
// -----------------------------------------------------------------------
void Renderer::renderBackdrop(ScratchProject& project) {
    ScratchSprite* stage = project.getStage();
    if (!stage || stage->costumes.empty()) {
        memset(BG_BITMAP_VRAM, 0, 256 * 192);
        return;
    }

    ScratchCostume& bg = stage->costumes[stage->currentCostume];
    if (!bg.gfxPtr || !bg.isBackdrop) {
        // No backdrop loaded — fill with a neutral grey
        memset(BG_BITMAP_VRAM, 128, 256 * 192);
        // Also write a simple 2-colour palette so index 128 shows as white
        uint16_t* bgPalRam = BG_PAL_MAIN;
        bgPalRam[128] = RGB15(31, 31, 31); // white
        return;
    }

    // Upload palette
    dmaCopy(bg.palPtr, BG_PAL_MAIN, 256 * 2);

    // Blit 8bpp pixels to BG VRAM
    uint8_t* src = (uint8_t*)bg.gfxPtr;
    uint8_t* dst = BG_BITMAP_VRAM;

    if (bg.width == 256 && bg.height == 192) {
        dmaCopy(src, dst, 256 * 192);
    } else {
        // Nearest-neighbour scale to fill 256×192
        for (int y = 0; y < 192; y++) {
            int sy = (y * bg.height) / 192;
            uint8_t* srcRow = src + sy * bg.width;
            uint8_t* dstRow = dst + y * 256;
            for (int x = 0; x < 256; x++) {
                dstRow[x] = srcRow[(x * bg.width) / 256];
            }
        }
    }
}

// -----------------------------------------------------------------------
// Render UI on bottom screen — fully clears before writing
// -----------------------------------------------------------------------
void Renderer::renderUI(ScratchProject& project, InputHandler& input) {
    consoleSelect(&bottomConsole);
    consoleClear();          // wipe everything, no ghost text

    printf("\x1b[0;0H");   // cursor to top-left
    printf("--- Variables ---\n");

    int shown = 0;
    for (auto& sprite : project.targets) {
        for (auto& var : sprite.variables) {
            if (var.visible && shown < 16) {
                printf("%-10s: %s\n",
                    var.name.substr(0, 10).c_str(),
                    var.value.substr(0, 12).c_str());
                shown++;
            }
        }
    }

    if (input.isTouching()) {
        printf("\x1b[20;0HTouch: (%3d,%3d)\n",
                input.getTouchX(), input.getTouchY());
    }

    printf("\x1b[22;0H[A]B[X][Y][L][R][^][v][<][>]");
}

// -----------------------------------------------------------------------
// PNG loader stub — loads a 32×32 coloured placeholder
// -----------------------------------------------------------------------
bool Renderer::loadPng(const char* path, uint16_t** outGfx, uint16_t** outPal,
                        int* outW, int* outH) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;

    uint8_t header[51];
    size_t got = fread(header, 1, sizeof(header), f);
    fclose(f);

    *outW   = 32; *outH = 32;
    *outGfx = (uint16_t*)malloc(32 * 32);
    *outPal = (uint16_t*)calloc(256, 2);
    if (!*outGfx || !*outPal) { free(*outGfx); free(*outPal); return false; }

    memset(*outGfx, 1, 32 * 32);
    (*outPal)[0] = 0;
    uint8_t r = 128, g = 128, b = 200;
    if (got > 22) { r = header[20]; g = header[21]; b = header[22]; }
    (*outPal)[1] = (uint16_t)RGB15(r >> 3, g >> 3, b >> 3);
    return true;
}

// -----------------------------------------------------------------------
// BMP loader — supports 1/4/8/24/32bpp, all standard header sizes
//
// Key fixes vs. previous version:
//  1. Read full BMP file header (BITMAPFILEHEADER = 14 bytes) then
//     DIB header (BITMAPINFOHEADER = 40 bytes minimum) separately so we
//     correctly handle extended headers (V4, V5) where dataOfs > 54.
//  2. For 1/4/8bpp, read the embedded colour table into the palette
//     directly rather than re-quantising.
//  3. Use dataOfs from the file header to seek to pixel data, not a
//     hardcoded offset — fixes blank white BMPs that have colour tables.
//  4. Correct stride: rows are DWORD-aligned regardless of bpp.
//  5. Scale output to fit dstW×dstH using nearest-neighbour so backdrops
//     fill the screen without needing a separate scale pass.
// -----------------------------------------------------------------------
bool Renderer::loadBmp(const char* path,
                       uint16_t** outGfx, uint16_t** outPal,
                       int* outW, int* outH,
                       int maxW, int maxH) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;

    // ── BMP file header (14 bytes) ────────────────────────────────────
    uint8_t fileHdr[14];
    if (fread(fileHdr, 1, 14, f) < 14) { fclose(f); return false; }
    if (fileHdr[0] != 'B' || fileHdr[1] != 'M') { fclose(f); return false; }

    uint32_t dataOfs = (uint32_t)fileHdr[10]
                     | ((uint32_t)fileHdr[11] <<  8)
                     | ((uint32_t)fileHdr[12] << 16)
                     | ((uint32_t)fileHdr[13] << 24);

    // ── DIB header (at least 40 bytes) ────────────────────────────────
    uint8_t dibHdr[124];  // max V5 header size
    if (fread(dibHdr, 1, 40, f) < 40) { fclose(f); return false; }

    uint32_t dibSize  = (uint32_t)dibHdr[0] | ((uint32_t)dibHdr[1]<<8)
                      | ((uint32_t)dibHdr[2]<<16) | ((uint32_t)dibHdr[3]<<24);

    int32_t  srcW    = (int32_t)( (uint32_t)dibHdr[4]  | ((uint32_t)dibHdr[5]<<8)
                                | ((uint32_t)dibHdr[6]<<16) | ((uint32_t)dibHdr[7]<<24) );
    int32_t  srcH    = (int32_t)( (uint32_t)dibHdr[8]  | ((uint32_t)dibHdr[9]<<8)
                                | ((uint32_t)dibHdr[10]<<16) | ((uint32_t)dibHdr[11]<<24) );
    uint16_t bpp     = (uint16_t)( dibHdr[14] | (dibHdr[15]<<8) );
    uint32_t compress= (uint32_t)dibHdr[16] | ((uint32_t)dibHdr[17]<<8)
                      | ((uint32_t)dibHdr[18]<<16) | ((uint32_t)dibHdr[19]<<24);
    uint32_t colorsUsed = (uint32_t)dibHdr[32] | ((uint32_t)dibHdr[33]<<8)
                        | ((uint32_t)dibHdr[34]<<16) | ((uint32_t)dibHdr[35]<<24);

    // Only support uncompressed (0=RGB, 3=BITFIELDS) and standard bpp
    if (compress != 0 && compress != 3) { fclose(f); return false; }
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32) {
        fclose(f); return false;
    }

    bool flipped = (srcH > 0);  // positive height = bottom-up
    if (srcH < 0) srcH = -srcH;
    if (srcW <= 0) { fclose(f); return false; }

    // Skip remainder of DIB header (V4/V5 can be up to 124 bytes total)
    if (dibSize > 40) {
        uint32_t extra = dibSize - 40;
        if (extra > sizeof(dibHdr) - 40) extra = sizeof(dibHdr) - 40;
        fread(dibHdr + 40, 1, extra, f);
    }

    // ── Colour table (for 1/4/8bpp) ──────────────────────────────────
    // Entries are RGBQUAD (4 bytes each): B, G, R, reserved
    uint16_t palBuf[256];
    memset(palBuf, 0, sizeof(palBuf));
    int palEntries = 0;

    if (bpp <= 8) {
        palEntries = (int)colorsUsed;
        if (palEntries == 0) palEntries = 1 << bpp;  // default
        if (palEntries > 256) palEntries = 256;

        uint8_t quad[4];
        for (int p = 0; p < palEntries; p++) {
            if (fread(quad, 1, 4, f) < 4) break;
            // quad = B G R reserved
            palBuf[p] = (uint16_t)RGB15(quad[2] >> 3, quad[1] >> 3, quad[0] >> 3);
        }
    }

    // ── Seek to pixel data ────────────────────────────────────────────
    if (fseek(f, (long)dataOfs, SEEK_SET) != 0) { fclose(f); return false; }

    // ── Determine output dimensions ───────────────────────────────────
    int dstW = (srcW  < maxW) ? srcW  : maxW;
    int dstH = (srcH < maxH) ? srcH : maxH;
    *outW = dstW;
    *outH = dstH;

    // ── Allocate output buffers ───────────────────────────────────────
    uint8_t*  px8 = (uint8_t*)malloc((size_t)(dstW * dstH));
    uint16_t* pal = (uint16_t*)calloc(256, 2);
    if (!px8 || !pal) { free(px8); free(pal); fclose(f); return false; }

    // Stride: each row is padded to a multiple of 4 bytes
    int bitsPerRow  = srcW * (int)bpp;
    int stride      = ((bitsPerRow + 31) / 32) * 4;
    uint8_t* rowBuf = (uint8_t*)malloc((size_t)stride);
    if (!rowBuf) { free(px8); free(pal); fclose(f); return false; }

    // ── Build output palette ──────────────────────────────────────────
    // Index 0 = transparent / background
    int palCount = 1;

    if (bpp <= 8) {
        // Copy pre-built colour table; index 0 stays transparent black
        pal[0] = 0;
        for (int p = 0; p < palEntries; p++) {
            pal[p + 1] = palBuf[p];
        }
        palCount = palEntries + 1;
    } else {
        pal[0] = 0;
        palCount = 1;
    }

    // ── Decode rows ───────────────────────────────────────────────────
    for (int y = 0; y < srcH; y++) {
        if (fread(rowBuf, 1, (size_t)stride, f) < (size_t)stride) {
            // Truncated file — zero-fill remaining
            memset(px8, 0, (size_t)(dstW * dstH));
            break;
        }

        // Skip rows beyond dstH
        if (y >= dstH) continue;

        int dstY = flipped ? (dstH - 1 - y) : y;
        if (dstY < 0 || dstY >= dstH) continue;

        uint8_t* dstRow = px8 + dstY * dstW;

        for (int x = 0; x < dstW; x++) {
            // Map source x to dstW (nearest-neighbour scale-down)
            int sx = (srcW > dstW) ? (x * srcW / dstW) : x;
            if (sx >= srcW) sx = srcW - 1;

            uint8_t idx = 0;

            if (bpp == 1) {
                int bit = 7 - (sx & 7);
                idx = (uint8_t)(((rowBuf[sx >> 3] >> bit) & 1) + 1);
            } else if (bpp == 4) {
                uint8_t nibble = rowBuf[sx >> 1];
                idx = (uint8_t)(((sx & 1) ? (nibble & 0x0F) : (nibble >> 4)) + 1);
            } else if (bpp == 8) {
                idx = rowBuf[sx] + 1;   // shift by 1 because pal[0]=transparent
                if (idx == 0) idx = 1;  // handle 0-index -> transparent slot
            } else {
                // 24/32-bit: quantise to our growing palette
                int bpp_bytes = bpp / 8;
                uint8_t b8 = rowBuf[sx * bpp_bytes + 0];
                uint8_t g8 = rowBuf[sx * bpp_bytes + 1];
                uint8_t r8 = rowBuf[sx * bpp_bytes + 2];
                uint8_t a8 = (bpp == 32) ? rowBuf[sx * bpp_bytes + 3] : 255;

                if (a8 < 128) { dstRow[x] = 0; continue; }

                uint16_t c15 = (uint16_t)RGB15(r8 >> 3, g8 >> 3, b8 >> 3);

                // Find existing palette entry
                int found = 0;
                for (int p = 1; p < palCount; p++) {
                    if (pal[p] == c15) { found = p; break; }
                }
                if (found == 0) {
                    if (palCount < 256) {
                        pal[palCount] = c15;
                        found = palCount++;
                    } else {
                        // Palette full: find nearest colour
                        int best = 1, bestD = 0x7FFFFFFF;
                        int tr = r8, tg = g8, tb = b8;
                        for (int p = 1; p < 256; p++) {
                            int pr = (pal[p] & 0x1F) << 3;
                            int pg = ((pal[p] >> 5) & 0x1F) << 3;
                            int pb = ((pal[p] >> 10) & 0x1F) << 3;
                            int d  = (tr-pr)*(tr-pr)+(tg-pg)*(tg-pg)+(tb-pb)*(tb-pb);
                            if (d < bestD) { bestD = d; best = p; }
                        }
                        found = best;
                    }
                }
                idx = (uint8_t)found;
            }
            dstRow[x] = idx;
        }
    }

    free(rowBuf);
    fclose(f);

    *outGfx = (uint16_t*)px8;
    *outPal = pal;
    return true;
}

// -----------------------------------------------------------------------
// OAM sprite size helpers
// -----------------------------------------------------------------------
SpriteSize Renderer::bestSpriteSize(int w, int h) {
    if (w <=  8 && h <=  8) return SpriteSize_8x8;
    if (w <= 16 && h <=  8) return SpriteSize_16x8;
    if (w <=  8 && h <= 16) return SpriteSize_8x16;
    if (w <= 16 && h <= 16) return SpriteSize_16x16;
    if (w <= 32 && h <=  8) return SpriteSize_32x8;
    if (w <=  8 && h <= 32) return SpriteSize_8x32;
    if (w <= 32 && h <= 16) return SpriteSize_32x16;
    if (w <= 16 && h <= 32) return SpriteSize_16x32;
    if (w <= 32 && h <= 32) return SpriteSize_32x32;
    if (w <= 64 && h <= 32) return SpriteSize_64x32;
    if (w <= 32 && h <= 64) return SpriteSize_32x64;
    return SpriteSize_64x64;
}

void Renderer::getSpriteSize(SpriteSize sz, int& w, int& h) {
    switch (sz) {
        case SpriteSize_8x8:   w=8;  h=8;  return;
        case SpriteSize_16x8:  w=16; h=8;  return;
        case SpriteSize_8x16:  w=8;  h=16; return;
        case SpriteSize_16x16: w=16; h=16; return;
        case SpriteSize_32x8:  w=32; h=8;  return;
        case SpriteSize_8x32:  w=8;  h=32; return;
        case SpriteSize_32x16: w=32; h=16; return;
        case SpriteSize_16x32: w=16; h=32; return;
        case SpriteSize_32x32: w=32; h=32; return;
        case SpriteSize_64x32: w=64; h=32; return;
        case SpriteSize_32x64: w=32; h=64; return;
        default:               w=64; h=64; return;
    }
}
