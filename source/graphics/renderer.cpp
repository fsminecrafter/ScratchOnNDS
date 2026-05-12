// =============================================================================
// renderer.cpp
// Global 16-slot OAM palette system.
//
// The NDS OAM supports one 256-entry palette shared by all 256-colour sprites.
// We divide it into 16 slots of 16 colours each (16×16=256).
// Entry 0 of every slot is always 0 (transparent), giving 15 usable colours
// per sprite — sufficient for most Scratch costumes after quantisation.
//
// Each costume is quantised to 16 colours at load time.  The 16 palette entries
// are written once into SPRITE_PALETTE at the slot's base offset and never
// touched again.  OAM oamSet() uses the palette index = slot number.
//
// This replaces the old per-frame per-sprite SPRITE_PALETTE[0] upload which
// caused every previous sprite's colours to be overwritten.
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
#include <vector>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// VRAM pointers
#define BG_BITMAP_VRAM  ((uint8_t*)0x06000000)
#define BG_BMP16_VRAM   ((uint16_t*)0x06000000)

// -----------------------------------------------------------------------
// Palette allocator
// -----------------------------------------------------------------------
int Renderer::allocPalSlot() {
    // Slot 0 is reserved (transparent index in all slots).
    for (int i = 1; i < PAL_MAX_SLOTS; i++) {
        if (!palSlotUsed[i]) {
            palSlotUsed[i] = true;
            return i;
        }
    }
    return -1;  // all slots full
}

void Renderer::uploadPalSlot(int slot, const uint16_t pal16[16]) {
    // SPRITE_PALETTE is uint16_t[256].  Slot n occupies entries [n*16 .. n*16+15].
    uint16_t* base = SPRITE_PALETTE + slot * PAL_COLORS_PER_SLOT;
    dmaCopy(pal16, base, PAL_COLORS_PER_SLOT * sizeof(uint16_t));
}

// -----------------------------------------------------------------------
// quantise16 — reduce an RGBA image to 16 colours using median-cut lite.
// For speed we use a simple popularity / greedy approach:
//   1. Collect unique RGB555 colours (up to 15; transparent = slot 0).
//   2. If more than 15 unique colours, find nearest match.
// outPal[0] is always 0 (transparent).  outPx values are 0–15.
// -----------------------------------------------------------------------
void Renderer::quantise16(const uint8_t* rgba, int w, int h,
                           uint16_t outPal[16], uint8_t* outPx) {
    outPal[0] = 0;
    int palCount = 1;

    const int n = w * h;
    for (int i = 0; i < n; i++) {
        uint8_t r = rgba[i*4+0];
        uint8_t g = rgba[i*4+1];
        uint8_t b = rgba[i*4+2];
        uint8_t a = rgba[i*4+3];

        if (a < 128) { outPx[i] = 0; continue; }

        uint16_t c15 = RGB15(r >> 3, g >> 3, b >> 3);

        // Search existing palette entries
        int found = 0;
        for (int p = 1; p < palCount; p++) {
            if (outPal[p] == c15) { found = p; break; }
        }

        if (!found) {
            if (palCount < PAL_COLORS_PER_SLOT) {
                outPal[palCount] = c15;
                found = palCount++;
            } else {
                // Palette full — find nearest RGB555 match
                int best = 1, bestD = 0x7FFFFFFF;
                int tr = r >> 3, tg = g >> 3, tb = b >> 3;
                for (int p = 1; p < PAL_COLORS_PER_SLOT; p++) {
                    int pr = outPal[p] & 0x1F;
                    int pg = (outPal[p] >> 5) & 0x1F;
                    int pb = (outPal[p] >> 10) & 0x1F;
                    int d  = (tr-pr)*(tr-pr) + (tg-pg)*(tg-pg) + (tb-pb)*(tb-pb);
                    if (d < bestD) { bestD = d; best = p; }
                }
                found = best;
            }
        }
        outPx[i] = (uint8_t)found;
    }

    // Zero-fill unused palette entries
    for (int p = palCount; p < PAL_COLORS_PER_SLOT; p++) outPal[p] = 0;
}

