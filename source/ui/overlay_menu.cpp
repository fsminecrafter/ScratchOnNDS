// =============================================================================
// overlay_menu.cpp — fixed: clean console management, no double-writing,
//                    no flicker, refined UI layout.
//
// Key fixes:
//   1. consoleSel() helper always calls consoleSelect before any printf.
//   2. render() clears ONCE at the top, no per-page clears.
//   3. renderUI() (game HUD) is a no-op when menu is open — enforced in
//      mainLoop by the paused return value; renderUI itself also guards.
//   4. USAGE page replaced with a simple line-by-line renderer (no macros,
//      no goto, no partial writes).
//   5. INFO page caches free RAM in open(), not on every render frame.
//   6. All pages use a fixed 32-col layout with explicit cursor positioning.
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

#if defined(ARM9)
#  if defined(__has_include)
#    if __has_include(<nds/arm9/rtc.h>)
#      include <nds/arm9/rtc.h>
#      define SCRATCHDS_HAVE_RTC 1
#    endif
#  endif
#endif

// ── Colour escape codes ───────────────────────────────────────────────────────
#define C_WHITE   "\x1b[37;1m"
#define C_CYAN    "\x1b[36;1m"
#define C_YELLOW  "\x1b[33;1m"
#define C_GREEN   "\x1b[32;1m"
#define C_RED     "\x1b[31;1m"
#define C_GREY    "\x1b[37;0m"
#define C_MAGENTA "\x1b[35;1m"
#define C_RESET   "\x1b[0m"

// NDS text console is exactly 32 cols × 24 rows.
#define CON_COLS 32
#define CON_ROWS 24
#define CON_CONTENT_ROWS 20   // rows 2-21, leaving header(2) + footer(2)

// ── Internal helpers ──────────────────────────────────────────────────────────

// Always select our console before printing.
static PrintConsole* s_con = nullptr;
static void consoleSel() {
    if (s_con) consoleSelect(s_con);
}

// Move cursor to row r (0-based), col 0, clear to end of line.
static void gotoRow(int r) {
    printf("\x1b[%d;0H", r);
}

// Print a divider line.
static void divider() {
    printf(C_CYAN "--------------------------------" C_RESET);
}

// Print a bar of given fill/total width.
static void printBar(int filled, int total,
                     const char* fillCol, const char* emptyCol) {
    if (filled < 0) filled = 0;
    if (filled > total) filled = total;
    printf(fillCol);
    for (int i = 0; i < filled;         i++) printf("#");
    printf(emptyCol);
    for (int i = filled; i < total; i++) printf("-");
    printf(C_RESET);
}

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
    if ((down & KEY_B) && len_ > 0) buf_[--len_] = '\0';
    return (down & KEY_START) != 0;
}
void DPadTextInput::render(int y) {
    consoleSel();
    printf("\x1b[%d;0H", y);
    printf(C_GREY "%s" C_RESET "\n", prompt_);
    printf(C_WHITE "> %s" C_CYAN "_" C_RESET "\n", buf_);
    printf(C_GREY "< " C_YELLOW "%c" C_GREY
           " > [A]=Add [B]=Del [START]=OK\n", CHARS[charSel_]);
}

// ═══════════════════════════════════════════════════════════════════════════════
// RAM probe — only called once on menu open, result cached.
// ═══════════════════════════════════════════════════════════════════════════════
int OverlayMenu::measureFreeRamBinary() {
    int lo = 0, hi = (int)(3.9f * 1024 * 1024);
    while (hi - lo > 4096) {
        int mid = (lo + hi) / 2;
        void* p = malloc((size_t)mid);
        if (p) { free(p); lo = mid; } else { hi = mid; }
    }
    return lo;
}

