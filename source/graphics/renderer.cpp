// =============================================================================
// renderer.cpp — Updated with SVG rasterization and proper backdrop rendering
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
// Each frame we blit the current backdrop palette and pixels.
// -----------------------------------------------------------------------
#define BG_BITMAP_VRAM ((uint8_t*)0x06000000)  // VRAM bank A mapped as main BG
#define BG_PAL_MAIN    ((uint16_t*)0x05000000)  // BG palette RAM (256 entries)

//static uint16_t sharedSpritePal[SHARED_PAL_SIZE];
//static int      sharedPalCount;

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

    // Set up BG2 in extended rotation/scaling bitmap mode for backdrop
    // Mode 5: BG2 and BG3 can be 256×192 8bpp bitmaps
    // We use BG3 (layer 3) for the backdrop so sprites appear on top
    bgHandle = bgInit(3, BgType_Bmp8, BgSize_B8_256x256, 0, 0);

    consoleInit(&bottomConsole, 0, BgType_Text4bpp, BgSize_T_256x256,
                2, 0, false, true);

    // Clear backdrop with dark grey
    memset(BG_BITMAP_VRAM, 0, 256*192);
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

    // For backdrops (stage), use larger size to fill the screen
    // We detect stage costumes by checking if the rotationCenter equals stage center
    bool isLikelyBackdrop = (costume.rotationCenterX >= 200 &&
                              costume.rotationCenterY >= 150);
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
        ok = loadBmp(path, &gfx, &pal, &w, &h);
    } else if (format == "svg") {
        ok = loadSvg(path, &gfx, &pal, &w, &h, dstW, dstH);
    }

    if (!ok || !gfx || !pal) {
        // Fallback: generate a coloured placeholder
        w = 32; h = 32;
        gfx = (uint16_t*)calloc(w * h / 2 + 1, 1);
        pal = (uint16_t*)calloc(256, 2);
        if (gfx && pal) {
            // Fill with a visible colour (magenta for SVG, cyan for others)
            uint8_t* px8 = (uint8_t*)gfx;
            memset(px8, 1, w * h);
            pal[0] = 0;
            pal[1] = (format == "svg") ?
                     (uint16_t)RGB15(31, 0, 31) :   // magenta
                     (uint16_t)RGB15(0, 31, 31);     // cyan
            ok = true;
        }
    }

    if (!ok || !gfx || !pal) return;

    costume.width  = w;
    costume.height = h;

    // Stage backdrops are stored specially — not in OAM
    if (isLikelyBackdrop) {
        // Store as a flat buffer directly on the costume struct
        // We repurpose gfxPtr to point to a heap buffer (not VRAM)
        // Renderer will blit this to BG VRAM each frame
        costume.gfxPtr = gfx;   // 8bpp pixel data (cast from uint16_t*)
        costume.palPtr = pal;
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

    // Copy pixel data (gfx is an 8bpp byte buffer cast as uint16_t*)
    size_t byteCount = (size_t)(sw * sh);
    dmaCopy(gfx, vramGfx, byteCount);
    free(gfx);

    // Upload palette to a slot based on costume index
    // We use the extended palette area — for simplicity use one shared palette
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
    // Zero-fill unused palette entries
    if (img.palCount < 256)
        memset((uint8_t*)*outPal + img.palCount*2, 0, (256 - img.palCount)*2);

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

        // Scratch coords → NDS screen coords
        // Scratch: x∈[-240,240], y∈[-180,180] (center=0,0)
        // NDS: x∈[0,255], y∈[0,191]
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
               0,                          // priority
               0,                          // palette index
               sz,
               SpriteColorFormat_256Color,
               costume.gfxPtr,
               affineIdx,
               (affineIdx >= 0),           // double size
               false,                      // hidden
               hFlip,                      // h-flip
               false,                      // v-flip
               false);                     // mosaic
    }
}

// -----------------------------------------------------------------------
// Render backdrop to BG layer
// -----------------------------------------------------------------------
void Renderer::renderBackdrop(ScratchProject& project) {
    ScratchSprite* stage = project.getStage();
    if (!stage || stage->costumes.empty()) {
        // Clear to dark grey if no backdrop
        memset(BG_BITMAP_VRAM, 0, 256 * 192);
        return;
    }

    ScratchCostume& bg = stage->costumes[stage->currentCostume];
    if (!bg.gfxPtr) return;

    // Upload palette
    uint16_t* bgPalRam = BG_PAL_MAIN;
    dmaCopy(bg.palPtr, bgPalRam, 256 * 2);

    // Blit 8bpp pixels to BG VRAM
    // bg.width and bg.height are the rasterized dimensions (up to 256×192)
    uint8_t* src = (uint8_t*)bg.gfxPtr;
    uint8_t* dst = BG_BITMAP_VRAM;

    if (bg.width == 256 && bg.height == 192) {
        // Direct DMA copy
        dmaCopy(src, dst, 256 * 192);
    } else {
        // Scale/blit to fit 256×192
        for (int y = 0; y < 192; y++) {
            int sy = (y * bg.height) / 192;
            for (int x = 0; x < 256; x++) {
                int sx = (x * bg.width) / 256;
                dst[y * 256 + x] = src[sy * bg.width + sx];
            }
        }
    }
}