// -----------------------------------------------------------------------
// Convert linear 8bpp to NDS tiled 8bpp (8×8 tiles)
// -----------------------------------------------------------------------
static void linearToTiled8(const uint8_t* src, uint8_t* dst, int w, int h) {
    int tilesX = w / 8;
    int tilesY = h / 8;
    int di = 0;
    for (int ty = 0; ty < tilesY; ty++)
        for (int tx = 0; tx < tilesX; tx++)
            for (int py = 0; py < 8; py++)
                for (int px = 0; px < 8; px++)
                    dst[di++] = src[(ty*8+py)*w + (tx*8+px)];
}

// -----------------------------------------------------------------------
// init
// -----------------------------------------------------------------------
void Renderer::init() {
    memset(oamUsed, 0, sizeof(oamUsed));
    nextOam = 0;
    lastOamCount = 0;
    backdropLoaded = false;
    memset(palSlotUsed, 0, sizeof(palSlotUsed));
    palSlotUsed[0] = true;
    memset(SPRITE_PALETTE, 0, 256 * sizeof(uint16_t));

    oamInit(&oamMain, SpriteMapping_1D_128, false);

    // Mode 5 main engine: BG2 is the 256x192 16-bit direct-color bitmap layer.
    // BG_BMP16_256x256 enables 16-bit color, BG_BMP_BASE(0) puts it at the
    // start of VRAM_A (0x06000000), priority 3 = behind sprites.
    REG_DISPCNT = MODE_5_2D | DISPLAY_BG2_ACTIVE | DISPLAY_SPR_ACTIVE | DISPLAY_SPR_1D_LAYOUT;
    bgHandle = 2;

    // Scale/rotation identity matrix for BG2 (required for bitmap layers)
    REG_BG2PA = 1 << 8;  // 1.0 in 8.8 fixed point
    REG_BG2PB = 0;
    REG_BG2PC = 0;
    REG_BG2PD = 1 << 8;  // 1.0 in 8.8 fixed point
    REG_BG2X = 0;
    REG_BG2Y = 0;

    dmaFillWords(0, BG_BMP16_VRAM, 256 * 192 * 2);
    
    // Set display control once: MODE_5 + BG2 + sprites (1D 128-byte for 8bpp)
    REG_DISPCNT = MODE_5_2D | DISPLAY_BG2_ACTIVE | DISPLAY_SPR_ACTIVE | DISPLAY_SPR_1D_LAYOUT;

    // Sub screen console for bottom display
    consoleInit(&bottomConsole, 0, BgType_Text4bpp, BgSize_T_256x256,
                2, 0, false, true);
}

void Renderer::clearBottomConsole() {
    consoleSelect(&bottomConsole);
    consoleClear();
}

// -----------------------------------------------------------------------
// loadSprites — iterate all costumes and load them
// -----------------------------------------------------------------------
void Renderer::loadSprites(ScratchProject& project) {
    for (auto& sprite : project.targets)
        for (auto& costume : sprite.costumes)
            loadCostume(costume, project.extractDir.c_str(), costume.dataFormat);
}

