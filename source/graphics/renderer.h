// =============================================================================
// renderer.h / renderer.cpp
// Renders the Scratch stage to the NDS top screen using OAM sprites
// Backdrops rendered as BG layers; sprites as OAM objects
// PNG/BMP loaded via lodepng (single-header, NDS-compatible subset)
// =============================================================================
#pragma once
#include "../core/project.h"
#include <nds.h>

// NDS sprite limits
#define MAX_OAM_SPRITES 128
#define MAX_GFX_SLOTS   64

//RAAAM

#define SHARED_PAL_SIZE 256
static uint16_t sharedSpritePal[SHARED_PAL_SIZE];
static int      sharedPalCount;

// Scratch stage is 480x360; NDS top screen is 256x192
// Scale factor: ~0.533x (we use 256/480 = 0.5333)
#define STAGE_SCALE_X (256.0 / 480.0)
#define STAGE_SCALE_Y (192.0 / 360.0)

// Scratch origin is center; NDS origin is top-left
#define NDS_CENTER_X 128
#define NDS_CENTER_Y 96

struct SpriteSlot {
    bool     used;
    int      oamIndex;
    uint16_t* gfxVram;
    uint16_t* palVram;
    int      width, height;
    SpriteSize oamSize;
};

class Renderer {
public:
    static Renderer& getInstance() {
        static Renderer inst;
        return inst;
    }

    void init();

    // Load all costume assets from SD into VRAM
    void loadSprites(ScratchProject& project);

    // Draw one frame (call during VBlank)
    void renderFrame(ScratchProject& project);

    // Draw UI on bottom screen: variable monitors, button hints, touch indicator
    void renderUI(ScratchProject& project, class InputHandler& input);

private:
    Renderer() {}

    void loadCostume(ScratchCostume& costume, const char* extractDir,
                     const std::string& format);
    bool loadPng(const char* path, uint16_t** outGfx, uint16_t** outPal,
                 int* outW, int* outH);
    bool loadBmp(const char* path, uint16_t** outGfx, uint16_t** outPal,
                 int* outW, int* outH);

    void placeSprite(int oamIdx, ScratchSprite& sprite);

    // OAM slot allocator
    int allocOamSlot();
    void freeOamSlot(int idx);
    bool oamUsed[MAX_OAM_SPRITES];
    int  nextOam;

    // VRAM sprite GFX allocator
    uint16_t* gfxBases[MAX_GFX_SLOTS];
    bool       gfxUsed[MAX_GFX_SLOTS];

    // BG for backdrop
    int bgHandle;
    uint16_t* bgGfxPtr;
    uint16_t* bgPalPtr;

    // Sub-screen console for UI
    PrintConsole bottomConsole;

    bool backdropLoaded;

    bool loadSvg(const char* path, uint16_t** outGfx, uint16_t** outPal,
                 int* outW, int* outH, int dstW, int dstH);

    void renderBackdrop(ScratchProject& project);

    // Sprite size LUT
    SpriteSize bestSpriteSize(int w, int h);
    void getSpriteSize(SpriteSize sz, int& w, int& h);
};