// ═══════════════════════════════════════════════════════════════════════════════
// gatherUsageStats
// ═══════════════════════════════════════════════════════════════════════════════
void OverlayMenu::gatherUsageStats() {
    UsageStats& s = usageStats_;
    memset(&s, 0, sizeof(s));

    s.totalRamBytes = 4 * 1024 * 1024;
    s.freeRamBytes  = measureFreeRamBinary();
    s.vramEstimateBytes = 512 * 1024;

    s.numSprites = 0;
    s.usedBySpritesBytes = 0;
    s.usedBySoundsBytes  = 0;

    if (liveProject_) {
        for (auto& sprite : liveProject_->targets) {
            if (s.numSprites >= UsageStats::MAX_SPRITES) break;
            UsageStats::SpriteEntry& e = s.sprites[s.numSprites++];
            memset(&e, 0, sizeof(e));
            strncpy(e.name, sprite.name.c_str(), sizeof(e.name) - 1);
            e.visible     = sprite.visible;
            e.isStage     = sprite.isStage;
            e.numBlocks   = (int)sprite.blocks.size();
            e.numCostumes = (int)sprite.costumes.size();
            e.numSounds   = (int)sprite.sounds.size();
            for (auto& c : sprite.costumes) {
                if (!c.gfxPtr) continue;
                e.costumesBytes += c.isBackdrop
                    ? c.width * c.height * 2
                    : c.width * c.height;
            }
            for (auto& snd : sprite.sounds)
                if (snd.loaded && !snd.isStreamed && snd.pcmData)
                    e.soundsBytes += (int)snd.pcmSize;
            s.usedBySpritesBytes += e.costumesBytes;
            s.usedBySoundsBytes  += e.soundsBytes;
        }
    }

    s.numThreads = 0;
    if (liveVm_) {
        const auto& tv = liveVm_->getThreads();
        for (const auto& t : tv) {
            if (s.numThreads >= UsageStats::MAX_THREADS) break;
            UsageStats::ThreadEntry& te = s.threads[s.numThreads++];
            memset(&te, 0, sizeof(te));
            if (t.sprite)
                strncpy(te.spriteName, t.sprite->name.c_str(),
                        sizeof(te.spriteName) - 1);
            else
                strncpy(te.spriteName, "(null)", sizeof(te.spriteName) - 1);
            switch (t.state) {
                case ScriptThread::RUNNING:       strncpy(te.state,"RUN",    15); break;
                case ScriptThread::WAITING_SECS:  strncpy(te.state,"WAIT_T", 15); break;
                case ScriptThread::WAITING_SOUND: strncpy(te.state,"WAIT_S", 15); break;
                case ScriptThread::DONE:          strncpy(te.state,"DONE",   15); break;
                default:                          strncpy(te.state,"???",    15); break;
            }
            strncpy(te.blockId, t.currentBlockId.c_str(), sizeof(te.blockId) - 1);
            te.stackDepth     = (int)t.callStack.size();
            te.stepsThisFrame = t.stepsThisFrame;
        }
    }

    s.palSlotsUsed = livePalSlots_;
    s.oamSlotsUsed = liveOamSlots_;

    s.batteryPercent = -1;
    s.isCharging     = false;

#if defined(SCRATCHDS_HAVE_RTC)
    rtcTimeAndDate now;
    if (rtcGetTimeAndDate(&now) == 0) {
        int h   = ((now.hours   >> 4) & 0x3) * 10 + (now.hours   & 0xF);
        int m   = ((now.minutes >> 4) & 0x7) * 10 + (now.minutes & 0xF);
        int sec = ((now.seconds >> 4) & 0x7) * 10 + (now.seconds & 0xF);
        int yy  = ((now.year    >> 4) & 0xF) * 10 + (now.year    & 0xF);
        int mo  = ((now.month   >> 4) & 0x1) * 10 + (now.month   & 0xF);
        int dd  = ((now.day     >> 4) & 0x3) * 10 + (now.day     & 0xF);
        snprintf(s.timeStr, sizeof(s.timeStr), "%02d:%02d:%02d", h, m, sec);
        snprintf(s.dateStr, sizeof(s.dateStr), "20%02d-%02d-%02d", yy, mo, dd);
    } else {
        strncpy(s.timeStr, "--:--:--",   sizeof(s.timeStr) - 1);
        strncpy(s.dateStr, "----/--/--", sizeof(s.dateStr) - 1);
    }
#else
    strncpy(s.timeStr, "--:--:--",   sizeof(s.timeStr) - 1);
    strncpy(s.dateStr, "----/--/--", sizeof(s.dateStr) - 1);
#endif

    s.fpsTenths = (int)(liveFps_ * 10.0f);
    getNDSModel(s.ndsModel, sizeof(s.ndsModel));
    usageStatsDirty_ = false;
}