// -----------------------------------------------------------------------
// loadCostume — load one costume, assign a palette slot, upload to VRAM
// -----------------------------------------------------------------------
void Renderer::loadCostume(ScratchCostume& costume, const char* extractDir,
                             const std::string& format) {
    costume.gfxPtr   = nullptr;
    costume.palSlot  = -1;
    costume.isBackdrop = false;

    char path[512];
    snprintf(path, sizeof(path), "%s/%s.%s",
             extractDir, costume.assetId.c_str(), format.c_str());

    bool isLikelyBackdrop = (costume.rotationCenterX >= 180 &&
                              costume.rotationCenterY >= 130);

    if (isLikelyBackdrop) {
        // Backdrops: decode to RGB555 direct-colour buffer, no palette needed
        int dstW = 256, dstH = 192;
        uint16_t* buf = nullptr;
        int w = 0, h = 0;

        if (format == "png") {
            std::vector<unsigned char> img;
            unsigned sw = 0, sh = 0;
            if (lodepng::decode(img, sw, sh, path) == 0 && !img.empty()) {
                w = ((int)sw < dstW) ? (int)sw : dstW;
                h = ((int)sh < dstH) ? (int)sh : dstH;
                buf = (uint16_t*)malloc(dstW * dstH * sizeof(uint16_t));
                if (buf) {
                    memset(buf, 0, dstW * dstH * sizeof(uint16_t));
                    for (int y = 0; y < h; y++) {
                        int sy = (h != (int)sh) ? (int)((unsigned)y*sh/(unsigned)h) : y;
                        for (int x = 0; x < w; x++) {
                            int sx = (w != (int)sw) ? (int)((unsigned)x*sw/(unsigned)w) : x;
                            const unsigned char* p = img.data() + ((size_t)sy*sw+sx)*4;
                            if (p[3] >= 128)
                                buf[y*dstW+x] = RGB15(p[0]>>3, p[1]>>3, p[2]>>3);
                        }
                    }
                    w = dstW; h = dstH;
                }
            }
        }

        if (!buf) {
            // Placeholder grey backdrop
            buf = (uint16_t*)malloc(dstW * dstH * sizeof(uint16_t));
            if (buf) {
                uint16_t grey = RGB15(15,15,15);
                for (int i = 0; i < dstW*dstH; i++) buf[i] = grey;
                w = dstW; h = dstH;
            }
        }

        costume.gfxPtr    = buf;
        costume.palSlot   = -1;
        costume.width     = w;
        costume.height    = h;
        costume.isBackdrop = true;
        return;
    }

    // --- Sprite costume ---
    const int dstW = 64, dstH = 64;
    uint8_t*  px   = nullptr;
    uint16_t  pal16[PAL_COLORS_PER_SLOT];
    memset(pal16, 0, sizeof(pal16));
    int w = 0, h = 0;
    bool ok = false;

    if (format == "png")
        ok = loadPng(path, &px, pal16, &w, &h, dstW, dstH);
    else if (format == "bmp")
        ok = loadBmp(path, &px, pal16, &w, &h, dstW, dstH);
    else if (format == "svg")
        ok = loadSvg(path, &px, pal16, &w, &h, dstW, dstH);

    if (!ok || !px) {
        free(px);
        // Placeholder: 32×32 magenta block
        w = 32; h = 32;
        px = (uint8_t*)malloc(w * h);
        if (!px) return;
        memset(px, 1, w * h);
        memset(pal16, 0, sizeof(pal16));
        pal16[0] = 0;
        pal16[1] = (format == "svg") ? (uint16_t)RGB15(31,0,31)
                                      : (uint16_t)RGB15(0,31,31);
        ok = true;
    }

    // Clamp to sprite size limits
    if (w > dstW) w = dstW;
    if (h > dstH) h = dstH;

    SpriteSize sz = bestSpriteSize(w, h);
    int sw, sh;
    getSpriteSize(sz, sw, sh);

    // Allocate a global palette slot
    int slot = allocPalSlot();
    if (slot < 0) {
        // Palette full — reuse slot 1 as fallback (colours will clash but
        // won't crash; a real engine would merge palettes)
        slot = 1;
    }

    // Upload the 16 palette colours for this slot
    uploadPalSlot(slot, pal16);

    // Tile and upload pixel data
    uint16_t* vramGfx = oamAllocateGfx(&oamMain, sz, SpriteColorFormat_256Color);
    if (!vramGfx) { free(px); return; }

    // Remap pixel indices: our quantise16 used indices 0–15, but in a 256-colour
    // OAM the palette index for slot N is (N*16 + local_index).
    // We store the raw local index (0–15) here and pass palIndex=slot to oamSet,
    // which tells the hardware to start reading from entry slot*16.
    // So no remapping of pixel values is needed — oamSet's palIndex parameter
    // does the offset automatically for 256-colour sprites.
    // (For 256-colour sprites, palIndex selects which 16-entry "sub-palette"
    //  within the 256-entry table to use.)

    uint8_t* tiled = (uint8_t*)malloc(sw * sh);
    if (!tiled) { free(px); return; }
    memset(tiled, 0, sw * sh);

    // Only copy within the valid region (px is w×h, tile buffer is sw×sh)
    // We need a temporary padded buffer
    uint8_t* padded = (uint8_t*)calloc(sw * sh, 1);
    if (padded) {
        int copyW = w < sw ? w : sw;
        int copyH = h < sh ? h : sh;
        for (int y = 0; y < copyH; y++)
            for (int x = 0; x < copyW; x++)
                padded[y*sw + x] = px[y*w + x];
        linearToTiled8(padded, tiled, sw, sh);
        free(padded);
    }
    free(px);

    dmaCopy(tiled, vramGfx, sw * sh);
    free(tiled);

    costume.gfxPtr   = vramGfx;
    costume.palSlot  = slot;
    costume.width    = sw;
    costume.height   = sh;
    costume.isBackdrop = false;
}

