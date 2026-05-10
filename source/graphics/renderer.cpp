// =============================================================================
// renderer.cpp — Real PNG loading via lodepng, memory-efficient for NDS
//
// PNG decode pipeline:
//   PNG file on SD → lodepng_decode32_file() → RGBA8888 heap buffer
//   → quantise to 8bpp indexed + RGB555 palette → free RGBA buffer
//   → dmaCopy 8bpp pixels + palette to VRAM / OAM
//
// The RGBA buffer is freed immediately after quantisation so peak RAM
// usage for a 64×64 sprite is:
//   RGBA: 64*64*4 = 16 KB  (freed promptly)
//   8bpp: 64*64*1 =  4 KB  (stays in VRAM)
//   pal:  256*2   =  0.5KB (stays in palette RAM)
// A 256×192 backdrop:
//   RGBA: 256*192*4 = 196 KB (freed promptly)
//   8bpp: 256*192*1 =  48 KB (in BG VRAM)
// =============================================================================
#include "renderer.h"
#include "lodepng.h"
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

// ── BG VRAM for backdrop (bank A mapped as 8bpp bitmap BG) ───────────────────
#define BG_BITMAP_VRAM ((uint8_t*)0x06000000)
#define BG_PAL_MAIN    ((uint16_t*)0x05000000)

// ── Internal: quantise RGBA8888 → 8bpp indexed + RGB555 palette ──────────────
//
// Returns a freshly malloc'd 8bpp pixel buffer (w*h bytes) and fills
// outPal[256] with RGB555 entries.  Index 0 is always transparent.
// Pixels with alpha < 128 are mapped to index 0.
// Remaining colours are added to the palette on first occurrence;
// when the palette is full (255 non-transparent entries) the nearest
// existing colour is used (O(n) search, fast for 256-entry space).
//
// Caller must free() the returned buffer.
static uint8_t* rgba_to_8bpp(const uint8_t* rgba,
                              unsigned w, unsigned h,
                              uint16_t* outPal, int* outPalCount) {
    uint8_t* px = (uint8_t*)malloc(w * h);
    if (!px) return nullptr;

    memset(outPal, 0, 256 * 2);
    int palCount = 1; /* index 0 = transparent */

    for (unsigned i = 0; i < w * h; i++) {
        uint8_t r = rgba[i*4+0];
        uint8_t g = rgba[i*4+1];
        uint8_t b = rgba[i*4+2];
        uint8_t a = rgba[i*4+3];

        if (a < 128) {
            px[i] = 0;
            continue;
        }

        uint16_t c15 = lodepng_rgb555(r, g, b);

        /* Linear search for existing entry */
        int found = 0;
        for (int p = 1; p < palCount; p++) {
            if (outPal[p] == c15) { found = p; break; }
        }

        if (found == 0) {
            if (palCount < 256) {
                outPal[palCount] = c15;
                found = palCount++;
            } else {
                /* Palette full: nearest colour (squared RGB555 distance) */
                int best = 1, bestD = 0x7FFFFFFF;
                int tr = (int)(r >> 3), tg = (int)(g >> 3), tb = (int)(b >> 3);
                for (int p = 1; p < 256; p++) {
                    int pr = (int)(outPal[p] & 0x1F);
                    int pg = (int)((outPal[p] >> 5) & 0x1F);
                    int pb = (int)((outPal[p] >> 10) & 0x1F);
                    int d  = (tr-pr)*(tr-pr) + (tg-pg)*(tg-pg) + (tb-pb)*(tb-pb);
                    if (d < bestD) { bestD = d; best = p; }
                }
                found = best;
            }
        }
        px[i] = (uint8_t)found;
    }

    if (outPalCount) *outPalCount = palCount;
    return px;
}

// ─────────────────────────────────────────────────────────────────────────────
void Renderer::init() {
    memset(oamUsed, 0, sizeof(oamUsed));
    memset(gfxUsed, 0, sizeof(gfxUsed));
    nextOam        = 0;
    bgHandle       = -1;
    bgGfxPtr       = nullptr;
    bgPalPtr       = nullptr;
    backdropLoaded = false;

    oamInit(&oamMain, SpriteMapping_1D_32, false);
    bgHandle = bgInit(3, BgType_Bmp8, BgSize_B8_256x256, 0, 0);

    consoleInit(&bottomConsole, 0, BgType_Text4bpp, BgSize_T_256x256,
                2, 0, false, true);

    memset(BG_BITMAP_VRAM, 0, 256 * 192);
}

