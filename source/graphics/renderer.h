// =============================================================================
// renderer.h — Fixed: clearBottomConsole, loadBmp signature, backdrop flag
// =============================================================================
#pragma once
#include "../core/project.h"
#include <nds.h>

#define MAX_OAM_SPRITES 128
#define MAX_GFX_SLOTS   64
#define SHARED_PAL_SIZE 256

// Scratch stage 480×360 → NDS top screen 256×192
#define STAGE_SCALE_X (256.0 / 480.0)
#define STAGE_SCALE_Y (192.0 / 360.0)
#define NDS_CENTER_X 128
#define NDS_CENTER_Y 96

struct SpriteSlot {
    bool      used;
    int       oamIndex;
    uint16_t* gfxVram;
    uint16_t* palVram;
    int       width, height;
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

    // Draw UI on bottom screen — clears fully before writing
    void renderUI(ScratchProject& project, class InputHandler& input);

    // Explicitly clear bottom console (call when menu opens/closes)
    void clearBottomConsole();

    // Expose sub-screen console so main.cpp / overlay_menu can select it
    PrintConsole* getBottomConsole() { return &bottomConsole; }

private:
    Renderer() {}

    void loadCostume(ScratchCostume& costume, const char* extractDir,
                     const std::string& format);
    bool loadPng(const char* path, uint16_t** outGfx, uint16_t** outPal,
                 int* outW, int* outH,
                 int maxW = 64, int maxH = 64);

    // maxW/maxH: maximum output pixel dimensions (64 for sprites, 256×192 for backdrops)
    bool loadBmp(const char* path, uint16_t** outGfx, uint16_t** outPal,
                 int* outW, int* outH,
                 int maxW = 64, int maxH = 64);

    bool loadSvg(const char* path, uint16_t** outGfx, uint16_t** outPal,
                 int* outW, int* outH, int dstW, int dstH);

    void renderBackdrop(ScratchProject& project);

    SpriteSize bestSpriteSize(int w, int h);
    void getSpriteSize(SpriteSize sz, int& w, int& h);

    // OAM slot allocator
    bool      oamUsed[MAX_OAM_SPRITES];
    int       nextOam;

    // VRAM sprite GFX allocator
    uint16_t* gfxBases[MAX_GFX_SLOTS];
    bool      gfxUsed[MAX_GFX_SLOTS];

    // BG for backdrop
    int       bgHandle;
    uint16_t* bgGfxPtr;
    uint16_t* bgPalPtr;

    // Sub-screen console for UI
    PrintConsole bottomConsole;

    bool backdropLoaded;
};