// -----------------------------------------------------------------------
// loadPng — decode PNG, quantise to 16 colours
// -----------------------------------------------------------------------
bool Renderer::loadPng(const char* path, uint8_t** outPx, uint16_t outPal[16],
                        int* outW, int* outH, int maxW, int maxH) {
    std::vector<unsigned char> image;
    unsigned srcW = 0, srcH = 0;
    if (lodepng::decode(image, srcW, srcH, path) != 0 || image.empty())
        return false;

    int dstW = ((int)srcW < maxW) ? (int)srcW : maxW;
    int dstH = ((int)srcH < maxH) ? (int)srcH : maxH;

    // Downsample if needed
    std::vector<unsigned char> ds;
    const unsigned char* src = image.data();
    if (dstW != (int)srcW || dstH != (int)srcH) {
        ds.resize((size_t)dstW * dstH * 4);
        for (int y = 0; y < dstH; y++) {
            int sy = (int)((unsigned)y * srcH / (unsigned)dstH);
            for (int x = 0; x < dstW; x++) {
                int sx = (int)((unsigned)x * srcW / (unsigned)dstW);
                const unsigned char* s = image.data() + ((size_t)sy*srcW+sx)*4;
                unsigned char* d = ds.data() + ((size_t)y*dstW+x)*4;
                d[0]=s[0]; d[1]=s[1]; d[2]=s[2]; d[3]=s[3];
            }
        }
        image.clear(); image.shrink_to_fit();
        src = ds.data();
    }

    uint8_t* px = (uint8_t*)malloc((size_t)dstW * dstH);
    if (!px) return false;

    quantise16(src, dstW, dstH, outPal, px);

    *outPx = px;
    *outW  = dstW;
    *outH  = dstH;
    return true;
}

// -----------------------------------------------------------------------
// loadSvg — placeholder pink square, 16 colours
// -----------------------------------------------------------------------
bool Renderer::loadSvg(const char* path, uint8_t** outPx, uint16_t outPal[16],
                        int* outW, int* outH, int dstW, int dstH) {
    (void)path;
    if (dstW > 64) dstW = 64;
    if (dstH > 64) dstH = 64;

    uint8_t* px = (uint8_t*)malloc(dstW * dstH);
    if (!px) return false;
    memset(px, 1, dstW * dstH);

    memset(outPal, 0, PAL_COLORS_PER_SLOT * sizeof(uint16_t));
    outPal[0] = 0;
    outPal[1] = RGB15(31, 0, 31);  // magenta placeholder

    *outPx = px;
    *outW  = dstW;
    *outH  = dstH;
    return true;
}

