// =============================================================================
// renderer.cpp
// =============================================================================
#include "renderer.h"
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

// Include lodepng for PNG loading:
// #define LODEPNG_NO_COMPILE_ENCODER   (saves RAM)
// #include "lodepng.h"
// For now we provide the interface stubs and a BMP fallback.

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

    oamInit(&oamMain, SpriteMapping_1D_32, false);

    consoleInit(&bottomConsole, 0, BgType_Text4bpp, BgSize_T_256x256, 2, 0, false, true);
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
        // SVG: too complex for NDS hardware — use a 16×16 pink placeholder square
        w = h = 16;
        gfx = (uint16_t*)calloc(w * h / 2, 1);
        pal = (uint16_t*)calloc(256 * 2, 1);
        if (gfx && pal) {
            memset(gfx, 0x11, w * h / 2);
            pal[1] = RGB15(31, 0, 31);
            ok = true;
        }
    }

    if (!ok || !gfx || !pal) return;

    // Clamp to NDS max sprite size (64×64)
    if (w > 64) w = 64;
    if (h > 64) h = 64;

    SpriteSize sz = bestSpriteSize(w, h);
    int sw, sh;
    getSpriteSize(sz, sw, sh);

    uint16_t* vramGfx = oamAllocateGfx(&oamMain, sz, SpriteColorFormat_256Color);
    if (!vramGfx) { free(gfx); free(pal); return; }

    dmaCopy(gfx, vramGfx, sw * sh);
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
// Render one frame — place all visible sprites as OAM objects
// -----------------------------------------------------------------------
void Renderer::renderFrame(ScratchProject& project) {
    oamClear(&oamMain, 0, 0);
    int oamIdx = 0;

    std::vector<ScratchSprite*> sorted;
    for (auto& s : project.targets) sorted.push_back(&s);
    std::sort(sorted.begin(), sorted.end(),
        [](ScratchSprite* a, ScratchSprite* b) {
            return a->layerOrder < b->layerOrder;
        });

    for (ScratchSprite* sprite : sorted) {
        if (!sprite->visible || sprite->isStage) continue;
        if (sprite->costumes.empty()) continue;
        if (oamIdx >= MAX_OAM_SPRITES) break;

        ScratchCostume& costume = sprite->costumes[sprite->currentCostume];
        if (!costume.gfxPtr) continue;

        double scaledX = sprite->x * STAGE_SCALE_X;
        double scaledY = -sprite->y * STAGE_SCALE_Y;
        int screenX = (int)(NDS_CENTER_X + scaledX) - costume.width  / 2;
        int screenY = (int)(NDS_CENTER_Y + scaledY) - costume.height / 2;

        bool scaled   = (sprite->size != 100.0);
        int  affineIdx = -1;

        if (scaled && oamIdx < 32) {
            double sc = sprite->size / 100.0;
            affineIdx = oamIdx % 32;
            int32_t hdx = (int32_t)((1.0 / sc) * (1 << 8));
            oamAffineTransformation(&oamMain, affineIdx, hdx, 0, 0, hdx);
        }

        if (sprite->rotationStyle == "all around" && sprite->direction != 90) {
            double angle = sprite->direction - 90.0;
            double rad   = angle * M_PI / 180.0;
            double sc    = sprite->size / 100.0;
            int32_t cosA = (int32_t)(cos(rad) / sc * 256);
            int32_t sinA = (int32_t)(sin(rad) / sc * 256);
            if (affineIdx < 0) affineIdx = oamIdx % 32;
            oamAffineTransformation(&oamMain, affineIdx, cosA, sinA, -sinA, cosA);
        }

        SpriteSize sz = bestSpriteSize(costume.width, costume.height);
        oamSet(&oamMain, oamIdx++,
               screenX, screenY,
               0,
               0,
               sz,
               SpriteColorFormat_256Color,
               costume.gfxPtr,
               affineIdx,
               (affineIdx >= 0),
               false,
               (sprite->rotationStyle == "left-right" && sprite->x < 0),
               false,
               false);

        if (!sprite->sayMessage.empty()) {
            // Say bubble would go here — write to sub-screen console
        }
    }

    ScratchSprite* stage = project.getStage();
    if (stage && !stage->costumes.empty()) {
        ScratchCostume& bg = stage->costumes[stage->currentCostume];
        if (bg.gfxPtr) {
            bgSetMapBase(bgHandle, 0);
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
    for (auto& sprite : project.targets) {
        for (auto& kv : sprite.variables) {
            if (kv.second.visible) {
                printf("%-10s: %s\n",
                    kv.second.name.substr(0, 10).c_str(),
                    kv.second.value.substr(0, 12).c_str());
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
// PNG loader stub
// -----------------------------------------------------------------------
bool Renderer::loadPng(const char* path, uint16_t** outGfx, uint16_t** outPal,
                        int* outW, int* outH) {
    // With lodepng:
    //   unsigned char* image; unsigned w, h;
    //   unsigned err = lodepng_decode32_file(&image, &w, &h, path);
    //   if (err) return false;
    //   ... quantize RGBA8 -> 8bpp indexed ...
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t* raw = (uint8_t*)malloc(sz);
    if (!raw) { fclose(f); return false; }
    fread(raw, 1, sz, f);
    fclose(f);

    *outW   = 32;
    *outH   = 32;
    *outGfx = (uint16_t*)malloc(32 * 32 / 2);
    *outPal = (uint16_t*)calloc(256, 2);

    if (*outGfx && *outPal) {
        memset(*outGfx, 1, 32 * 32 / 2);
        (*outPal)[0] = 0;
        (*outPal)[1] = RGB15(raw[0] >> 3, raw[1] >> 3, raw[2] >> 3);
    }
    free(raw);
    return (*outGfx != nullptr);
}

// -----------------------------------------------------------------------
// BMP loader (24-bit uncompressed)
// -----------------------------------------------------------------------
bool Renderer::loadBmp(const char* path, uint16_t** outGfx, uint16_t** outPal,
                        int* outW, int* outH) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;

    uint8_t hdr[54];
    if (fread(hdr, 1, 54, f) < 54) { fclose(f); return false; }
    if (hdr[0] != 'B' || hdr[1] != 'M') { fclose(f); return false; }

    int w   = *(int32_t*)(hdr + 18);
    int h   = *(int32_t*)(hdr + 22);
    int bpp = *(uint16_t*)(hdr + 28);
    if (h < 0) h = -h;
    if (bpp != 24) { fclose(f); return false; }

    // Clamp to NDS max sprite size — both clamps on separate lines
    if (w > 64) w = 64;
    if (h > 64) h = 64;

    *outGfx = (uint16_t*)malloc(w * h / 2 + 1);
    *outPal = (uint16_t*)calloc(256, 2);
    *outW   = w;
    *outH   = h;

    if (!*outGfx || !*outPal) { fclose(f); return false; }

    int palCount = 1;
    (*outPal)[0] = 0; // index 0 = transparent

    int     stride   = ((w * 3 + 3) & ~3);
    uint8_t* row     = (uint8_t*)malloc(stride);
    uint8_t* pixels8 = (uint8_t*)*outGfx;

    if (!row) { fclose(f); return false; }

    for (int y = h - 1; y >= 0; y--) { // BMP rows are stored bottom-up
        fread(row, 1, stride, f);
        for (int x = 0; x < w; x++) {
            uint8_t  b   = row[x * 3 + 0];
            uint8_t  g   = row[x * 3 + 1];
            uint8_t  r   = row[x * 3 + 2];
            uint16_t c15 = RGB15(r >> 3, g >> 3, b >> 3);

            int idx = 0;
            for (int p = 1; p < palCount; p++) {
                if ((*outPal)[p] == c15) { idx = p; break; }
            }
            if (idx == 0 && palCount < 256) {
                (*outPal)[palCount] = c15;
                idx = palCount++;
            }
            pixels8[y * w + x] = (uint8_t)idx;
        }
    }
    free(row);
    fclose(f);
    return true;
}

// -----------------------------------------------------------------------
// Best OAM sprite size for given pixel dimensions
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
        case SpriteSize_8x8:   w =  8; h =  8; return;
        case SpriteSize_16x8:  w = 16; h =  8; return;
        case SpriteSize_8x16:  w =  8; h = 16; return;
        case SpriteSize_16x16: w = 16; h = 16; return;
        case SpriteSize_32x8:  w = 32; h =  8; return;
        case SpriteSize_8x32:  w =  8; h = 32; return;
        case SpriteSize_32x16: w = 32; h = 16; return;
        case SpriteSize_16x32: w = 16; h = 32; return;
        case SpriteSize_32x32: w = 32; h = 32; return;
        case SpriteSize_64x32: w = 64; h = 32; return;
        case SpriteSize_32x64: w = 32; h = 64; return;
        default:               w = 64; h = 64; return;
    }
}
