// =============================================================================
// renderer.h
// Global palette: 256 OAM palette entries shared across all sprites.
// Each costume is assigned a 16-color slot (max 16 sprites).
// Costumes that need more than 16 colors are quantized down during load.
// This eliminates the per-costume palette upload that was overwriting previous
// sprites' colors every frame.
// =============================================================================
#pragma once
#include "../core/project.h"
#include <nds.h>

#define MAX_OAM_SPRITES    128
#define MAX_GFX_SLOTS      64

// Global palette layout: 256 entries split into 16-color slots.
// Slot 0 is reserved (index 0 = transparent in every slot).
// Slots 1–15 are for costumes (15 unique palettes × 16 colors).
#define PAL_COLORS_PER_SLOT  16
#define PAL_MAX_SLOTS        16   // 16 × 16 = 256 entries
#define PAL_SLOT_UNASSIGNED  0xFF

// Scratch stage 480×360 → NDS top screen 256×192
#define STAGE_SCALE_X (256.0 / 480.0)
#define STAGE_SCALE_Y (192.0 / 360.0)
#define NDS_CENTER_X 128
#define NDS_CENTER_Y 96

class Renderer {
public:
    static Renderer& getInstance() {
        static Renderer inst;
        return inst;
    }

    void init();

    void loadSprites(ScratchProject& project);
    void renderFrame(ScratchProject& project);
    void renderUI(ScratchProject& project, class InputHandler& input);
    void clearBottomConsole();

    PrintConsole* getBottomConsole() { return &bottomConsole; }

    int getPalSlotsUsed() const {
        int n = 0;
        for (int i = 0; i < PAL_MAX_SLOTS; i++) if (palSlotUsed[i]) n++;
        return n;
    }

private:
    Renderer() {}

    // ---- Palette allocator ----
    // Global 256-entry OAM palette. Each sprite gets a 16-color slot.
    // We track which slots are in use.
    bool     palSlotUsed[PAL_MAX_SLOTS];   // slot 0 always used (transparent)
    int      nextPalSlot;
    int      allocPalSlot();               // returns slot index or -1 if full

    // Quantise an arbitrary RGBA buffer to exactly 16 colours (+ idx 0 = transparent).
    // Writes the 16 RGB555 colors into outPal[0..15] and fills outPx (w*h bytes,
    // values 0–15). outPal and outPx must be pre-allocated by caller.
    void quantise16(const uint8_t* rgba, int w, int h,
                    uint16_t outPal[16], uint8_t* outPx);

    // Upload one 16-color palette slot to OAM SPRITE_PALETTE.
    void uploadPalSlot(int slot, const uint16_t pal16[16]);

    // ---- Costume loading ----
    void loadCostume(ScratchCostume& costume, const char* extractDir,
                     const std::string& format);

    bool loadPng(const char* path, uint8_t** outPx, uint16_t outPal[16],
                 int* outW, int* outH, int maxW, int maxH);
    bool loadBmp(const char* path, uint8_t** outPx, uint16_t outPal[16],
                 int* outW, int* outH, int maxW, int maxH);
    bool loadSvg(const char* path, uint8_t** outPx, uint16_t outPal[16],
                 int* outW, int* outH, int dstW, int dstH);

    void renderBackdrop(ScratchProject& project);

    SpriteSize bestSpriteSize(int w, int h);
    void getSpriteSize(SpriteSize sz, int& w, int& h);

    bool      oamUsed[MAX_OAM_SPRITES];
    int       nextOam;

    int       bgHandle;
    bool      backdropLoaded;

    PrintConsole bottomConsole;
};