// -----------------------------------------------------------------------
// loadBmp — uncompressed BMP, quantise result to 16 colours
// -----------------------------------------------------------------------
bool Renderer::loadBmp(const char* path, uint8_t** outPx, uint16_t outPal[16],
                        int* outW, int* outH, int maxW, int maxH) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;

    uint8_t hdr[54];
    if (fread(hdr, 1, 54, f) < 54 || hdr[0]!='B' || hdr[1]!='M') {
        fclose(f); return false;
    }

    uint32_t dataOfs = hdr[10]|(hdr[11]<<8)|(hdr[12]<<16)|(hdr[13]<<24);
    int32_t  srcW    = hdr[18]|(hdr[19]<<8)|(hdr[20]<<16)|(hdr[21]<<24);
    int32_t  srcH    = hdr[22]|(hdr[23]<<8)|(hdr[24]<<16)|(hdr[25]<<24);
    uint16_t bpp     = hdr[28]|(hdr[29]<<8);
    uint32_t comp    = hdr[30]|(hdr[31]<<8)|(hdr[32]<<16)|(hdr[33]<<24);
    uint32_t clrUsed = hdr[46]|(hdr[47]<<8)|(hdr[48]<<16)|(hdr[49]<<24);

    if (comp != 0 || (bpp != 24 && bpp != 32 && bpp != 8)) {
        fclose(f); return false;
    }

    bool flipped = (srcH > 0);
    if (srcH < 0) srcH = -srcH;
    if (srcW <= 0) { fclose(f); return false; }

    // Read BMP palette if 8-bit
    uint16_t bmpPal[256] = {};
    if (bpp == 8) {
        int pe = (int)(clrUsed ? clrUsed : 256);
        if (pe > 256) pe = 256;
        fseek(f, 54, SEEK_SET);
        for (int i = 0; i < pe; i++) {
            uint8_t q[4];
            if (fread(q, 1, 4, f) < 4) break;
            bmpPal[i] = RGB15(q[2]>>3, q[1]>>3, q[0]>>3);
        }
    }

    fseek(f, (long)dataOfs, SEEK_SET);

    int dstW = srcW < maxW ? srcW : maxW;
    int dstH = srcH < maxH ? srcH : maxH;

    // Decode to RGBA first, then quantise
    int stride = (((srcW * bpp) + 31) / 32) * 4;
    uint8_t* rowBuf = (uint8_t*)malloc(stride);
    uint8_t* rgba   = (uint8_t*)malloc((size_t)dstW * dstH * 4);
    if (!rowBuf || !rgba) { free(rowBuf); free(rgba); fclose(f); return false; }

    for (int y = 0; y < srcH; y++) {
        fread(rowBuf, 1, stride, f);
        if (y >= dstH) continue;
        int dstY = flipped ? (dstH-1-y) : y;
        if (dstY < 0 || dstY >= dstH) continue;
        uint8_t* row = rgba + (size_t)dstY * dstW * 4;

        for (int x = 0; x < dstW; x++) {
            int sx = (srcW > dstW) ? (x * srcW / dstW) : x;
            if (sx >= srcW) sx = srcW-1;
            uint8_t r, g, b, a = 255;
            if (bpp == 8) {
                uint16_t c = bmpPal[rowBuf[sx]];
                r = (c & 0x1F) << 3;
                g = ((c>>5) & 0x1F) << 3;
                b = ((c>>10) & 0x1F) << 3;
            } else {
                int bs = bpp/8;
                b = rowBuf[sx*bs+0];
                g = rowBuf[sx*bs+1];
                r = rowBuf[sx*bs+2];
                if (bpp == 32) a = rowBuf[sx*bs+3];
            }
            row[x*4+0]=r; row[x*4+1]=g; row[x*4+2]=b; row[x*4+3]=a;
        }
    }
    free(rowBuf);
    fclose(f);

    uint8_t* px = (uint8_t*)malloc((size_t)dstW * dstH);
    if (!px) { free(rgba); return false; }

    quantise16(rgba, dstW, dstH, outPal, px);
    free(rgba);

    *outPx = px;
    *outW  = dstW;
    *outH  = dstH;
    return true;
}

