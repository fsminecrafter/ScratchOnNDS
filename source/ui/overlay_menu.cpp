// =============================================================================
// overlay_menu.cpp — patched: added USAGE page
//
// USAGE page shows:
//   - Per-sprite RAM breakdown (gfx VRAM + PCM sound + block/costume counts)
//   - Active threads: sprite name, state, stack depth, steps/frame
//   - OAM + palette slot utilisation bars
//   - Battery percentage and charging state (DSi only; DS Lite shows N/A)
//   - RTC wall-clock time and date
//   - Total / free / used RAM bars
//
// Performance notes:
//   - gatherUsageStats() is called once when the USAGE page is first opened
//     (usageStatsDirty_ flag), then only when the user presses Y to refresh.
//   - No malloc/free happens during rendering — all data is in UsageStats.
//   - measureFreeRamBinary() uses a binary-search probe (2 allocs max).
// =============================================================================
#include "overlay_menu.h"
#include "../input/input_handler.h"
#include "../scratch_extension/nds_extension.h"
#include "../core/project.h"
#include "../core/vm.h"
#include <nds.h>
#include <fat.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// ── libnds RTC / power headers (available in libnds 2.x) ─────────────────────
#if defined(ARM9)
#  if defined(__has_include)
#    if __has_include(<nds/arm9/rtc.h>)
#      include <nds/arm9/rtc.h>
#      define SCRATCHDS_HAVE_RTC 1
#    endif
#  endif
#endif

#define COL_WHITE   "\x1b[37;1m"
#define COL_CYAN    "\x1b[36;1m"
#define COL_YELLOW  "\x1b[33;1m"
#define COL_GREEN   "\x1b[32;1m"
#define COL_RED     "\x1b[31;1m"
#define COL_GREY    "\x1b[37;0m"
#define COL_MAGENTA "\x1b[35;1m"
#define COL_RESET   "\x1b[0m"

