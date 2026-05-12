// =============================================================================
// render_test.cpp
// Drop into source/core/
// Wire up by adding to main.cpp:
//   #include "core/render_test.h"
//   and call render_test_step() once per frame in mainLoop() before oamUpdate
// =============================================================================
#include <nds.h>
#include <stdio.h>

#define MAIN_BG_VRAM  ((volatile uint16_t*)0x06000000)

static int s_testFrame = 0;

static void fillRect16(volatile uint16_t* vram, int x, int y,
                       int w, int h, uint16_t col) {
    for (int row = y; row < y+h; row++)
        for (int col2 = x; col2 < x+w; col2++)
            vram[row*256 + col2] = col;
}

void render_test_step() {
    s_testFrame++;

    // ── Test 1: cycle a coloured square in top-left of BG2 bitmap.
    // Red/green/blue every 60 frames. If ANY colour appears on top screen,
    // the BG2 bitmap pipeline is alive.
    uint16_t colour;
    int phase = (s_testFrame / 60) % 3;
    if      (phase == 0) colour = RGB15(31,  0,  0);
    else if (phase == 1) colour = RGB15( 0, 31,  0);
    else                 colour = RGB15( 0,  0, 31);
    fillRect16(MAIN_BG_VRAM, 0, 0, 64, 64, colour);

    // ── Test 2: manually write one 8x8 8bpp tile into sprite VRAM and
    // one raw OAM entry. Bypasses libnds OAM helpers entirely.
    // If a white 8x8 square appears at (10,10), sprite hardware is alive.
    if (s_testFrame == 2) {
        // Tile data: fill tile 0 with palette index 1
        volatile uint8_t* tiles = (volatile uint8_t*)0x06200000;
        for (int i = 0; i < 64; i++) tiles[i] = 1;

        // Sprite palette entry 1 = white
        SPRITE_PALETTE[1] = RGB15(31, 31, 31);

        // Raw OAM entry 0 (four 16-bit attributes)
        volatile uint16_t* oam = (volatile uint16_t*)0x07000000;
        // attr0: y=10, normal mode, 256-color, square shape
        oam[0] = (10 & 0xFF) | (1 << 13);
        // attr1: x=10, no affine, 8x8 size
        oam[1] = (10 & 0x1FF);
        // attr2: tile index 0, priority 0, palette slot 0
        oam[2] = 0;
        // attr3: unused for non-affine
        oam[3] = 0;
    }

    // ── Test 3: dump key register values to bottom screen every 2 seconds
    if (s_testFrame % 120 == 1) {
        printf("\x1b[0;0H");
        printf("=== RENDER DIAG (frame %d)\n",  s_testFrame);
        printf("DISPCNT : %08lX\n", (unsigned long)REG_DISPCNT);
        printf("BG2CNT  : %04X\n",  (unsigned)REG_BG2CNT);
        printf("BG2PA   : %04X\n",  (unsigned)REG_BG2PA);
        printf("BG2PD   : %04X\n",  (unsigned)REG_BG2PD);
        // VRAM bank control registers
        printf("VRAMCNT_A: %02X\n", (unsigned)(*(volatile uint8_t*)0x04000240));
        printf("VRAMCNT_B: %02X\n", (unsigned)(*(volatile uint8_t*)0x04000241));
        printf("VRAMCNT_C: %02X\n", (unsigned)(*(volatile uint8_t*)0x04000242));
        // What is actually in BG VRAM at offset 0?
        printf("BG_VRAM[0]: %04X\n",(unsigned)MAIN_BG_VRAM[0]);
        printf("BG_VRAM[1]: %04X\n",(unsigned)MAIN_BG_VRAM[1]);
        // Sprite palette
        printf("SPR_PAL[1]: %04X\n",(unsigned)SPRITE_PALETTE[1]);
        // OAM slot 0 raw
        volatile uint16_t* oam = (volatile uint16_t*)0x07000000;
        printf("OAM[0]: %04X %04X %04X\n",
                (unsigned)oam[0],(unsigned)oam[1],(unsigned)oam[2]);
        // Sprite VRAM tile 0 first byte
        printf("SPR_VRAM[0]: %02X\n",
                (unsigned)(*(volatile uint8_t*)0x06200000));
    }
}
