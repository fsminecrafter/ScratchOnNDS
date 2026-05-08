// =============================================================================
// tests/stubs/nds.h
// Minimal NDS API stubs so that core logic (vm.cpp, project.cpp) can be
// compiled and tested on a host Linux machine without devkitARM.
//
// Only the symbols actually referenced by the files under test are stubbed.
// Hardware-only code in source files is guarded by #ifndef SCRATCHDS_HOST_BUILD.
// =============================================================================
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// ── Type aliases ─────────────────────────────────────────────────────────────
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef volatile u16 vu16;
typedef volatile u32 vu32;

// ── Key masks (from libnds) ───────────────────────────────────────────────────
#define KEY_A       (1 << 0)
#define KEY_B       (1 << 1)
#define KEY_SELECT  (1 << 2)
#define KEY_START   (1 << 3)
#define KEY_RIGHT   (1 << 4)
#define KEY_LEFT    (1 << 5)
#define KEY_UP      (1 << 6)
#define KEY_DOWN    (1 << 7)
#define KEY_R       (1 << 8)
#define KEY_L       (1 << 9)
#define KEY_X       (1 << 10)
#define KEY_Y       (1 << 11)
#define KEY_TOUCH   (1 << 12)
#define KEY_LID     (1 << 23)

// ── Touch ────────────────────────────────────────────────────────────────────
typedef struct { int px, py, rawx, rawy; } touchPosition;
static inline void touchRead(touchPosition* t) { (void)t; }
static inline void touchInit(void) {}

// ── OAM / sprite stubs ───────────────────────────────────────────────────────
typedef enum { SpriteMapping_1D_32 } OAMMappingMode;
typedef enum {
    SpriteSize_8x8, SpriteSize_16x8, SpriteSize_8x16, SpriteSize_16x16,
    SpriteSize_32x8, SpriteSize_8x32, SpriteSize_32x16, SpriteSize_16x32,
    SpriteSize_32x32, SpriteSize_64x32, SpriteSize_32x64, SpriteSize_64x64
} SpriteSize;
typedef enum { SpriteColorFormat_256Color } SpriteColorFormat;
typedef struct { int dummy; } OAMTable;
static OAMTable oamMain, oamSub;
static inline void oamInit(OAMTable*, OAMMappingMode, bool) {}
static inline void oamClear(OAMTable*, int, int) {}
static inline void oamUpdate(OAMTable*) {}
static inline void oamSet(OAMTable*, int, int, int, int, int,
                           SpriteSize, SpriteColorFormat, void*,
                           int, bool, bool, bool, bool, bool) {}
static inline u16* oamAllocateGfx(OAMTable*, SpriteSize, SpriteColorFormat)
    { return (u16*)malloc(64*64); }
static inline void oamAffineTransformation(OAMTable*, int,
    int32_t, int32_t, int32_t, int32_t) {}

// ── Background stubs ─────────────────────────────────────────────────────────
typedef enum { BgType_Text4bpp } BgType;
typedef enum { BgSize_T_256x256 } BgSize;
static inline int bgInit(int, BgType, BgSize, int, int) { return 0; }
static inline void bgSetMapBase(int, int) {}

// ── Console ──────────────────────────────────────────────────────────────────
typedef struct { int dummy; } PrintConsole;
static inline PrintConsole* consoleInit(PrintConsole* c, int, BgType, BgSize,
                                         int, int, bool, bool) { return c; }
static inline void consoleSelect(PrintConsole*) {}
static inline void consoleClear(void) {}
static inline void consoleDemoInit(void) {}
#define iprintf(...) printf(__VA_ARGS__)

// ── Video / VRAM stubs ────────────────────────────────────────────────────────
typedef enum { MODE_5_2D, MODE_0_2D } VideoMode;
typedef enum {
    VRAM_A_MAIN_BG, VRAM_B_MAIN_SPRITE,
    VRAM_C_SUB_BG,  VRAM_D_SUB_SPRITE,
    VRAM_A_SUB_BG_0x06200000, VRAM_B_SUB_SPRITE,
    VRAM_C_MAIN_BG, VRAM_D_MAIN_SPRITE
} VRAMBankEnum;
static inline void videoSetMode(VideoMode) {}
static inline void videoSetModeSub(VideoMode) {}
static inline void vramSetBankA(VRAMBankEnum) {}
static inline void vramSetBankB(VRAMBankEnum) {}
static inline void vramSetBankC(VRAMBankEnum) {}
static inline void vramSetBankD(VRAMBankEnum) {}

// ── DMA ──────────────────────────────────────────────────────────────────────
static inline void dmaCopy(const void* src, void* dst, size_t len)
    { memcpy(dst, src, len); }

// ── Timing ───────────────────────────────────────────────────────────────────
typedef enum { ClockDivider_1024 } ClockDivider;
#define TIMER_FREQ_1024(fps) (fps)
#define FIFO_MAXMOD 0
static inline void timerStart(int, ClockDivider, int, void*) {}
static inline int  timerElapsed(int) { return 0; }
static inline void swiWaitForVBlank(void) {}

// ── Power ────────────────────────────────────────────────────────────────────
#define POWER_ALL  0
#define POWER_LCD  0
#define POWER_2D_A 0
static inline void powerOn(int) {}
static inline void powerOff(int) {}

// ── Mic stubs ─────────────────────────────────────────────────────────────────
#define MIC_AMP_ON  1
#define MIC_AMP_OFF 0
static inline void micSetAmp(int, int) {}
static inline void micReadData(void*, int, bool) {}

// ── Misc ──────────────────────────────────────────────────────────────────────
static inline void scanKeys(void) {}
static inline u32  keysDown(void) { return 0; }
static inline u32  keysHeld(void) { return 0; }
static inline u32  keysUp(void)   { return 0; }
#define RGB15(r,g,b) ((u16)((r)|((g)<<5)|((b)<<10)))
#define SPRITE_PALETTE ((u16*)malloc(512))
#define REG_MASTER_BRIGHT_SUB (*(vu16*)malloc(2))

// maxmod types/functions are in maxmod9.h stub — included separately