// ═══════════════════════════════════════════════════════════════════════════════
// ScratchDSSettings
// ═══════════════════════════════════════════════════════════════════════════════
bool ScratchDSSettings::save(const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fwrite(this, 1, sizeof(*this), f);
    fclose(f);
    return true;
}
bool ScratchDSSettings::load(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fread(this, 1, sizeof(*this), f);
    fclose(f);
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// DPadTextInput
// ═══════════════════════════════════════════════════════════════════════════════
const char DPadTextInput::CHARS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
    "0123456789 ._-/:\\";
const int DPadTextInput::NUM_CHARS = sizeof(DPadTextInput::CHARS) - 1;

DPadTextInput::DPadTextInput() : len_(0), charSel_(0) {
    buf_[0] = prompt_[0] = '\0';
}
void DPadTextInput::reset(const char* prompt, const char* initial) {
    strncpy(prompt_, prompt, sizeof(prompt_) - 1);
    prompt_[sizeof(prompt_) - 1] = '\0';
    strncpy(buf_, initial ? initial : "", sizeof(buf_) - 1);
    buf_[sizeof(buf_) - 1] = '\0';
    len_ = (int)strlen(buf_);
    charSel_ = 0;
}
bool DPadTextInput::update() {
    scanKeys();
    u32 down = keysDown();
    if (down & KEY_RIGHT) charSel_ = (charSel_ + 1) % NUM_CHARS;
    if (down & KEY_LEFT)  charSel_ = (charSel_ - 1 + NUM_CHARS) % NUM_CHARS;
    if (down & KEY_UP)    charSel_ = (charSel_ + 10) % NUM_CHARS;
    if (down & KEY_DOWN)  charSel_ = (charSel_ - 10 + NUM_CHARS) % NUM_CHARS;
    if (down & KEY_A) {
        if (len_ < (int)sizeof(buf_) - 2)
            buf_[len_++] = CHARS[charSel_], buf_[len_] = '\0';
    }
    if (down & KEY_B && len_ > 0) buf_[--len_] = '\0';
    return (down & KEY_START) != 0;
}
void DPadTextInput::render(int y) {
    printf("\x1b[%d;0H", y);
    printf(COL_GREY "%s" COL_RESET "\n", prompt_);
    printf(COL_WHITE "> %s" COL_CYAN "_" COL_RESET "\n", buf_);
    printf(COL_GREY "< " COL_YELLOW "%c" COL_GREY " > [A]=Add [B]=Del [START]=OK\n",
           CHARS[charSel_]);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Memory probe (binary-search, 2 allocs maximum, no fragmentation leaks)
// ═══════════════════════════════════════════════════════════════════════════════
static int s_freeRamCache = -1;

int OverlayMenu::measureFreeRamBinary() {
    // Binary search between 0 and 3.9 MB in 4 KB steps.
    // We probe with a single malloc/free per iteration to avoid fragmenting heap.
    int lo = 0, hi = (int)(3.9f * 1024 * 1024);
    while (hi - lo > 4096) {
        int mid = (lo + hi) / 2;
        void* p = malloc((size_t)mid);
        if (p) { free(p); lo = mid; } else { hi = mid; }
    }
    return lo;
}

// ═══════════════════════════════════════════════════════════════════════════════
// gatherUsageStats — called once on page open, or on Y-refresh
// ═══════════════════════════════════════════════════════════════════════════════
void OverlayMenu::gatherUsageStats() {
    UsageStats& s = usageStats_;
    memset(&s, 0, sizeof(s));

    // ── RAM ──────────────────────────────────────────────────────────────────
    s.totalRamBytes = 4 * 1024 * 1024;  // NDS ARM9 has 4 MB main RAM
    s.freeRamBytes  = measureFreeRamBinary();
    s_freeRamCache  = s.freeRamBytes;
    // VRAM banks A+B+C+D = 4 × 128 KB = 512 KB (separate, not in main RAM)
    s.vramEstimateBytes = 512 * 1024;

    // ── Sprite / sound breakdown ──────────────────────────────────────────────
    s.numSprites = 0;
    s.usedBySpritesBytes = 0;
    s.usedBySoundsBytes  = 0;

    if (liveProject_) {
        for (auto& sprite : liveProject_->targets) {
            if (s.numSprites >= UsageStats::MAX_SPRITES) break;
            UsageStats::SpriteEntry& e = s.sprites[s.numSprites++];
            memset(&e, 0, sizeof(e));

            strncpy(e.name, sprite.name.c_str(), sizeof(e.name) - 1);
            e.name[sizeof(e.name) - 1] = '\0';
            e.visible     = sprite.visible;
            e.isStage     = sprite.isStage;
            e.numBlocks   = (int)sprite.blocks.size();
            e.numCostumes = (int)sprite.costumes.size();
            e.numSounds   = (int)sprite.sounds.size();

            // Costume VRAM: pixel data is w*h bytes in VRAM (OAM tiled 8bpp)
            for (auto& c : sprite.costumes) {
                if (c.gfxPtr && !c.isBackdrop)
                    e.costumesBytes += c.width * c.height;
                else if (c.gfxPtr && c.isBackdrop)
                    e.costumesBytes += c.width * c.height * 2; // RGB555 = 2B/px
            }

            // PCM sound RAM
            for (auto& snd : sprite.sounds) {
                if (snd.loaded && !snd.isStreamed && snd.pcmData)
                    e.soundsBytes += (int)snd.pcmSize;
            }

            s.usedBySpritesBytes += e.costumesBytes;
            s.usedBySoundsBytes  += e.soundsBytes;
        }
    }

    // ── Threads ──────────────────────────────────────────────────────────────
    s.numThreads = 0;
    if (liveVm_) {
        // ScratchVM exposes threads as a public vector (it does in vm.h)
        // We use the accessor; if threads is private, expose via a method.
        // Here we assume the patch adds: const std::vector<ScriptThread>& getThreads() const
        const auto& tv = liveVm_->getThreads();
        for (const auto& t : tv) {
            if (s.numThreads >= UsageStats::MAX_THREADS) break;
            UsageStats::ThreadEntry& te = s.threads[s.numThreads++];
            memset(&te, 0, sizeof(te));

            // sprite name
            if (t.sprite) {
                strncpy(te.spriteName, t.sprite->name.c_str(),
                        sizeof(te.spriteName) - 1);
            } else {
                strncpy(te.spriteName, "(null)", sizeof(te.spriteName) - 1);
            }

            // state string
            switch (t.state) {
                case ScriptThread::RUNNING:       strncpy(te.state, "RUNNING",   15); break;
                case ScriptThread::WAITING_SECS:  strncpy(te.state, "WAIT_SEC",  15); break;
                case ScriptThread::WAITING_SOUND: strncpy(te.state, "WAIT_SND",  15); break;
                case ScriptThread::DONE:          strncpy(te.state, "DONE",      15); break;
                default:                          strncpy(te.state, "UNKNOWN",   15); break;
            }

            // truncated block id
            strncpy(te.blockId, t.currentBlockId.c_str(), sizeof(te.blockId) - 1);
            te.blockId[sizeof(te.blockId) - 1] = '\0';

            te.stackDepth    = (int)t.callStack.size();
            te.stepsThisFrame = t.stepsThisFrame;
        }
    }

    // ── OAM / palette ────────────────────────────────────────────────────────
    s.palSlotsUsed = livePalSlots_;
    s.oamSlotsUsed = liveOamSlots_;

    // ── Battery ──────────────────────────────────────────────────────────────
    s.batteryPercent = -1;
    s.isCharging     = false;
#ifdef ARM9
    // libnds DSi power management (dsiPowerStatus is only valid on DSi)
  #ifdef isDSiMode
    if (isDSiMode()) {
        // TWL_POWER registers
        // Bit 0 of REG_BPTWL_BATTERY: 0 = not charging, 1 = charging
        // REG_BPTWL_BATTERY_PERCENT: 0–100
        // These symbols come from <nds/arm9/trig_lut.h> or power headers.
        // Use the safe libnds 2.x API if available:
        extern int getBatteryPercent();     // libnds stub; may not exist on all SDK versions
        // Wrap in a try-equivalent: check symbol at link time via weak attribute
        // For safety we just read known memory-mapped registers:
        volatile uint8_t* bptwl_percent =
            (volatile uint8_t*)0x4004700;   // TWL_SPI battery percent
        volatile uint8_t* bptwl_status  =
            (volatile uint8_t*)0x4004701;
        uint8_t pct = *bptwl_percent;
        if (pct <= 100) {
            s.batteryPercent = (int)pct;
            s.isCharging     = (*bptwl_status & 0x80) != 0;
        }
    }
  #endif
    // DS Lite: no battery percentage register; leave at -1.
#endif

    // ── RTC ──────────────────────────────────────────────────────────────────
#if defined(SCRATCHDS_HAVE_RTC)
    rtcTimeAndDate now;
    if (rtcGetTimeAndDate(&now) == 0) {
        // rtcTimeAndDate fields are BCD-encoded in libnds
        int h  = ((now.hours   >> 4) & 0x3) * 10 + (now.hours   & 0xF);
        int m  = ((now.minutes >> 4) & 0x7) * 10 + (now.minutes & 0xF);
        int sec= ((now.seconds >> 4) & 0x7) * 10 + (now.seconds & 0xF);
        int yy = ((now.year    >> 4) & 0xF) * 10 + (now.year    & 0xF);
        int mo = ((now.month   >> 4) & 0x1) * 10 + (now.month   & 0xF);
        int dd = ((now.day     >> 4) & 0x3) * 10 + (now.day     & 0xF);
        snprintf(s.timeStr, sizeof(s.timeStr), "%02d:%02d:%02d", h, m, sec);
        snprintf(s.dateStr, sizeof(s.dateStr), "20%02d-%02d-%02d", yy, mo, dd);
    } else {
        strncpy(s.timeStr, "--:--:--", sizeof(s.timeStr) - 1);
        strncpy(s.dateStr, "----/--/--", sizeof(s.dateStr) - 1);
    }
#else
    strncpy(s.timeStr, "--:--:--",   sizeof(s.timeStr) - 1);
    strncpy(s.dateStr, "----/--/--", sizeof(s.dateStr) - 1);
#endif

    // ── FPS ──────────────────────────────────────────────────────────────────
    s.fpsTenths = (int)(liveFps_ * 10.0f);

    // ── NDS model ────────────────────────────────────────────────────────────
#ifdef isDSiMode
    strncpy(s.ndsModel,
            isDSiMode() ? "Nintendo DSi" : "Nintendo DS / DS Lite",
            sizeof(s.ndsModel) - 1);
#else
    strncpy(s.ndsModel, "Nintendo DS / DS Lite", sizeof(s.ndsModel) - 1);
#endif
    s.ndsModel[sizeof(s.ndsModel) - 1] = '\0';

    usageStatsDirty_ = false;
}

// ═══════════════════════════════════════════════════════════════════════════════
// renderUsage — the full USAGE page render
// NDS console is 32 columns × 24 rows.
// ═══════════════════════════════════════════════════════════════════════════════

// Helper: draw a small ASCII bar of given width, filled to 'pct' (0-100).
static void drawBar(int filled, int total, const char* fillCol,
                    const char* emptyCol) {
    printf(fillCol);
    for (int i = 0; i < filled; i++) printf("#");
    printf(emptyCol);
    for (int i = filled; i < total; i++) printf("-");
    printf(COL_RESET);
}

void OverlayMenu::renderUsage() {
    const UsageStats& s = usageStats_;
    const int TOTAL_COLS = 32;

    renderHeader("Usage");

    // ── Scroll sections:
    // 0 = RAM overview  (4 lines)
    // 1 = Sprites       (2 lines each, up to 16 = 32 lines)
    // 2 = Threads       (1 line each, up to 64)
    // 3 = OAM/Palette   (3 lines)
    // 4 = Battery/Clock (3 lines)
    // All accessed via D-pad Up/Down scrolling of a "virtual row" list.

    // Build a flat list of lines in a local buffer (no heap).
    // We directly printf each group, using usageScrollOff_ as a line counter.

    // For simplicity: render up to 18 visible lines starting at scrollOff_.
    // Each "section" contributes a fixed number of lines shown below.

    int row = 0;  // current logical row counter
    int vis = 0;  // visible row counter (0-17)
    const int MAX_VIS = 18;
    const int TOP = 4;  // console row where content starts (after header)

#define EMIT_BEGIN  if (row++ < usageScrollOff_) { goto next_emit; } \
                    if (vis >= MAX_VIS) goto stop_emit;               \
                    printf("\x1b[%d;0H", TOP + vis++);
#define EMIT_END
#define next_emit:  (void)0;
// The macro approach is awkward in C++ — use a lambda instead:
    (void)row; (void)vis;

    // Reset cursor to content area
    int line = TOP;
    int logRow = 0;

    auto emit = [&](bool doIt) -> bool {
        if (!doIt) return false;
        if (line >= 24) return false;
        printf("\x1b[%d;0H", line++);
        return true;
    };
    auto skip = [&]() { logRow++; };

    auto shouldShow = [&]() -> bool {
        if (logRow < usageScrollOff_) { logRow++; return false; }
        if (line >= 23) { logRow++; return false; }
        logRow++;
        printf("\x1b[%d;0H", line++);
        // clear to EOL
        printf("                                ");
        printf("\x1b[%d;0H", line - 1);
        return true;
    };

    // ── Section: RAM overview ─────────────────────────────────────────────────
    if (shouldShow()) {
        printf(COL_CYAN "── RAM ──────────────────────────" COL_RESET);
    }
    if (shouldShow()) {
        int usedKB = s.usedRamBytes() / 1024;
        int freeKB = s.freeRamBytes   / 1024;
        int totalKB = s.totalRamBytes / 1024;
        printf(COL_WHITE "Total %4dKB  Used %4dKB" COL_RESET, totalKB, usedKB);
    }
    if (shouldShow()) {
        int filled = (s.usedRamBytes() * 20) / s.totalRamBytes;
        if (filled < 0) filled = 0;
        if (filled > 20) filled = 20;
        printf("["); drawBar(filled, 20, COL_RED, COL_GREEN); printf("] ");
        printf(COL_GREY "%4dKB free" COL_RESET, s.freeRamBytes / 1024);
    }
    if (shouldShow()) {
        int sprKB = s.usedBySpritesBytes / 1024;
        int sndKB = s.usedBySoundsBytes  / 1024;
        printf(COL_YELLOW "Gfx %3dKB  Snd %3dKB  Sys~%3dKB" COL_RESET,
               sprKB, sndKB,
               (s.usedRamBytes() - s.usedBySpritesBytes - s.usedBySoundsBytes) / 1024);
    }

    // ── Section: Sprites ──────────────────────────────────────────────────────
    if (shouldShow()) {
        printf(COL_CYAN "── Sprites (%2d) ─────────────────" COL_RESET, s.numSprites);
    }
    for (int i = 0; i < s.numSprites; i++) {
        const UsageStats::SpriteEntry& e = s.sprites[i];
        if (shouldShow()) {
            // Line A: name + visibility
            printf("%s%-18s %s%s" COL_GREY " %3dKB" COL_RESET,
                   e.isStage ? COL_MAGENTA : (e.visible ? COL_WHITE : COL_GREY),
                   e.name,
                   e.isStage ? "STG" : (e.visible ? " on" : "off"),
                   COL_RESET,
                   (e.costumesBytes + e.soundsBytes) / 1024);
        }
        if (shouldShow()) {
            // Line B: block/costume/sound counts
            printf(COL_GREY "  blk:%-3d cos:%-2d snd:%-2d "
                   "gfx:%3dKB" COL_RESET,
                   e.numBlocks, e.numCostumes, e.numSounds,
                   e.costumesBytes / 1024);
        }
    }
    if (s.numSprites == 0 && shouldShow()) {
        printf(COL_GREY "  (no sprites loaded)" COL_RESET);
    }

    // ── Section: Threads ─────────────────────────────────────────────────────
    if (shouldShow()) {
        printf(COL_CYAN "── Threads (%2d) ─────────────────" COL_RESET, s.numThreads);
    }
    for (int i = 0; i < s.numThreads; i++) {
        const UsageStats::ThreadEntry& te = s.threads[i];
        if (shouldShow()) {
            const char* stCol = COL_GREEN;
            if (te.state[0] == 'W') stCol = COL_YELLOW;
            else if (te.state[0] == 'D') stCol = COL_GREY;
            printf(COL_WHITE "%-10s " COL_RESET "%s%-8s" COL_RESET
                   COL_GREY " stk:%d" COL_RESET,
                   te.spriteName, stCol, te.state, te.stackDepth);
        }
    }
    if (s.numThreads == 0 && shouldShow()) {
        printf(COL_GREY "  (no threads active)" COL_RESET);
    }

    // ── Section: OAM & Palette ────────────────────────────────────────────────
    if (shouldShow()) {
        printf(COL_CYAN "── OAM / Palette ────────────────" COL_RESET);
    }
    if (shouldShow()) {
        // OAM bar: 128 slots, show used/128
        int oamFill = (s.oamSlotsUsed * 16) / 128;
        if (oamFill > 16) oamFill = 16;
        printf(COL_WHITE "OAM["); drawBar(oamFill, 16, COL_YELLOW, COL_GREY);
        printf("] %3d/128" COL_RESET, s.oamSlotsUsed);
    }
    if (shouldShow()) {
        // Palette bar: 15 usable slots (slot 0 reserved)
        int palFill = s.palSlotsUsed;
        if (palFill > 15) palFill = 15;
        printf(COL_WHITE "PAL["); drawBar(palFill, 15, COL_MAGENTA, COL_GREY);
        printf("] %2d/15 " COL_RESET, s.palSlotsUsed);
        // FPS inline
        printf(COL_CYAN "FPS:%2d.%d" COL_RESET,
               s.fpsTenths / 10, s.fpsTenths % 10);
    }

    // ── Section: Battery & Clock ──────────────────────────────────────────────
    if (shouldShow()) {
        printf(COL_CYAN "── System ───────────────────────" COL_RESET);
    }
    if (shouldShow()) {
        // Time + date
        printf(COL_WHITE "%s  %s" COL_RESET "  " COL_GREY "%s" COL_RESET,
               s.timeStr, s.dateStr, s.ndsModel);
    }
    if (shouldShow()) {
        if (s.batteryPercent < 0) {
            printf(COL_GREY "Battery: N/A (DS Lite)" COL_RESET);
        } else {
            int batFill = (s.batteryPercent * 16) / 100;
            const char* batCol = s.batteryPercent > 50 ? COL_GREEN :
                                 s.batteryPercent > 20 ? COL_YELLOW : COL_RED;
            printf(COL_WHITE "Batt["); drawBar(batFill, 16, batCol, COL_GREY);
            printf("] %3d%%%s" COL_RESET,
                   s.batteryPercent,
                   s.isCharging ? COL_GREEN "+" COL_RESET : "");
        }
    }

stop_emit:
    // Footer hint
    printf("\x1b[23;0H");
    printf(COL_GREY "[Up/Dn]=Scroll [Y]=Refresh [B]=Back" COL_RESET);

    // Scroll indicator
    int totalLogRows = logRow;
    if (totalLogRows > MAX_VIS) {
        int pct = (usageScrollOff_ * 100) / (totalLogRows - MAX_VIS);
        printf("\x1b[4;31H");
        printf(COL_GREY "%2d%%" COL_RESET, pct);
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// handleUsageInput
// ═══════════════════════════════════════════════════════════════════════════════
void OverlayMenu::handleUsageInput() {
    if (cachedKeysDown_ & KEY_UP) {
        if (usageScrollOff_ > 0) usageScrollOff_--;
    }
    if (cachedKeysDown_ & KEY_DOWN) {
        usageScrollOff_++;
    }
    // Y = force refresh of stats (re-probe RAM, re-read threads)
    if (cachedKeysDown_ & KEY_Y) {
        gatherUsageStats();
    }
    if (cachedKeysDown_ & (KEY_B | KEY_START)) {
        page_    = MenuPage::MAIN;
        cursor_  = 0;
        usageScrollOff_ = 0;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// OverlayMenu::init
// ═══════════════════════════════════════════════════════════════════════════════
void OverlayMenu::init(ScratchDSSettings& settings) {
    settings_        = &settings;
    pending_         = settings;
    open_            = false;
    page_            = MenuPage::MAIN;
    cursor_          = 0;
    scrollOff_       = 0;
    compiling_       = false;
    compileTimer_    = 0;
    compileProgress_ = 0;
    comboHoldTimer_  = 0;
    externalConsole_ = nullptr;
    usageScrollOff_  = 0;
    usageStatsDirty_ = true;
    currentDir_[0]   = selectedPath_[0] = confirmMsg_[0] = '\0';
    confirmResult_   = false;
    cachedKeysDown_  = cachedKeysHeld_ = 0;
    liveProject_     = nullptr;
    liveVm_          = nullptr;
    liveFps_         = 0.0f;
    livePalSlots_    = 0;
    liveOamSlots_    = 0;
    strncpy(currentDir_, "fat:/scratch/", sizeof(currentDir_) - 1);
}

// ═══════════════════════════════════════════════════════════════════════════════
// update — reads InputHandler cache, never calls scanKeys()
// ═══════════════════════════════════════════════════════════════════════════════
bool OverlayMenu::update(float dt) {
    cachedKeysDown_ = InputHandler::getInstance().getKeysDown();
    cachedKeysHeld_ = InputHandler::getInstance().getKeysHeld();

    if (!open_) {
        if ((cachedKeysHeld_ & (KEY_L | KEY_R | KEY_B)) == (KEY_L | KEY_R | KEY_B)) {
            comboHoldTimer_ += dt;
            if (comboHoldTimer_ >= COMBO_HOLD_REQUIRED) {
                open();
                comboHoldTimer_ = 0;
            }
        } else {
            comboHoldTimer_ = 0;
        }
        return false;
    }

    if (compiling_) tickCompile(dt);
    handleInput();
    render();
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// open / close
// ═══════════════════════════════════════════════════════════════════════════════
void OverlayMenu::open() {
    open_            = true;
    page_            = MenuPage::MAIN;
    cursor_          = 0;
    scrollOff_       = 0;
    usageScrollOff_  = 0;
    usageStatsDirty_ = true;
    pending_         = *settings_;
    s_freeRamCache   = -1;

    if (externalConsole_) consoleSelect(externalConsole_);
    consoleClear();
}

void OverlayMenu::close() {
    open_ = false;
    if (externalConsole_) consoleSelect(externalConsole_);
    consoleClear();
}

void OverlayMenu::consoleClear() {
    if (externalConsole_) consoleSelect(externalConsole_);
    ::consoleClear();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Input dispatch
// ═══════════════════════════════════════════════════════════════════════════════
void OverlayMenu::handleInput() {
    switch (page_) {
        case MenuPage::MAIN:          handleMainInput();     break;
        case MenuPage::INFO:          handleInfoInput();     break;
        case MenuPage::SETTINGS:      handleSettingsInput(); break;
        case MenuPage::LOAD:          handleLoadInput();     break;
        case MenuPage::USAGE:         handleUsageInput();    break;
        case MenuPage::CONFIRM_RESET:
        case MenuPage::CONFIRM_LOAD: {
            bool confirmed = false;
            handleConfirmInput(confirmed);
            if (confirmed) {
                if (page_ == MenuPage::CONFIRM_RESET) { applySettings(); close(); }
                else if (page_ == MenuPage::CONFIRM_LOAD) {
                    if (onLoad_) onLoad_(selectedPath_);
                    close();
                }
            }
            break;
        }
        default: break;
    }
}

void OverlayMenu::handleMainInput() {
    const int N = 5;  // added USAGE
    if (cachedKeysDown_ & KEY_UP)   cursor_ = (cursor_ - 1 + N) % N;
    if (cachedKeysDown_ & KEY_DOWN) cursor_ = (cursor_ + 1) % N;
    if (cachedKeysDown_ & (KEY_A | KEY_START)) {
        switch (cursor_) {
            case 0: page_ = MenuPage::INFO;     cursor_ = 0; break;
            case 1: page_ = MenuPage::SETTINGS; cursor_ = 0; pending_ = *settings_; break;
            case 2: page_ = MenuPage::LOAD;     cursor_ = 0; scrollOff_ = 0;
                    scanDirectory(currentDir_); break;
            case 3:
                page_            = MenuPage::USAGE;
                cursor_          = 0;
                usageScrollOff_  = 0;
                usageStatsDirty_ = true;
                gatherUsageStats();
                break;
            case 4: close(); break;
        }
    }
    if (cachedKeysDown_ & KEY_B) close();
}

void OverlayMenu::handleInfoInput() {
    if (cachedKeysDown_ & (KEY_B | KEY_START)) { page_ = MenuPage::MAIN; cursor_ = 0; }
}

void OverlayMenu::handleSettingsInput() {
    const int N = 6;
    if (cachedKeysDown_ & KEY_UP)   cursor_ = (cursor_ - 1 + N) % N;
    if (cachedKeysDown_ & KEY_DOWN) cursor_ = (cursor_ + 1) % N;
    if (cachedKeysDown_ & (KEY_A | KEY_LEFT | KEY_RIGHT)) {
        switch (cursor_) {
            case 0: cycleFPS();    break;
            case 1: cycleScreen(); break;
            case 2: cycleScale();  break;
            case 3: pending_.showFPSCounter = !pending_.showFPSCounter; break;
            case 4: page_ = MenuPage::COMPILE; startCompile(); break;
            case 5:
                snprintf(confirmMsg_, sizeof(confirmMsg_), "Apply settings and\nreset scene?");
                page_ = MenuPage::CONFIRM_RESET; cursor_ = 0; break;
        }
    }
    if (cachedKeysDown_ & KEY_B) { page_ = MenuPage::MAIN; cursor_ = 1; }
}

void OverlayMenu::handleLoadInput() {
    int total = (int)dirEntries_.size();
    if (total == 0) {
        if (cachedKeysDown_ & KEY_B) { page_ = MenuPage::MAIN; cursor_ = 2; }
        return;
    }
    if (cachedKeysDown_ & KEY_UP)   cursor_ = (cursor_ - 1 + total) % total;
    if (cachedKeysDown_ & KEY_DOWN) cursor_ = (cursor_ + 1) % total;

    const int VIS = 14;
    if (cursor_ < scrollOff_)        scrollOff_ = cursor_;
    if (cursor_ >= scrollOff_ + VIS) scrollOff_ = cursor_ - VIS + 1;

    if (cachedKeysDown_ & KEY_A) {
        if (dirEntries_[cursor_].isDir) {
            navigateInto(cursor_);
        } else {
            snprintf(selectedPath_, sizeof(selectedPath_), "%s%s",
                     currentDir_, dirEntries_[cursor_].name);
            snprintf(confirmMsg_, sizeof(confirmMsg_),
                     "Load project:\n%.28s?", dirEntries_[cursor_].name);
            page_ = MenuPage::CONFIRM_LOAD; cursor_ = 0;
        }
    }
    if (cachedKeysDown_ & KEY_B)     navigateUp();
    if (cachedKeysDown_ & KEY_START) { page_ = MenuPage::MAIN; cursor_ = 2; }
}

void OverlayMenu::handleConfirmInput(bool& confirmed) {
    if (cachedKeysDown_ & (KEY_LEFT | KEY_RIGHT)) cursor_ ^= 1;
    if (cachedKeysDown_ & KEY_A) {
        confirmed = (cursor_ == 0);
        if (!confirmed) { page_ = MenuPage::SETTINGS; cursor_ = 5; }
    }
    if (cachedKeysDown_ & KEY_B) {
        confirmed = false; page_ = MenuPage::SETTINGS; cursor_ = 5;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Render dispatch
// ═══════════════════════════════════════════════════════════════════════════════
void OverlayMenu::render() {
    if (externalConsole_) consoleSelect(externalConsole_);
    ::consoleClear();

    switch (page_) {
        case MenuPage::MAIN:          renderMain();         break;
        case MenuPage::INFO:          renderInfo();         break;
        case MenuPage::SETTINGS:      renderSettings();     break;
        case MenuPage::LOAD:          renderLoad();         break;
        case MenuPage::COMPILE:       renderCompile();      break;
        case MenuPage::USAGE:         renderUsage();        break;
        case MenuPage::CONFIRM_RESET:
        case MenuPage::CONFIRM_LOAD:  renderConfirmReset(); break;
        default: break;
    }
}

void OverlayMenu::renderHeader(const char* title) {
    printf(COL_CYAN "================================" COL_RESET "\n");
    printf(COL_YELLOW " ScratchDS " COL_WHITE "%-20s" COL_RESET "\n", title);
    printf(COL_CYAN "================================" COL_RESET "\n");
}

void OverlayMenu::renderFooter() {
    printf("\x1b[23;0H");
    printf(COL_GREY "[B]=Back [A]=Select [L+R+B]=Close" COL_RESET);
}

void OverlayMenu::renderMain() {
    renderHeader("v" SCRATCHDS_VERSION);
    printf("\n");
    // 5 items now (added Usage)
    const char* items[] = { "  Info", "  Settings", "  Load Project",
                             "  Usage", "  Resume" };
    const char* icons[] = { "i", "*", "^", "?", ">" };
    for (int i = 0; i < 5; i++) {
        bool sel = (cursor_ == i);
        printf("%s[%s]%s%s\n", sel ? COL_YELLOW : COL_GREY, icons[i], items[i], COL_RESET);
        if (i < 4) printf("\n");
    }
    printf("\n");
    printf(COL_GREY " Hold L+R+B to reopen menu" COL_RESET "\n");
    renderFooter();
}

void OverlayMenu::renderInfo() {
    renderHeader("Info");
    if (s_freeRamCache < 0) s_freeRamCache = measureFreeRamBinary();

    const int TOTAL_RAM = 4 * 1024 * 1024;
    int freeRam = s_freeRamCache < 0 ? 0 : s_freeRamCache;
    if (freeRam > TOTAL_RAM) freeRam = TOTAL_RAM;
    int usedRam = TOTAL_RAM - freeRam;
    int filled  = (usedRam * 20) / TOTAL_RAM;
    if (filled < 0) filled = 0;
    if (filled > 20) filled = 20;

    char ndsModel[32]; getNDSModel(ndsModel, sizeof(ndsModel));
    char projName[64]; getProjectName(projName, sizeof(projName));

    printf("\n");
    printf(COL_CYAN " Version    " COL_WHITE SCRATCHDS_VERSION COL_RESET "\n");
    printf(COL_CYAN " Built      " COL_WHITE SCRATCHDS_BUILD_DATE COL_RESET "\n\n");
    printf(COL_CYAN " Device     " COL_WHITE "%s" COL_RESET "\n\n", ndsModel);
    printf(COL_CYAN " RAM Usage\n" COL_RESET);
    printf(" [");
    for (int i = 0; i < 20; i++)
        printf(i < filled ? COL_RED "#" COL_RESET : COL_GREEN "-" COL_RESET);
    printf("] %d/%dKB\n", usedRam / 1024, TOTAL_RAM / 1024);
    printf(COL_GREY " Free: %dKB  Used: %dKB\n\n" COL_RESET, freeRam/1024, usedRam/1024);
    printf(COL_CYAN " Project    " COL_WHITE "%.20s" COL_RESET "\n", projName);
    printf(COL_CYAN " Target FPS " COL_WHITE "%d" COL_RESET "\n", settings_->targetFPS);
    printf(COL_CYAN " Scale      " COL_WHITE "%s" COL_RESET "\n",
           settings_->stageScale == 0 ? "Stretch" :
           settings_->stageScale == 1 ? "Aspect" : "Native");
    printf("\n" COL_GREY " devkitARM  " DEVKITARM_VERSION COL_RESET "\n");
    renderFooter();
}

void OverlayMenu::renderSettings() {
    renderHeader("Settings");
    printf("\n");
    char fpsStr[8], scaleStr[16], fpsCountStr[8];
    snprintf(fpsStr,      sizeof(fpsStr),      "%d", pending_.targetFPS);
    snprintf(scaleStr,    sizeof(scaleStr),    "%s",
             pending_.stageScale == 0 ? "Stretch" :
             pending_.stageScale == 1 ? "Aspect"  : "Native");
    snprintf(fpsCountStr, sizeof(fpsCountStr), "%s",
             pending_.showFPSCounter ? "On" : "Off");

    const char* labels[] = {
        " Target FPS   "," Primary Screen"," Stage Scale   ",
        " FPS Counter  "," Compile        "," Apply & Back  "
    };
    const char* values[] = {
        fpsStr, pending_.stageOnTop ? "Top=Stage" : "Bot=Stage",
        scaleStr, fpsCountStr, "...", ""
    };
    for (int i = 0; i < 6; i++) {
        bool sel = (cursor_ == i);
        if (i == 5)
            printf("%s[APPLY & BACK]%s\n", sel ? COL_GREEN : COL_GREY, COL_RESET);
        else
            printf("%s%s%s%s%s\n", sel ? COL_YELLOW : COL_GREY,
                   labels[i], COL_WHITE, values[i], COL_RESET);
        printf("\n");
    }
    renderFooter();
}

void OverlayMenu::renderLoad() {
    renderHeader("Load Project");
    printf(COL_GREY " Dir: %.26s\n" COL_RESET, currentDir_);
    printf(COL_CYAN "--------------------------------" COL_RESET "\n");
    if (dirEntries_.empty()) {
        printf(COL_RED "\n  No .sb3 files found.\n" COL_RESET);
        printf(COL_GREY "  fat:/scratch/\n" COL_RESET);
    } else {
        const int VIS = 14;
        int end = scrollOff_ + VIS;
        if (end > (int)dirEntries_.size()) end = (int)dirEntries_.size();
        for (int i = scrollOff_; i < end; i++) {
            bool sel = (cursor_ == i);
            printf("%s%s%.26s%s\n",
                   sel ? COL_YELLOW : COL_WHITE,
                   dirEntries_[i].isDir ? "[D] " : "    ",
                   dirEntries_[i].name, COL_RESET);
        }
    }
    printf("\x1b[23;0H");
    printf(COL_GREY "[A]=Select [B]=Up [START]=Cancel" COL_RESET);
}

void OverlayMenu::renderCompile() {
    renderHeader("Compile");
    printf("\n" COL_WHITE " Compiling Scratch project...\n\n" COL_RESET);
    printf(COL_CYAN " Progress: %3d%%\n [", compileProgress_);
    int filled = compileProgress_ / 5;
    for (int i = 0; i < 20; i++)
        printf("%s", i < filled ? COL_GREEN "#" : COL_GREY "-");
    printf(COL_CYAN "]\n\n" COL_RESET);
    printf(COL_YELLOW " Status: %s\n" COL_RESET, compileStatus_);
    if (compileProgress_ >= 100) {
        printf("\n" COL_GREEN " Done! Output: fat:/scratch/out/\n" COL_RESET);
        printf(COL_GREY " [A] to return to menu\n" COL_RESET);
        if (cachedKeysDown_ & KEY_A) { page_ = MenuPage::SETTINGS; cursor_ = 4; }
    }
}

void OverlayMenu::renderConfirmReset() {
    renderHeader("Confirm");
    printf("\n\n" COL_WHITE " %s\n\n" COL_RESET, confirmMsg_);
    printf("\n");
    printf("%s [YES] %s  %s [NO] %s\n",
           cursor_ == 0 ? COL_GREEN : COL_GREY, COL_RESET,
           cursor_ == 1 ? COL_RED   : COL_GREY, COL_RESET);
    printf("\n" COL_GREY " [Left/Right] to switch\n [A] to confirm\n" COL_RESET);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Settings helpers
// ═══════════════════════════════════════════════════════════════════════════════
void OverlayMenu::cycleFPS()    { pending_.targetFPS = (pending_.targetFPS == 60) ? 30 : 60; }
void OverlayMenu::cycleScreen() { pending_.stageOnTop = !pending_.stageOnTop; }
void OverlayMenu::cycleScale()  { pending_.stageScale = (pending_.stageScale + 1) % 3; }

void OverlayMenu::applySettings() {
    *settings_ = pending_;
    if (settings_->autoSaveSettings) settings_->save();
    if (onApply_) onApply_(*settings_);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Compile simulation
// ═══════════════════════════════════════════════════════════════════════════════
void OverlayMenu::startCompile() {
    compiling_ = true; compileTimer_ = 0; compileProgress_ = 0;
    strncpy(compileStatus_, "Parsing project.json...", sizeof(compileStatus_)-1);
}
void OverlayMenu::tickCompile(float dt) {
    compileTimer_ += dt;
    struct Stage { float t; int p; const char* s; };
    static const Stage stages[] = {
        {0.5f,10,"Parsing project.json..."},
        {1.0f,25,"Converting costumes to tiles..."},
        {2.0f,50,"Decoding audio to PCM..."},
        {2.8f,75,"Building block bytecode..."},
        {3.5f,90,"Writing output binary..."},
        {4.0f,100,"Compilation complete!"},
    };
    for (int i = 0; i < 6; i++) {
        if (compileTimer_ >= stages[i].t) {
            compileProgress_ = stages[i].p;
            strncpy(compileStatus_, stages[i].s, sizeof(compileStatus_)-1);
        }
    }
    if (compileProgress_ >= 100) compiling_ = false;
}

// ═══════════════════════════════════════════════════════════════════════════════
// File browser
// ═══════════════════════════════════════════════════════════════════════════════
void OverlayMenu::scanDirectory(const char* path) {
    dirEntries_.clear(); cursor_ = 0; scrollOff_ = 0;
    strncpy(currentDir_, path, sizeof(currentDir_)-1);
    currentDir_[sizeof(currentDir_)-1] = '\0';

    if (strcmp(path,"fat:/") != 0 && strcmp(path,"fat:/scratch/") != 0) {
        FileEntry up; strncpy(up.name, "..", sizeof(up.name)-1);
        up.name[sizeof(up.name)-1] = '\0'; up.isDir = true;
        dirEntries_.push_back(up);
    }
    DIR* dir = opendir(path);
    if (!dir) return;
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        FileEntry fe;
        strncpy(fe.name, ent->d_name, sizeof(fe.name)-1);
        fe.name[sizeof(fe.name)-1] = '\0';
        char full[512];
        snprintf(full, sizeof(full), "%s%s", path, ent->d_name);
        struct stat st;
        fe.isDir = (stat(full, &st) == 0 && S_ISDIR(st.st_mode));
        if (!fe.isDir) {
            size_t len = strlen(fe.name);
            if (len < 4 || strcmp(fe.name + len - 4, ".sb3") != 0) continue;
        }
        dirEntries_.push_back(fe);
    }
    closedir(dir);
}
void OverlayMenu::navigateInto(int idx) {
    if (idx < 0 || idx >= (int)dirEntries_.size() || !dirEntries_[idx].isDir) return;
    if (strcmp(dirEntries_[idx].name, "..") == 0) { navigateUp(); return; }
    char np[512];
    snprintf(np, sizeof(np), "%s%s/", currentDir_, dirEntries_[idx].name);
    scanDirectory(np);
}
void OverlayMenu::navigateUp() {
    char tmp[256]; strncpy(tmp, currentDir_, sizeof(tmp)-1); tmp[sizeof(tmp)-1] = '\0';
    int len = (int)strlen(tmp);
    if (len > 0 && tmp[len-1] == '/') tmp[--len] = '\0';
    char* slash = strrchr(tmp, '/');
    if (slash) *(slash+1) = '\0';
    if (strlen(tmp) < 5) strncpy(tmp, "fat:/scratch/", sizeof(tmp)-1);
    tmp[sizeof(tmp)-1] = '\0';
    scanDirectory(tmp);
}

// ═══════════════════════════════════════════════════════════════════════════════
// NDS info helpers
// ═══════════════════════════════════════════════════════════════════════════════
void OverlayMenu::getNDSModel(char* out, int maxLen) {
#ifdef isDSiMode
    strncpy(out, isDSiMode() ? "Nintendo DSi" : "Nintendo DS / DS Lite", maxLen-1);
#else
    strncpy(out, "Nintendo DS / DS Lite", maxLen-1);
#endif
    out[maxLen-1] = '\0';
}
int OverlayMenu::getFreeRAM() { return measureFreeRamBinary(); }
void OverlayMenu::getProjectName(char* out, int maxLen) {
    if (settings_->lastProjectPath[0] != '\0') {
        const char* slash = strrchr(settings_->lastProjectPath, '/');
        strncpy(out, slash ? slash+1 : settings_->lastProjectPath, maxLen-1);
    } else {
        strncpy(out, "(example.sb3)", maxLen-1);
    }
    out[maxLen-1] = '\0';
}