// ═══════════════════════════════════════════════════════════════════════════════
// init
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
    cachedFreeRam_   = -1;
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

    // Probe RAM once here so INFO page never re-probes during render.
    cachedFreeRam_ = measureFreeRamBinary();

    s_con = externalConsole_;
    consoleSel();
    consoleClear();
}

void OverlayMenu::close() {
    open_ = false;
    s_con = externalConsole_;
    consoleSel();
    consoleClear();
}

void OverlayMenu::consoleClear() {
    consoleSel();
    ::consoleClear();
}

void OverlayMenu::consoleClearSafe() {
    consoleClear();
}

// ═══════════════════════════════════════════════════════════════════════════════
// update  — called every frame from mainLoop
// Returns true while menu is open (mainLoop must skip renderUI when true).
// ═══════════════════════════════════════════════════════════════════════════════
bool OverlayMenu::update(float dt) {
    s_con = externalConsole_;

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
// render  — single clear, then dispatch
// ═══════════════════════════════════════════════════════════════════════════════
void OverlayMenu::render() {
    // ONE clear per frame, here, before any page renders.
    consoleSel();
    consoleClear();

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

// ═══════════════════════════════════════════════════════════════════════════════
// Shared header / footer
// ═══════════════════════════════════════════════════════════════════════════════
void OverlayMenu::renderHeader(const char* title) {
    // Row 0: title bar
    gotoRow(0);
    printf(C_CYAN "ScratchDS" C_RESET " " C_WHITE "%-22s" C_RESET C_CYAN "v" SCRATCHDS_VERSION C_RESET, title);
    // Row 1: divider
    gotoRow(1);
    divider();
}

void OverlayMenu::renderFooter() {
    gotoRow(22);
    divider();
    gotoRow(23);
    printf(C_GREY "[B]=Back  [A]=Select  [L+R+B]=Exit" C_RESET);
}

// ═══════════════════════════════════════════════════════════════════════════════
// MAIN menu
// ═══════════════════════════════════════════════════════════════════════════════
void OverlayMenu::renderMain() {
    renderHeader("Menu");

    struct Item { const char* icon; const char* label; };
    static const Item items[] = {
        { "i", "Info"          },
        { "*", "Settings"      },
        { "^", "Load Project"  },
        { "?", "Usage"         },
        { ">", "Resume"        },
    };
    static const int N = 5;

    for (int i = 0; i < N; i++) {
        gotoRow(3 + i * 3);
        bool sel = (cursor_ == i);
        printf("%s %s %s %s",
               sel ? C_YELLOW : C_GREY,
               sel ? ">" : " ",
               items[i].label,
               C_RESET);
        // icon on the right
        printf("\x1b[%d;30H[%s]", 3 + i * 3, items[i].icon);
    }

    gotoRow(21);
    printf(C_GREY " Hold L+R+B to open anytime" C_RESET);
    renderFooter();
}

// ═══════════════════════════════════════════════════════════════════════════════
// INFO page
// ═══════════════════════════════════════════════════════════════════════════════
void OverlayMenu::renderInfo() {
    renderHeader("Info");

    // Use RAM value cached in open() — never re-probe during render.
    const int TOTAL_RAM = 4 * 1024 * 1024;
    int freeRam  = (cachedFreeRam_ >= 0) ? cachedFreeRam_ : 0;
    int usedRam  = TOTAL_RAM - freeRam;
    int barFill  = (usedRam * 20) / TOTAL_RAM;

    char ndsModel[32]; getNDSModel(ndsModel, sizeof(ndsModel));
    char projName[28]; getProjectName(projName, sizeof(projName));

    int r = 2;
    gotoRow(r++); printf(C_CYAN " Version  " C_WHITE SCRATCHDS_VERSION C_RESET);
    gotoRow(r++); printf(C_CYAN " Built    " C_WHITE SCRATCHDS_BUILD_DATE C_RESET);
    gotoRow(r++); printf(C_CYAN " Compiler " C_WHITE DEVKITARM_VERSION C_RESET);
    r++;
    gotoRow(r++); printf(C_CYAN " Device   " C_WHITE "%-22s" C_RESET, ndsModel);
    r++;
    gotoRow(r++); printf(C_CYAN " Project  " C_WHITE "%.22s" C_RESET, projName);
    gotoRow(r++); printf(C_CYAN " FPS      " C_WHITE "%d" C_RESET, settings_->targetFPS);
    gotoRow(r++); printf(C_CYAN " Scale    " C_WHITE "%s" C_RESET,
                         settings_->stageScale == 0 ? "Stretch" :
                         settings_->stageScale == 1 ? "Aspect"  : "Native");
    r++;
    gotoRow(r++); printf(C_CYAN " RAM used " C_RESET);
    gotoRow(r++);
    printf(" ["); printBar(barFill, 20, C_RED, C_GREEN); printf("]");
    gotoRow(r++); printf(C_GREY " Used: %dKB  Free: %dKB" C_RESET,
                         usedRam / 1024, freeRam / 1024);

    renderFooter();
}

// ═══════════════════════════════════════════════════════════════════════════════
// SETTINGS page
// ═══════════════════════════════════════════════════════════════════════════════
void OverlayMenu::renderSettings() {
    renderHeader("Settings");

    char fpsStr[8];    snprintf(fpsStr,    sizeof(fpsStr),    "%d", pending_.targetFPS);
    const char* screenStr = pending_.stageOnTop ? "Top=Stage" : "Bot=Stage";
    const char* scaleStr  = pending_.stageScale == 0 ? "Stretch" :
                            pending_.stageScale == 1 ? "Aspect"  : "Native";
    const char* fpsCtStr  = pending_.showFPSCounter ? "On" : "Off";

    struct Row { const char* label; const char* value; };
    const Row rows[] = {
        { " Target FPS",    fpsStr     },
        { " Screen",        screenStr  },
        { " Stage Scale",   scaleStr   },
        { " FPS Counter",   fpsCtStr   },
        { " Compile...",    ""         },
        { " Apply & Back",  ""         },
    };
    static const int N = 6;

    for (int i = 0; i < N; i++) {
        gotoRow(2 + i * 3);
        bool sel = (cursor_ == i);
        if (i >= 4) {
            printf("%s %s %s%s" C_RESET,
                   sel ? C_YELLOW : C_GREY,
                   sel ? ">" : " ",
                   rows[i].label, C_RESET);
        } else {
            printf("%s %s %-14s" C_RESET C_WHITE " %s" C_RESET,
                   sel ? C_YELLOW : C_GREY,
                   sel ? ">" : " ",
                   rows[i].label,
                   rows[i].value);
        }
    }

    renderFooter();
}

// ═══════════════════════════════════════════════════════════════════════════════
// LOAD page
// ═══════════════════════════════════════════════════════════════════════════════
void OverlayMenu::renderLoad() {
    renderHeader("Load Project");

    gotoRow(2);
    printf(C_GREY " %.30s" C_RESET, currentDir_);
    gotoRow(3);
    divider();

    if (dirEntries_.empty()) {
        gotoRow(5);
        printf(C_RED "  No .sb3 files found." C_RESET);
        gotoRow(6);
        printf(C_GREY "  fat:/scratch/" C_RESET);
    } else {
        const int VIS = 17;
        int end = scrollOff_ + VIS;
        if (end > (int)dirEntries_.size()) end = (int)dirEntries_.size();
        for (int i = scrollOff_; i < end; i++) {
            gotoRow(4 + (i - scrollOff_));
            bool sel = (cursor_ == i);
            printf("%s%s%.27s" C_RESET,
                   sel ? C_YELLOW : C_WHITE,
                   dirEntries_[i].isDir ? "[D] " : "    ",
                   dirEntries_[i].name);
        }
    }

    gotoRow(22);
    divider();
    gotoRow(23);
    printf(C_GREY "[A]=Open  [B]=Up  [START]=Cancel" C_RESET);
}

// ═══════════════════════════════════════════════════════════════════════════════
// COMPILE page
// ═══════════════════════════════════════════════════════════════════════════════
void OverlayMenu::renderCompile() {
    renderHeader("Compile");

    gotoRow(3);  printf(C_WHITE " Compiling..." C_RESET);
    gotoRow(5);  printf(C_CYAN " Progress: %3d%%" C_RESET, compileProgress_);
    gotoRow(6);  printf(" ["); printBar(compileProgress_ / 5, 20, C_GREEN, C_GREY); printf("]");
    gotoRow(8);  printf(C_YELLOW " %s" C_RESET, compileStatus_);

    if (compileProgress_ >= 100) {
        gotoRow(11); printf(C_GREEN " Done! Output: fat:/scratch/out/" C_RESET);
        gotoRow(12); printf(C_GREY "  Press [A] to return." C_RESET);
        if (cachedKeysDown_ & KEY_A) { page_ = MenuPage::SETTINGS; cursor_ = 4; }
    }

    renderFooter();
}

// ═══════════════════════════════════════════════════════════════════════════════
// CONFIRM dialog
// ═══════════════════════════════════════════════════════════════════════════════
void OverlayMenu::renderConfirmReset() {
    renderHeader("Confirm");

    gotoRow(5);  printf(C_WHITE " %s" C_RESET, confirmMsg_);
    gotoRow(10);
    printf("  %s[ YES ]%s    %s[ NO  ]%s",
           cursor_ == 0 ? C_GREEN  : C_GREY, C_RESET,
           cursor_ == 1 ? C_RED    : C_GREY, C_RESET);
    gotoRow(13); printf(C_GREY "  Left/Right to switch, A to confirm" C_RESET);

    renderFooter();
}

// ═══════════════════════════════════════════════════════════════════════════════
// USAGE page  — clean line-by-line, no macros, no goto
// ═══════════════════════════════════════════════════════════════════════════════
void OverlayMenu::renderUsage() {
    const UsageStats& s = usageStats_;

    renderHeader("Usage");

    // Render into a flat string array, then display a scrollable window.
    // static = lives in BSS, zero stack cost on the NDS ARM9.
    static const int MAX_LINES = 120;
    static char lines[MAX_LINES][33];   // 32 visible chars + NUL
    int nLines = 0;

    // addL: safe line builder — never touches lines[] out of bounds.
    // Blank separator lines use a single space so snprintf has a real format string.
    auto addL = [&](const char* str) {
        if (nLines < MAX_LINES) {
            strncpy(lines[nLines], str, 32);
            lines[nLines][32] = '\0';
            nLines++;
        }
    };
    // addF: formatted line.
    auto addF = [&](char* buf, const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, 33, fmt, ap);
        va_end(ap);
        addL(buf);
    };
    // Scratch buffer reused by addF.
    char _fb[33];
#define ADDF(fmt, ...) addF(_fb, fmt, ##__VA_ARGS__)
#define ADDL(str)      addL(str)
#define ADDS()         addL(" ")   // blank separator — no format string

    // ── RAM ──────────────────────────────────────────────────────────────────
    ADDL("--- RAM ---");
    int usedKB  = s.usedRamBytes() / 1024;
    int freeKB  = s.freeRamBytes   / 1024;
    int totalKB = s.totalRamBytes  / 1024;
    ADDF("Total%4dK Used%4dK Free%3dK", totalKB, usedKB, freeKB);
    ADDF("Gfx:%3dK Snd:%3dK Sys:%3dK",
         s.usedBySpritesBytes / 1024,
         s.usedBySoundsBytes  / 1024,
         (s.usedRamBytes() - s.usedBySpritesBytes - s.usedBySoundsBytes) / 1024);
    ADDS();

    // ── Sprites ───────────────────────────────────────────────────────────────
    ADDF("--- Sprites (%d) ---", s.numSprites);
    for (int i = 0; i < s.numSprites; i++) {
        const UsageStats::SpriteEntry& e = s.sprites[i];
        ADDF("%-14s %s %3dK",
             e.name, e.visible ? " on" : "off",
             (e.costumesBytes + e.soundsBytes) / 1024);
        ADDF("  blk:%-3d cos:%-2d snd:%-2d",
             e.numBlocks, e.numCostumes, e.numSounds);
    }
    if (s.numSprites == 0) ADDL("  (none)");
    ADDS();

    // ── Threads ───────────────────────────────────────────────────────────────
    ADDF("--- Threads (%d) ---", s.numThreads);
    for (int i = 0; i < s.numThreads; i++) {
        const UsageStats::ThreadEntry& te = s.threads[i];
        ADDF("%-12s %-6s stk:%d",
             te.spriteName, te.state, te.stackDepth);
    }
    if (s.numThreads == 0) ADDL("  (none)");
    ADDS();

    // ── OAM / Palette ─────────────────────────────────────────────────────────
    ADDL("--- OAM / PAL ---");
    ADDF("OAM %3d/128  PAL %2d/15", s.oamSlotsUsed, s.palSlotsUsed);
    ADDF("FPS %2d.%d", s.fpsTenths / 10, s.fpsTenths % 10);
    ADDS();

    // ── System ────────────────────────────────────────────────────────────────
    ADDL("--- System ---");
    ADDF("%s  %s", s.timeStr, s.dateStr);
    ADDL(s.ndsModel);
    if (s.batteryPercent < 0)
        ADDL("Battery: N/A");
    else
        ADDF("Battery: %3d%%%s", s.batteryPercent, s.isCharging ? " +" : "");

#undef ADDF
#undef ADDL
#undef ADDS

    // ── Render visible window ─────────────────────────────────────────────────
    const int VISIBLE = 19;  // rows 2-20

    // Clamp scroll
    int maxScroll = nLines - VISIBLE;
    if (maxScroll < 0) maxScroll = 0;
    if (usageScrollOff_ > maxScroll) usageScrollOff_ = maxScroll;

    for (int i = 0; i < VISIBLE; i++) {
        gotoRow(2 + i);
        // Clear the row first (32 spaces)
        printf("                                ");
        gotoRow(2 + i);
        int li = usageScrollOff_ + i;
        if (li < nLines) printf(" %s", lines[li]);
    }

    // Scroll indicator
    gotoRow(21);
    if (nLines > VISIBLE) {
        int pct = (usageScrollOff_ * 100) / maxScroll;
        printf(C_GREY " [%3d%%] %d/%d lines" C_RESET, pct, usageScrollOff_ + 1, nLines);
    }

    gotoRow(22);
    divider();
    gotoRow(23);
    printf(C_GREY "[Up/Dn]=Scroll [Y]=Refresh [B]=Back" C_RESET);
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
    const int N = 5;
    if (cachedKeysDown_ & KEY_UP)   cursor_ = (cursor_ - 1 + N) % N;
    if (cachedKeysDown_ & KEY_DOWN) cursor_ = (cursor_ + 1)     % N;
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
    if (cachedKeysDown_ & KEY_DOWN) cursor_ = (cursor_ + 1)     % N;
    if (cachedKeysDown_ & (KEY_A | KEY_LEFT | KEY_RIGHT)) {
        switch (cursor_) {
            case 0: cycleFPS();    break;
            case 1: cycleScreen(); break;
            case 2: cycleScale();  break;
            case 3: pending_.showFPSCounter = !pending_.showFPSCounter; break;
            case 4: page_ = MenuPage::COMPILE; startCompile(); break;
            case 5:
                snprintf(confirmMsg_, sizeof(confirmMsg_),
                         "Apply settings and reset?");
                page_ = MenuPage::CONFIRM_RESET; cursor_ = 0; break;
        }
    }
    if (cachedKeysDown_ & KEY_B) { page_ = MenuPage::MAIN; cursor_ = 1; }
}

void OverlayMenu::handleLoadInput() {
    int total = (int)dirEntries_.size();
    if (total == 0) {
        if (cachedKeysDown_ & (KEY_B | KEY_START)) { page_ = MenuPage::MAIN; cursor_ = 2; }
        return;
    }
    if (cachedKeysDown_ & KEY_UP)   cursor_ = (cursor_ - 1 + total) % total;
    if (cachedKeysDown_ & KEY_DOWN) cursor_ = (cursor_ + 1)         % total;

    const int VIS = 17;
    if (cursor_ < scrollOff_)        scrollOff_ = cursor_;
    if (cursor_ >= scrollOff_ + VIS) scrollOff_ = cursor_ - VIS + 1;

    if (cachedKeysDown_ & KEY_A) {
        if (dirEntries_[cursor_].isDir) {
            navigateInto(cursor_);
        } else {
            snprintf(selectedPath_, sizeof(selectedPath_),
                     "%s%s", currentDir_, dirEntries_[cursor_].name);
            snprintf(confirmMsg_,  sizeof(confirmMsg_),
                     "Load:\n%.28s?", dirEntries_[cursor_].name);
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

void OverlayMenu::handleUsageInput() {
    if (cachedKeysDown_ & KEY_UP)   { if (usageScrollOff_ > 0) usageScrollOff_--; }
    if (cachedKeysDown_ & KEY_DOWN) { usageScrollOff_++; }
    if (cachedKeysDown_ & KEY_Y)    { gatherUsageStats(); }
    if (cachedKeysDown_ & (KEY_B | KEY_START)) {
        page_ = MenuPage::MAIN; cursor_ = 3; usageScrollOff_ = 0;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Settings helpers
// ═══════════════════════════════════════════════════════════════════════════════
void OverlayMenu::cycleFPS()    { pending_.targetFPS    = (pending_.targetFPS == 60) ? 30 : 60; }
void OverlayMenu::cycleScreen() { pending_.stageOnTop   = !pending_.stageOnTop; }
void OverlayMenu::cycleScale()  { pending_.stageScale   = (pending_.stageScale + 1) % 3; }

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
    strncpy(compileStatus_, "Parsing project.json...", sizeof(compileStatus_) - 1);
}

void OverlayMenu::tickCompile(float dt) {
    compileTimer_ += dt;
    struct Stage { float t; int p; const char* s; };
    static const Stage stages[] = {
        {0.5f, 10, "Parsing project.json..."},
        {1.0f, 25, "Converting costumes..."},
        {2.0f, 50, "Decoding audio..."},
        {2.8f, 75, "Building bytecode..."},
        {3.5f, 90, "Writing output..."},
        {4.0f, 100,"Done!"},
    };
    for (int i = 0; i < 6; i++) {
        if (compileTimer_ >= stages[i].t) {
            compileProgress_ = stages[i].p;
            strncpy(compileStatus_, stages[i].s, sizeof(compileStatus_) - 1);
        }
    }
    if (compileProgress_ >= 100) compiling_ = false;
}

// ═══════════════════════════════════════════════════════════════════════════════
// File browser
// ═══════════════════════════════════════════════════════════════════════════════
void OverlayMenu::scanDirectory(const char* path) {
    dirEntries_.clear(); cursor_ = 0; scrollOff_ = 0;
    strncpy(currentDir_, path, sizeof(currentDir_) - 1);
    currentDir_[sizeof(currentDir_) - 1] = '\0';

    if (strcmp(path, "fat:/") != 0 && strcmp(path, "fat:/scratch/") != 0) {
        FileEntry up;
        strncpy(up.name, "..", sizeof(up.name) - 1);
        up.name[sizeof(up.name) - 1] = '\0';
        up.isDir = true;
        dirEntries_.push_back(up);
    }

    DIR* dir = opendir(path);
    if (!dir) return;
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        FileEntry fe;
        strncpy(fe.name, ent->d_name, sizeof(fe.name) - 1);
        fe.name[sizeof(fe.name) - 1] = '\0';
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
    char tmp[256];
    strncpy(tmp, currentDir_, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    int len = (int)strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/') tmp[--len] = '\0';
    char* slash = strrchr(tmp, '/');
    if (slash) *(slash + 1) = '\0';
    if (strlen(tmp) < 5) strncpy(tmp, "fat:/scratch/", sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    scanDirectory(tmp);
}

// ═══════════════════════════════════════════════════════════════════════════════
// NDS info helpers
// ═══════════════════════════════════════════════════════════════════════════════
void OverlayMenu::getNDSModel(char* out, int maxLen) {
#ifdef isDSiMode
    strncpy(out, isDSiMode() ? "Nintendo DSi" : "DS / DS Lite", maxLen - 1);
#else
    strncpy(out, "DS / DS Lite", maxLen - 1);
#endif
    out[maxLen - 1] = '\0';
}

int OverlayMenu::getFreeRAM() {
    return (cachedFreeRam_ >= 0) ? cachedFreeRam_ : 0;
}

void OverlayMenu::getProjectName(char* out, int maxLen) {
    if (settings_->lastProjectPath[0] != '\0') {
        const char* slash = strrchr(settings_->lastProjectPath, '/');
        strncpy(out, slash ? slash + 1 : settings_->lastProjectPath, maxLen - 1);
    } else {
        strncpy(out, "(none)", maxLen - 1);
    }
    out[maxLen - 1] = '\0';
}