void Renderer::clearBottomConsole() {
    consoleSelect(&bottomConsole);
    consoleClear();
}

void Renderer::loadSprites(ScratchProject& project) {
    for (auto& sprite : project.targets) {
        for (auto& costume : sprite.costumes) {
            loadCostume(costume, project.extractDir.c_str(), costume.dataFormat);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// loadCostume — dispatches to format-specific loader
// ─────────────────────────────────────────────────────────────────────────────
void Renderer::loadCostume(ScratchCostume& costume, const char* extractDir,
                            const std::string& format) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.%s",
             extractDir, costume.assetId.c_str(), format.c_str());

    bool isLikelyBackdrop = (costume.rotationCenterX >= 180 &&
                              costume.rotationCenterY >= 130);

    int dstW = isLikelyBackdrop ? 256 : 64;
    int dstH = isLikelyBackdrop ? 192 : 64;

    uint16_t* gfx = nullptr;
    uint16_t* pal = nullptr;
    int w = 0, h = 0;
    bool ok = false;

    if (format == "png") {
        ok = loadPng(path, &gfx, &pal, &w, &h, dstW, dstH);
    } else if (format == "bmp") {
        ok = loadBmp(path, &gfx, &pal, &w, &h, dstW, dstH);
    } else if (format == "svg") {
        ok = loadSvg(path, &gfx, &pal, &w, &h, dstW, dstH);
    }

    if (!ok || !gfx || !pal) {
        /* Visible placeholder */
        w = isLikelyBackdrop ? 256 : 32;
        h = isLikelyBackdrop ? 192 : 32;
        size_t pixCount = (size_t)(w * h);
        free(gfx); free(pal);
        gfx = (uint16_t*)malloc(pixCount);
        pal = (uint16_t*)calloc(256, 2);
        if (gfx && pal) {
            memset(gfx, 1, pixCount);
            pal[0] = 0;
            pal[1] = (format == "svg") ? (uint16_t)RGB15(31, 0, 31)
                                       : (uint16_t)RGB15(0, 31, 31);
            ok = true;
        } else {
            free(gfx); free(pal);
            return;
        }
    }

    costume.width  = w;
    costume.height = h;

    if (isLikelyBackdrop) {
        costume.gfxPtr    = gfx;
        costume.palPtr    = pal;
        costume.isBackdrop = true;
        return;
    }

    costume.isBackdrop = false;
    if (w > 64) w = 64;
    if (h > 64) h = 64;

    SpriteSize sz = bestSpriteSize(w, h);
    int sw, sh; getSpriteSize(sz, sw, sh);

    uint16_t* vramGfx = oamAllocateGfx(&oamMain, sz, SpriteColorFormat_256Color);
    if (!vramGfx) { free(gfx); free(pal); return; }

    dmaCopy(gfx, vramGfx, (size_t)(sw * sh));
    free(gfx);

    uint16_t* vramPal = SPRITE_PALETTE;
    dmaCopy(pal, vramPal, 256 * 2);
    free(pal);

    costume.gfxPtr = vramGfx;
    costume.palPtr = vramPal;
    costume.width  = sw;
    costume.height = sh;
}

// ─────────────────────────────────────────────────────────────────────────────
// loadPng — real PNG decode using our embedded lodepng
//
// Pipeline:
//   1. lodepng_decode32_file → RGBA8888 (malloc'd, freed in this function)
//   2. If source is larger than dstW×dstH, nearest-neighbour downsample
//      BEFORE quantisation (saves palette slots and quantisation time)
//   3. rgba_to_8bpp → 8bpp indexed + RGB555 palette
//   4. Return 8bpp pixels as (uint16_t*) — the caller treats it as uint8_t*
// ─────────────────────────────────────────────────────────────────────────────
bool Renderer::loadPng(const char* path,
                       uint16_t** outGfx, uint16_t** outPal,
                       int* outW, int* outH,
                       int maxW, int maxH) {
    unsigned char* rgba = nullptr;
    unsigned srcW = 0, srcH = 0;

    unsigned err = lodepng_decode32_file(&rgba, &srcW, &srcH, path);
    if (err || !rgba) {
        free(rgba);
        return false;
    }

    /* Clamp output dimensions */
    int dstW = (int)srcW  < maxW ? (int)srcW  : maxW;
    int dstH = (int)srcH < maxH ? (int)srcH : maxH;

    /* If source exactly fits, use it directly; otherwise downsample */
    uint8_t* src8 = nullptr;
    bool     needFree = false;

    if (dstW == (int)srcW && dstH == (int)srcH) {
        src8 = rgba;
    } else {
        /* Nearest-neighbour downsample in-place into a new buffer */
        src8 = (uint8_t*)malloc((size_t)dstW * dstH * 4);
        if (!src8) { free(rgba); return false; }
        needFree = true;

        for (int y = 0; y < dstH; y++) {
            int sy = (int)((unsigned)y * srcH / (unsigned)dstH);
            for (int x = 0; x < dstW; x++) {
                int sx = (int)((unsigned)x * srcW / (unsigned)dstW);
                const uint8_t* s = rgba + ((size_t)sy * srcW + sx) * 4;
                uint8_t*       d = src8 + ((size_t)y  * dstW + x ) * 4;
                d[0]=s[0]; d[1]=s[1]; d[2]=s[2]; d[3]=s[3];
            }
        }
        free(rgba);
        rgba = nullptr;
    }

    /* Quantise to 8bpp */
    uint16_t* pal = (uint16_t*)calloc(256, 2);
    if (!pal) { if (needFree) free(src8); else free(rgba); return false; }

    int palCount = 0;
    uint8_t* px8 = rgba_to_8bpp(src8, (unsigned)dstW, (unsigned)dstH,
                                  pal, &palCount);

    if (needFree) free(src8);
    else          free(rgba);

    if (!px8) { free(pal); return false; }

    *outGfx = (uint16_t*)px8;   /* caller treats as uint8_t* for 8bpp */
    *outPal = pal;
    *outW   = dstW;
    *outH   = dstH;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// loadSvg — rasterise via svg_rasterizer (returns placeholder pink square)
// ─────────────────────────────────────────────────────────────────────────────
bool Renderer::loadSvg(const char* path, uint16_t** outGfx, uint16_t** outPal,
                       int* outW, int* outH, int dstW, int dstH) {
    SvgImage img;
    if (!svgRasterize(path, img, dstW, dstH)) return false;

    *outW = img.width;
    *outH = img.height;

    size_t pixBytes = (size_t)(img.width * img.height);
    *outGfx = (uint16_t*)malloc(pixBytes + 1);
    *outPal = (uint16_t*)malloc(256 * 2);
    if (!*outGfx || !*outPal) { free(*outGfx); free(*outPal); return false; }

    memcpy(*outGfx, img.pixels, pixBytes);
    memcpy(*outPal, img.palette, (size_t)img.palCount * 2);
    if (img.palCount < 256)
        memset((uint8_t*)*outPal + img.palCount * 2, 0,
               (size_t)(256 - img.palCount) * 2);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// loadBmp — supports 1/4/8/24/32bpp uncompressed Windows BMP
// ─────────────────────────────────────────────────────────────────────────────
bool Renderer::loadBmp(const char* path,
                       uint16_t** outGfx, uint16_t** outPal,
                       int* outW, int* outH,
                       int maxW, int maxH) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;

    uint8_t fileHdr[14];
    if (fread(fileHdr, 1, 14, f) < 14) { fclose(f); return false; }
    if (fileHdr[0] != 'B' || fileHdr[1] != 'M') { fclose(f); return false; }

    uint32_t dataOfs = (uint32_t)fileHdr[10] | ((uint32_t)fileHdr[11]<<8)
                     | ((uint32_t)fileHdr[12]<<16) | ((uint32_t)fileHdr[13]<<24);

    uint8_t dibHdr[40];
    if (fread(dibHdr, 1, 40, f) < 40) { fclose(f); return false; }

    uint32_t dibSize   = (uint32_t)dibHdr[0] | ((uint32_t)dibHdr[1]<<8)
                       | ((uint32_t)dibHdr[2]<<16) | ((uint32_t)dibHdr[3]<<24);
    int32_t  srcW      = (int32_t)((uint32_t)dibHdr[4]  | ((uint32_t)dibHdr[5]<<8)
                        | ((uint32_t)dibHdr[6]<<16) | ((uint32_t)dibHdr[7]<<24));
    int32_t  srcH      = (int32_t)((uint32_t)dibHdr[8]  | ((uint32_t)dibHdr[9]<<8)
                        | ((uint32_t)dibHdr[10]<<16) | ((uint32_t)dibHdr[11]<<24));
    uint16_t bpp       = (uint16_t)(dibHdr[14] | (dibHdr[15]<<8));
    uint32_t compress  = (uint32_t)dibHdr[16] | ((uint32_t)dibHdr[17]<<8)
                       | ((uint32_t)dibHdr[18]<<16) | ((uint32_t)dibHdr[19]<<24);
    uint32_t clrUsed   = (uint32_t)dibHdr[32] | ((uint32_t)dibHdr[33]<<8)
                       | ((uint32_t)dibHdr[34]<<16) | ((uint32_t)dibHdr[35]<<24);

    if (compress != 0 && compress != 3) { fclose(f); return false; }
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32) {
        fclose(f); return false;
    }

    bool flipped = (srcH > 0);
    if (srcH < 0) srcH = -srcH;
    if (srcW <= 0) { fclose(f); return false; }

    /* Skip extra DIB header bytes */
    if (dibSize > 40) {
        uint32_t extra = dibSize - 40;
        if (extra > 84) extra = 84; /* V5 max */
        uint8_t tmp[84];
        fread(tmp, 1, extra, f);
    }

    /* Palette for indexed modes */
    uint16_t palBuf[256] = {0};
    int      palEntries  = 0;
    if (bpp <= 8) {
        palEntries = (int)(clrUsed ? clrUsed : (1u << bpp));
        if (palEntries > 256) palEntries = 256;
        uint8_t quad[4];
        for (int p = 0; p < palEntries; p++) {
            if (fread(quad, 1, 4, f) < 4) break;
            palBuf[p] = (uint16_t)RGB15(quad[2]>>3, quad[1]>>3, quad[0]>>3);
        }
    }

    if (fseek(f, (long)dataOfs, SEEK_SET) != 0) { fclose(f); return false; }

    int dstW = srcW < maxW ? srcW : maxW;
    int dstH = srcH < maxH ? srcH : maxH;

    uint8_t*  px8 = (uint8_t*)malloc((size_t)(dstW * dstH));
    uint16_t* pal = (uint16_t*)calloc(256, 2);
    if (!px8 || !pal) { free(px8); free(pal); fclose(f); return false; }

    int stride = (((srcW * bpp) + 31) / 32) * 4;
    uint8_t* rowBuf = (uint8_t*)malloc((size_t)stride);
    if (!rowBuf) { free(px8); free(pal); fclose(f); return false; }

    int palCount = 1; /* 0 = transparent */
    pal[0] = 0;
    if (bpp <= 8) {
        for (int p = 0; p < palEntries; p++)
            pal[p+1] = palBuf[p];
        palCount = palEntries + 1;
    }

    for (int y = 0; y < srcH; y++) {
        if (fread(rowBuf, 1, (size_t)stride, f) < (size_t)stride) {
            memset(px8, 0, (size_t)(dstW * dstH));
            break;
        }
        if (y >= dstH) continue;

        int dstY = flipped ? (dstH - 1 - y) : y;
        if (dstY < 0 || dstY >= dstH) continue;
        uint8_t* dstRow = px8 + dstY * dstW;

        for (int x = 0; x < dstW; x++) {
            int sx = (srcW > dstW) ? (x * srcW / dstW) : x;
            if (sx >= srcW) sx = srcW - 1;
            uint8_t idx = 0;

            if (bpp == 1) {
                idx = (uint8_t)(((rowBuf[sx>>3] >> (7-(sx&7))) & 1) + 1);
            } else if (bpp == 4) {
                uint8_t n = rowBuf[sx>>1];
                idx = (uint8_t)(((sx&1) ? (n&0x0F) : (n>>4)) + 1);
            } else if (bpp == 8) {
                idx = rowBuf[sx] + 1;
                if (idx == 0) idx = 1;
            } else {
                int bs = bpp / 8;
                uint8_t b8 = rowBuf[sx*bs+0];
                uint8_t g8 = rowBuf[sx*bs+1];
                uint8_t r8 = rowBuf[sx*bs+2];
                uint8_t a8 = (bpp == 32) ? rowBuf[sx*bs+3] : 255;
                if (a8 < 128) { dstRow[x] = 0; continue; }
                uint16_t c15 = (uint16_t)RGB15(r8>>3, g8>>3, b8>>3);
                int found = 0;
                for (int p = 1; p < palCount; p++)
                    if (pal[p] == c15) { found = p; break; }
                if (found == 0) {
                    if (palCount < 256) { pal[palCount]=c15; found=palCount++; }
                    else {
                        int best=1, bestD=0x7FFFFFFF;
                        int tr=r8>>3, tg=g8>>3, tb=b8>>3;
                        for (int p=1;p<256;p++){
                            int pr=pal[p]&0x1F, pg=(pal[p]>>5)&0x1F, pb=(pal[p]>>10)&0x1F;
                            int d=(tr-pr)*(tr-pr)+(tg-pg)*(tg-pg)+(tb-pb)*(tb-pb);
                            if(d<bestD){bestD=d;best=p;}
                        }
                        found=best;
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
    *outW   = dstW;
    *outH   = dstH;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// renderFrame
// ─────────────────────────────────────────────────────────────────────────────
void Renderer::renderFrame(ScratchProject& project) {
    renderBackdrop(project);

    oamClear(&oamMain, 0, 0);
    int oamIdx = 0;

    std::vector<ScratchSprite*> sorted;
    for (auto& s : project.targets)
        if (!s.isStage) sorted.push_back(&s);
    std::sort(sorted.begin(), sorted.end(),
        [](ScratchSprite* a, ScratchSprite* b){
            return a->layerOrder < b->layerOrder;
        });

    for (ScratchSprite* sprite : sorted) {
        if (!sprite->visible || sprite->costumes.empty()) continue;
        if (oamIdx >= MAX_OAM_SPRITES) break;

        ScratchCostume& costume = sprite->costumes[sprite->currentCostume];
        if (!costume.gfxPtr || costume.isBackdrop) continue;

        double scaledX = sprite->x * STAGE_SCALE_X;
        double scaledY = -sprite->y * STAGE_SCALE_Y;
        int screenX = (int)(NDS_CENTER_X + scaledX) - costume.width  / 2;
        int screenY = (int)(NDS_CENTER_Y + scaledY) - costume.height / 2;

        bool scaled  = (sprite->size != 100.0);
        bool rotated = (sprite->rotationStyle == "all around" && sprite->direction != 90);
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
               affineIdx, (affineIdx >= 0),
               false, hFlip, false, false);
    }
}

void Renderer::renderBackdrop(ScratchProject& project) {
    ScratchSprite* stage = project.getStage();
    if (!stage || stage->costumes.empty()) {
        memset(BG_BITMAP_VRAM, 0, 256 * 192);
        return;
    }
    ScratchCostume& bg = stage->costumes[stage->currentCostume];
    if (!bg.gfxPtr || !bg.isBackdrop) {
        memset(BG_BITMAP_VRAM, 128, 256 * 192);
        BG_PAL_MAIN[128] = RGB15(31,31,31);
        return;
    }
    dmaCopy(bg.palPtr, BG_PAL_MAIN, 256 * 2);
    uint8_t* src = (uint8_t*)bg.gfxPtr;
    if (bg.width == 256 && bg.height == 192) {
        dmaCopy(src, BG_BITMAP_VRAM, 256 * 192);
    } else {
        for (int y = 0; y < 192; y++) {
            int sy = (y * bg.height) / 192;
            uint8_t* srcRow = src + sy * bg.width;
            uint8_t* dstRow = BG_BITMAP_VRAM + y * 256;
            for (int x = 0; x < 256; x++)
                dstRow[x] = srcRow[(x * bg.width) / 256];
        }
    }
}

void Renderer::renderUI(ScratchProject& project, InputHandler& input) {
    consoleSelect(&bottomConsole);
    consoleClear();
    printf("\x1b[0;0H--- Variables ---\n");
    int shown = 0;
    for (auto& sprite : project.targets) {
        for (auto& var : sprite.variables) {
            if (var.visible && shown < 16) {
                printf("%-10s: %s\n",
                    var.name.substr(0,10).c_str(),
                    var.value.substr(0,12).c_str());
                shown++;
            }
        }
    }
    if (input.isTouching())
        printf("\x1b[20;0HTouch: (%3d,%3d)\n",
               input.getTouchX(), input.getTouchY());
    printf("\x1b[22;0H[A]B[X][Y][L][R][^][v][<][>]");
}

// ─────────────────────────────────────────────────────────────────────────────
// OAM helpers
// ─────────────────────────────────────────────────────────────────────────────
SpriteSize Renderer::bestSpriteSize(int w, int h) {
    if (w<= 8&&h<= 8) return SpriteSize_8x8;
    if (w<=16&&h<= 8) return SpriteSize_16x8;
    if (w<= 8&&h<=16) return SpriteSize_8x16;
    if (w<=16&&h<=16) return SpriteSize_16x16;
    if (w<=32&&h<= 8) return SpriteSize_32x8;
    if (w<= 8&&h<=32) return SpriteSize_8x32;
    if (w<=32&&h<=16) return SpriteSize_32x16;
    if (w<=16&&h<=32) return SpriteSize_16x32;
    if (w<=32&&h<=32) return SpriteSize_32x32;
    if (w<=64&&h<=32) return SpriteSize_64x32;
    if (w<=32&&h<=64) return SpriteSize_32x64;
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