// -----------------------------------------------------------------------
// Render UI on bottom screen
// -----------------------------------------------------------------------
void Renderer::renderUI(ScratchProject& project, InputHandler& input) {
    consoleSelect(&bottomConsole);
    consoleClear();
    printf("\x1b[0;0H");
    printf("--- Variables ---\n");
    int shown = 0;
    for (auto& sprite : project.targets) {
        for (auto& kv : sprite.variables) {
            if (kv.second.visible && shown < 16) {
                printf("%-10s: %s\n",
                    kv.second.name.substr(0, 10).c_str(),
                    kv.second.value.substr(0, 12).c_str());
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
// PNG loader stub (same as before)
// -----------------------------------------------------------------------
bool Renderer::loadPng(const char* path, uint16_t** outGfx, uint16_t** outPal,
                        int* outW, int* outH) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* raw = (uint8_t*)malloc(sz);
    if (!raw) { fclose(f); return false; }
    fread(raw, 1, sz, f); fclose(f);

    // With lodepng this would be a full decode.
    // For now: 32×32 placeholder using first 3 bytes as colour hint
    *outW   = 32; *outH   = 32;
    *outGfx = (uint16_t*)malloc(32 * 32 + 1);
    *outPal = (uint16_t*)calloc(256, 2);
    if (*outGfx && *outPal) {
        memset(*outGfx, 1, 32 * 32);
        (*outPal)[0] = 0;
        // Sample first pixel (offset 0x1a for typical PNG IDAT)
        uint8_t r=128, g=128, b=200;
        if (sz > 50) { r=raw[20]; g=raw[21]; b=raw[22]; }
        (*outPal)[1] = (uint16_t)RGB15(r>>3, g>>3, b>>3);
    }
    free(raw);
    return (*outGfx != nullptr);
}

// -----------------------------------------------------------------------
// BMP loader (24-bit uncompressed) — unchanged
// -----------------------------------------------------------------------
bool Renderer::loadBmp(const char* path, uint16_t** outGfx, uint16_t** outPal,
                        int* outW, int* outH) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    
    uint8_t hdr[54];
    if (fread(hdr, 1, 54, f) < 54) { fclose(f); return false; }
    if (hdr[0] != 'B' || hdr[1] != 'M') { fclose(f); return false; }
    
    int w        = *(int32_t*)(hdr + 18);
    int h        = *(int32_t*)(hdr + 22);
    int bpp      = *(uint16_t*)(hdr + 28);
    int dataOfs  = *(int32_t*)(hdr + 10);
    bool flipped = (h > 0);
    if (h < 0) h = -h;
    if (bpp != 24 && bpp != 32) { fclose(f); return false; }
    if (w > 64) w = 64;
    if (h > 64) h = 64;
    *outW = w; *outH = h;

    // Allocate output buffers
    // outGfx holds 8bpp indices, cast to uint16_t* for API compatibility
    uint8_t*  px8 = (uint8_t*)malloc(w * h);
    uint16_t* pal = (uint16_t*)calloc(256, 2);
    if (!px8 || !pal) { free(px8); free(pal); fclose(f); return false; }

    int bytesPerPixel = bpp / 8;
    int stride = (w * bytesPerPixel + 3) & ~3;  // row padded to 4 bytes
    uint8_t* row = (uint8_t*)malloc(stride);
    if (!row) { free(px8); free(pal); fclose(f); return false; }

    // Index 0 = transparent black
    pal[0] = 0;
    int palCount = 1;

    fseek(f, dataOfs, SEEK_SET);

    for (int y = 0; y < h; y++) {
        // BMP rows are bottom-up when h > 0
        int dstY = flipped ? (h - 1 - y) : y;
        fread(row, 1, stride, f);
        for (int x = 0; x < w; x++) {
            uint8_t b = row[x * bytesPerPixel + 0];
            uint8_t g = row[x * bytesPerPixel + 1];
            uint8_t r = row[x * bytesPerPixel + 2];
            // Skip transparent pixels if 32bpp and alpha=0
            uint8_t a = (bpp == 32) ? row[x * bytesPerPixel + 3] : 255;
            if (a < 128) { px8[dstY * w + x] = 0; continue; }

            uint16_t c15 = (uint16_t)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
            // Find or add palette entry
            int idx = 0;
            for (int p = 1; p < palCount; p++) {
                if (pal[p] == c15) { idx = p; break; }
            }
            if (idx == 0 && palCount < 256) {
                pal[palCount] = c15;
                idx = palCount++;
            } else if (idx == 0) {
                // Palette full: find nearest colour
                int best = 1, bestD = 0x7FFFFFFF;
                int tr = r, tg = g, tb = b;
                for (int p = 1; p < 256; p++) {
                    int pr = (pal[p] & 0x1F) << 3;
                    int pg = ((pal[p] >> 5) & 0x1F) << 3;
                    int pb = ((pal[p] >> 10) & 0x1F) << 3;
                    int d = (tr-pr)*(tr-pr)+(tg-pg)*(tg-pg)+(tb-pb)*(tb-pb);
                    if (d < bestD) { bestD = d; best = p; }
                }
                idx = best;
            }
            px8[dstY * w + x] = (uint8_t)idx;
        }
    }
    free(row);
    fclose(f);

    *outGfx = (uint16_t*)px8;  // caller frees via free()
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