// -----------------------------------------------------------------------
// renderFrame
// -----------------------------------------------------------------------
void Renderer::renderFrame(ScratchProject& project) {
    renderBackdrop(project);

    oamClear(&oamMain, 0, 0);
    int oamIdx = 0;

    std::vector<ScratchSprite*> sorted;
    for (auto& s : project.targets)
        if (!s.isStage) sorted.push_back(&s);
    std::sort(sorted.begin(), sorted.end(),
        [](ScratchSprite* a, ScratchSprite* b) {
            return a->layerOrder < b->layerOrder;
        });

    for (ScratchSprite* sprite : sorted) {
        if (!sprite->visible || sprite->costumes.empty()) continue;
        if (oamIdx >= MAX_OAM_SPRITES) break;

        ScratchCostume& costume = sprite->costumes[sprite->currentCostume];
        if (!costume.gfxPtr || costume.isBackdrop || costume.palSlot < 0) continue;

        double scaledX = sprite->x * STAGE_SCALE_X;
        double scaledY = -sprite->y * STAGE_SCALE_Y;
        int screenX = (int)(NDS_CENTER_X + scaledX) - costume.width  / 2;
        int screenY = (int)(NDS_CENTER_Y + scaledY) - costume.height / 2;

        bool scaled  = (sprite->size != 100.0);
        bool rotated = (sprite->rotationStyle == "all around" && sprite->direction != 90);
        int affineIdx = -1;

        if ((scaled || rotated) && oamIdx < 32) {
            affineIdx = oamIdx % 32;
            double sc = sprite->size / 100.0;
            if (sc < 0.01) sc = 0.01;
            double angle = (sprite->rotationStyle == "all around") ?
                           (sprite->direction - 90.0) * M_PI / 180.0 : 0.0;
            int32_t cosA = (int32_t)(cos(angle) / sc * 256);
            int32_t sinA = (int32_t)(sin(angle) / sc * 256);
            oamAffineTransformation(&oamMain, affineIdx, cosA, sinA, -sinA, cosA);
        }

        bool hFlip = (sprite->rotationStyle == "left-right" && sprite->direction < 0);
        SpriteSize sz = bestSpriteSize(costume.width, costume.height);

        // palIndex = slot number (hardware multiplies by 16 internally for 256-colour mode)
        oamSet(&oamMain, oamIdx++,
               screenX, screenY,
               0,                          // priority
               costume.palSlot,            // palette slot (0–15)
               sz,
               SpriteColorFormat_256Color,
               costume.gfxPtr,
               affineIdx, (affineIdx >= 0),
               false,                      // disabled
               hFlip, false,               // hFlip, vFlip
               false);                     // mosaic
    }

    lastOamCount = oamIdx;
}

// -----------------------------------------------------------------------
// renderBackdrop
// -----------------------------------------------------------------------

void Renderer::renderBackdrop(ScratchProject& project) {

    ScratchSprite* stage = project.getStage();
    if (!stage || stage->costumes.empty()) {
        dmaFillWords(0, BG_BMP16_VRAM, 256 * 192 * 2);
        return;
    }
    ScratchCostume& bg = stage->costumes[stage->currentCostume];
    if (!bg.gfxPtr || !bg.isBackdrop) {
        uint16_t white = RGB15(31, 31, 31);
        for (int i = 0; i < 256 * 192; i++) BG_BMP16_VRAM[i] = white;
        return;
    }

    uint16_t* src = bg.gfxPtr;
    if (bg.width == 256 && bg.height == 192) {
        dmaCopy(src, BG_BMP16_VRAM, 256 * 192 * 2);
    } else {
        for (int y = 0; y < 192; y++) {
            int sy = (y * bg.height) / 192;
            uint16_t* srcRow = src + sy * bg.width;
            uint16_t* dstRow = BG_BMP16_VRAM + y * 256;
            for (int x = 0; x < 256; x++)
                dstRow[x] = srcRow[(x * bg.width) / 256];
        }
    }
}

// -----------------------------------------------------------------------
// renderUI
// -----------------------------------------------------------------------
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

// -----------------------------------------------------------------------
// OAM helpers
// -----------------------------------------------------------------------
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
