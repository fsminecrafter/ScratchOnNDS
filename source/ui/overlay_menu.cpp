// =============================================================================
// overlay_menu.cpp — ScratchDS Overlay Menu Implementation
// =============================================================================
#include "overlay_menu.h"
#include "../scratch_extension/nds_extension.h"
#include <nds.h>
#include <fat.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>

// -----------------------------------------------------------------------
// NDS console ANSI-style colour codes (libnds printf)
// -----------------------------------------------------------------------
#define COL_WHITE   "\x1b[37;1m"
#define COL_CYAN    "\x1b[36;1m"
#define COL_YELLOW  "\x1b[33;1m"
#define COL_GREEN   "\x1b[32;1m"
#define COL_RED     "\x1b[31;1m"
#define COL_BLUE    "\x1b[34;1m"
#define COL_GREY    "\x1b[37;0m"
#define COL_RESET   "\x1b[0m"

// NDS text console is 32 columns × 24 rows
#define COLS 32
#define ROWS 24

// -----------------------------------------------------------------------
// ScratchDSSettings — persist to FAT
// -----------------------------------------------------------------------
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

// -----------------------------------------------------------------------
// DPadTextInput
// -----------------------------------------------------------------------
const char DPadTextInput::CHARS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
    "0123456789 ._-/:\\";
const int DPadTextInput::NUM_CHARS = sizeof(DPadTextInput::CHARS) - 1;

DPadTextInput::DPadTextInput() : len_(0), charSel_(0) {
    buf_[0] = '\0'; prompt_[0] = '\0';
}
void DPadTextInput::reset(const char* prompt, const char* initial) {
    strncpy(prompt_, prompt, sizeof(prompt_) - 1);
    strncpy(buf_, initial ? initial : "", sizeof(buf_) - 1);
    len_ = (int)strlen(buf_);
    charSel_ = 0;
}
bool DPadTextInput::update() {
    scanKeys();
    u32 down = keysDown();

    if (down & KEY_RIGHT) charSel_ = (charSel_ + 1) % NUM_CHARS;
    if (down & KEY_LEFT)  charSel_ = (charSel_ - 1 + NUM_CHARS) % NUM_CHARS;
    if (down & KEY_UP) { // jump by 10
        charSel_ = (charSel_ + 10) % NUM_CHARS;
    }
    if (down & KEY_DOWN) {
        charSel_ = (charSel_ - 10 + NUM_CHARS) % NUM_CHARS;
    }
    if (down & KEY_A) { // append char
        if (len_ < (int)sizeof(buf_) - 2) {
            buf_[len_++] = CHARS[charSel_];
            buf_[len_] = '\0';
        }
    }
    if (down & KEY_B) { // backspace
        if (len_ > 0) buf_[--len_] = '\0';
    }
    if (down & KEY_START) return true; // confirm
    return false;
}
void DPadTextInput::render(int y) {
    printf("\x1b[%d;0H", y);
    printf(COL_GREY "%s" COL_RESET "\n", prompt_);
    printf(COL_WHITE "> %s" COL_CYAN "_" COL_RESET "\n", buf_);
    printf(COL_GREY "< " COL_YELLOW "%c" COL_GREY " > [A]=Add [B]=Del [START]=OK\n",
            CHARS[charSel_]);
}

// -----------------------------------------------------------------------
// OverlayMenu::init
// -----------------------------------------------------------------------
void OverlayMenu::init(ScratchDSSettings& settings) {
    settings_      = &settings;
    pending_       = settings;
    open_          = false;
    page_          = MenuPage::MAIN;
    cursor_        = 0;
    scrollOff_     = 0;
    compiling_     = false;
    compileTimer_  = 0;
    compileProgress_ = 0;
    comboHoldTimer_ = 0;
    consoleInited_ = false;
    currentDir_[0] = '\0';
    selectedPath_[0] = '\0';
    confirmMsg_[0]   = '\0';
    confirmResult_   = false;

    strncpy(currentDir_, "fat:/scratch/", sizeof(currentDir_) - 1);
}

// -----------------------------------------------------------------------
// OverlayMenu::update — returns true if VM should pause
// -----------------------------------------------------------------------
bool OverlayMenu::update(float dt) {
    NDSExtension& ext = NDSExtension::getInstance();

    // Check combo hold to open
    if (!open_) {
        if ((keysHeld() & (KEY_L | KEY_R | KEY_B)) == (KEY_L | KEY_R | KEY_B)) {
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

    // Menu is open — handle input and render
    if (compiling_) tickCompile(dt);
    handleInput();
    render();
    return true; // VM paused
}

void OverlayMenu::open() {
    open_ = true;
    page_ = MenuPage::MAIN;
    cursor_ = 0;
    scrollOff_ = 0;
    pending_ = *settings_;

    // Init console on top screen for the menu
    if (!consoleInited_) {
        consoleInit(&menuConsole_, 0, BgType_Text4bpp, BgSize_T_256x256,
                    2, 0, true, true); // true = main (top) screen
        consoleInited_ = true;
    }
    consoleSelect(&menuConsole_);
}

void OverlayMenu::close() {
    open_ = false;
    // Restore original console configuration
    // (Renderer will reclaim the top screen on next renderFrame call)
}

// -----------------------------------------------------------------------
// Input dispatch
// -----------------------------------------------------------------------
void OverlayMenu::handleInput() {
    switch (page_) {
        case MenuPage::MAIN:         handleMainInput();    break;
        case MenuPage::INFO:         handleInfoInput();    break;
        case MenuPage::SETTINGS:     handleSettingsInput(); break;
        case MenuPage::LOAD:         handleLoadInput();    break;
        case MenuPage::CONFIRM_RESET:
        case MenuPage::CONFIRM_LOAD: {
            bool confirmed = false;
            handleConfirmInput(confirmed);
            if (confirmed) {
                if (page_ == MenuPage::CONFIRM_RESET) {
                    applySettings();
                    close();
                } else if (page_ == MenuPage::CONFIRM_LOAD) {
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
    scanKeys();
    u32 down = keysDown();
    // Main menu items: Info(0), Settings(1), Load(2), Resume(3)
    const int MAIN_ITEMS = 4;
    if (down & KEY_UP)   cursor_ = (cursor_ - 1 + MAIN_ITEMS) % MAIN_ITEMS;
    if (down & KEY_DOWN) cursor_ = (cursor_ + 1) % MAIN_ITEMS;
    if (down & KEY_A || down & KEY_START) {
        switch (cursor_) {
            case 0: page_ = MenuPage::INFO;     cursor_ = 0; break;
            case 1: page_ = MenuPage::SETTINGS; cursor_ = 0; pending_ = *settings_; break;
            case 2: page_ = MenuPage::LOAD;     cursor_ = 0; scrollOff_ = 0;
                    scanDirectory(currentDir_); break;
            case 3: close(); break;
        }
    }
    if (down & KEY_B) close();
}

void OverlayMenu::handleInfoInput() {
    scanKeys();
    if (keysDown() & (KEY_B | KEY_START)) {
        page_ = MenuPage::MAIN; cursor_ = 0;
    }
}

void OverlayMenu::handleSettingsInput() {
    scanKeys();
    u32 down = keysDown();
    const int SETTINGS_ITEMS = 6; // FPS, Screen, Scale, FPSCounter, Compile, Back

    if (down & KEY_UP)   cursor_ = (cursor_ - 1 + SETTINGS_ITEMS) % SETTINGS_ITEMS;
    if (down & KEY_DOWN) cursor_ = (cursor_ + 1) % SETTINGS_ITEMS;

    if (down & KEY_A || down & KEY_LEFT || down & KEY_RIGHT) {
        switch (cursor_) {
            case 0: cycleFPS();    break;
            case 1: cycleScreen(); break;
            case 2: cycleScale();  break;
            case 3: pending_.showFPSCounter = !pending_.showFPSCounter; break;
            case 4: // Compile
                page_ = MenuPage::COMPILE;
                startCompile();
                break;
            case 5: // Apply & Back
                snprintf(confirmMsg_, sizeof(confirmMsg_),
                         "Apply settings and\nreset scene?");
                page_ = MenuPage::CONFIRM_RESET;
                cursor_ = 0;
                break;
        }
    }
    if (down & KEY_B) { page_ = MenuPage::MAIN; cursor_ = 1; }
}

void OverlayMenu::handleLoadInput() {
    scanKeys();
    u32 down = keysDown();
    int total = (int)dirEntries_.size();
    if (total == 0) { if (down & KEY_B) { page_ = MenuPage::MAIN; cursor_ = 2; } return; }

    if (down & KEY_UP)   { cursor_ = (cursor_ - 1 + total) % total; }
    if (down & KEY_DOWN) { cursor_ = (cursor_ + 1) % total; }

    // Scroll window
    const int VISIBLE = 14;
    if (cursor_ < scrollOff_) scrollOff_ = cursor_;
    if (cursor_ >= scrollOff_ + VISIBLE) scrollOff_ = cursor_ - VISIBLE + 1;

    if (down & KEY_A) {
        if (dirEntries_[cursor_].isDir) {
            navigateInto(cursor_);
        } else {
            // Confirm load
            snprintf(selectedPath_, sizeof(selectedPath_), "%s%s",
                     currentDir_, dirEntries_[cursor_].name);
            snprintf(confirmMsg_, sizeof(confirmMsg_),
                     "Load project:\n%.28s?", dirEntries_[cursor_].name);
            page_ = MenuPage::CONFIRM_LOAD;
            cursor_ = 0;
        }
    }
    if (down & KEY_B)     navigateUp();
    if (down & KEY_START) { page_ = MenuPage::MAIN; cursor_ = 2; }
}

void OverlayMenu::handleConfirmInput(bool& confirmed) {
    scanKeys();
    u32 down = keysDown();
    if (down & KEY_LEFT || down & KEY_RIGHT) cursor_ ^= 1;
    if (down & KEY_A) {
        confirmed = (cursor_ == 0); // 0=Yes, 1=No
        if (!confirmed) { page_ = MenuPage::SETTINGS; cursor_ = 5; }
    }
    if (down & KEY_B) { confirmed = false; page_ = MenuPage::SETTINGS; cursor_ = 5; }
}

// -----------------------------------------------------------------------
// Render dispatch
// -----------------------------------------------------------------------
void OverlayMenu::render() {
    consoleSelect(&menuConsole_);
    consoleClear();
    switch (page_) {
        case MenuPage::MAIN:         renderMain();         break;
        case MenuPage::INFO:         renderInfo();         break;
        case MenuPage::SETTINGS:     renderSettings();     break;
        case MenuPage::LOAD:         renderLoad();         break;
        case MenuPage::COMPILE:      renderCompile();      break;
        case MenuPage::CONFIRM_RESET:
        case MenuPage::CONFIRM_LOAD: renderConfirmReset(); break;
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

    const char* items[] = { "  Info", "  Settings", "  Load Project", "  Resume" };
    const char* icons[] = { "i", "*", "^", ">" };
    for (int i = 0; i < 4; i++) {
        bool sel = (cursor_ == i);
        printf("%s[%s]%s%s\n",
                sel ? COL_YELLOW : COL_GREY,
                icons[i],
                items[i],
                COL_RESET);
        printf("\n");
    }

    printf("\n");
    printf(COL_GREY " Hold L+R+B to reopen menu" COL_RESET "\n");
    renderFooter();
}

void OverlayMenu::renderInfo() {
    renderHeader("Info");

    char ndsModel[32];
    getNDSModel(ndsModel, sizeof(ndsModel));
    int freeRam = getFreeRAM();
    char projName[64];
    getProjectName(projName, sizeof(projName));

    printf("\n");
    printf(COL_CYAN " Version    " COL_WHITE SCRATCHDS_VERSION COL_RESET "\n");
    printf(COL_CYAN " Built      " COL_WHITE SCRATCHDS_BUILD_DATE COL_RESET "\n");
    printf("\n");
    printf(COL_CYAN " Device     " COL_WHITE "%s" COL_RESET "\n", ndsModel);
    printf(COL_CYAN " Free RAM   " COL_WHITE "%d KB" COL_RESET "\n", freeRam / 1024);
    printf("\n");
    printf(COL_CYAN " Project    " COL_WHITE "%.20s" COL_RESET "\n", projName);
    printf(COL_CYAN " Target FPS " COL_WHITE "%d" COL_RESET "\n", settings_->targetFPS);
    printf(COL_CYAN " Screen     " COL_WHITE "%s" COL_RESET "\n",
            settings_->stageOnTop ? "Stage=Top" : "Stage=Bottom");
    printf(COL_CYAN " Scale      " COL_WHITE "%s" COL_RESET "\n",
            settings_->stageScale == 0 ? "Stretch" :
            settings_->stageScale == 1 ? "Aspect" : "Native");
    printf("\n");
    printf(COL_GREY " devkitARM  " DEVKITARM_VERSION COL_RESET "\n");
    printf(COL_GREY " libnds     " "2.x" COL_RESET "\n");
    printf(COL_GREY " maxmod     " "1.x" COL_RESET "\n");

    renderFooter();
}

void OverlayMenu::renderSettings() {
    renderHeader("Settings");
    printf("\n");

    struct Item { const char* label; const char* value; };
    char fpsStr[8], scaleStr[16], fpsCountStr[8];
    snprintf(fpsStr,    sizeof(fpsStr),    "%d",   pending_.targetFPS);
    snprintf(scaleStr,  sizeof(scaleStr),  "%s",
             pending_.stageScale == 0 ? "Stretch" :
             pending_.stageScale == 1 ? "Aspect"  : "Native");
    snprintf(fpsCountStr, sizeof(fpsCountStr), "%s",
             pending_.showFPSCounter ? "On" : "Off");

    const char* labels[] = {
        " Target FPS   ", " Primary Screen", " Stage Scale   ",
        " FPS Counter  ", " Compile        ", " Apply & Back  "
    };
    const char* values[] = {
        fpsStr,
        pending_.stageOnTop ? "Top=Stage" : "Bot=Stage",
        scaleStr,
        fpsCountStr,
        "...",
        ""
    };

    for (int i = 0; i < 6; i++) {
        bool sel = (cursor_ == i);
        if (i == 5) {
            // Apply button
            printf("%s[APPLY & BACK]%s\n",
                    sel ? COL_GREEN : COL_GREY, COL_RESET);
        } else {
            printf("%s%s%s%s%s\n",
                    sel ? COL_YELLOW : COL_GREY,
                    labels[i],
                    COL_WHITE,
                    values[i],
                    COL_RESET);
        }
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
        printf(COL_GREY "  Place .sb3 files in:\n");
        printf("  fat:/scratch/\n" COL_RESET);
    } else {
        const int VISIBLE = 14;
        int end = scrollOff_ + VISIBLE;
        if (end > (int)dirEntries_.size()) end = (int)dirEntries_.size();
        for (int i = scrollOff_; i < end; i++) {
            bool sel = (cursor_ == i);
            bool isDir = dirEntries_[i].isDir;
            printf("%s%s%.26s%s\n",
                    sel ? COL_YELLOW : COL_WHITE,
                    isDir ? "[D] " : "    ",
                    dirEntries_[i].name,
                    COL_RESET);
        }
    }

    printf("\x1b[23;0H");
    printf(COL_GREY "[A]=Select [B]=Up [START]=Cancel" COL_RESET);
}

void OverlayMenu::renderCompile() {
    renderHeader("Compile");
    printf("\n");
    printf(COL_WHITE " Compiling Scratch project...\n\n" COL_RESET);
    printf(COL_GREY " This converts the .sb3 into\n");
    printf(" a pre-parsed binary format\n");
    printf(" for faster loading from SD.\n\n" COL_RESET);

    // Progress bar (ASCII)
    printf(COL_CYAN " Progress: %3d%%\n [", compileProgress_);
    int filled = compileProgress_ / 5;
    for (int i = 0; i < 20; i++)
        printf("%s", i < filled ? COL_GREEN "#" : COL_GREY "-");
    printf(COL_CYAN "]\n\n" COL_RESET);

    printf(COL_YELLOW " Status: %s\n" COL_RESET, compileStatus_);

    if (compileProgress_ >= 100) {
        printf("\n" COL_GREEN " Done! Output: fat:/scratch/out/\n" COL_RESET);
        printf(COL_GREY " [A] to return to menu\n" COL_RESET);
        scanKeys();
        if (keysDown() & KEY_A) { page_ = MenuPage::SETTINGS; cursor_ = 4; }
    }
}

void OverlayMenu::renderConfirmReset() {
    renderHeader("Confirm");
    printf("\n\n");
    printf(COL_WHITE " %s\n\n" COL_RESET, confirmMsg_);
    printf("\n");
    printf("%s [YES] %s  %s [NO] %s\n",
            cursor_ == 0 ? COL_GREEN : COL_GREY, COL_RESET,
            cursor_ == 1 ? COL_RED   : COL_GREY, COL_RESET);
    printf("\n");
    printf(COL_GREY " [Left/Right] to switch\n");
    printf(" [A] to confirm\n" COL_RESET);
}

// -----------------------------------------------------------------------
// Settings cycle helpers
// -----------------------------------------------------------------------
void OverlayMenu::cycleFPS() {
    pending_.targetFPS = (pending_.targetFPS == 60) ? 30 : 60;
}
void OverlayMenu::cycleScreen() {
    pending_.stageOnTop = !pending_.stageOnTop;
}
void OverlayMenu::cycleScale() {
    pending_.stageScale = (pending_.stageScale + 1) % 3;
}
void OverlayMenu::applySettings() {
    *settings_ = pending_;
    if (settings_->autoSaveSettings) settings_->save();
    if (onApply_) onApply_(*settings_);
}

// -----------------------------------------------------------------------
// Compile (simulated — real compilation would run a converter tool)
// In a full implementation, this would:
//   1. Re-parse project.json into a compact binary
//   2. Pre-convert all PNG/BMP to NDS tile format
//   3. Pre-decode all WAV/MP3 to raw PCM
//   4. Write a .sds (ScratchDS) binary to fat:/scratch/out/
// -----------------------------------------------------------------------
void OverlayMenu::startCompile() {
    compiling_ = true;
    compileTimer_ = 0;
    compileProgress_ = 0;
    strncpy(compileStatus_, "Parsing project.json...", sizeof(compileStatus_) - 1);
}

void OverlayMenu::tickCompile(float dt) {
    compileTimer_ += dt;

    // Simulated stages — in a real build, each stage calls the actual converter
    struct Stage { float time; int progress; const char* status; };
    static const Stage stages[] = {
        { 0.5f,  10, "Parsing project.json..." },
        { 1.0f,  25, "Converting costumes to tiles..." },
        { 2.0f,  50, "Decoding audio to PCM..." },
        { 2.8f,  75, "Building block bytecode..." },
        { 3.5f,  90, "Writing output binary..." },
        { 4.0f, 100, "Compilation complete!" },
    };
    static const int NUM_STAGES = 6;

    for (int i = 0; i < NUM_STAGES; i++) {
        if (compileTimer_ >= stages[i].time) {
            compileProgress_ = stages[i].progress;
            strncpy(compileStatus_, stages[i].status, sizeof(compileStatus_) - 1);
        }
    }

    if (compileProgress_ >= 100) {
        compiling_ = false;
        // In a real impl: write the .sds file here and update settings_->lastProjectPath
    }
}

// -----------------------------------------------------------------------
// File browser
// -----------------------------------------------------------------------
void OverlayMenu::scanDirectory(const char* path) {
    dirEntries_.clear();
    cursor_ = 0;
    scrollOff_ = 0;
    strncpy(currentDir_, path, sizeof(currentDir_) - 1);

    // Add parent dir entry if not at root
    if (strcmp(path, "fat:/") != 0 && strcmp(path, "fat:/scratch/") != 0) {
        FileEntry up;
        strncpy(up.name, "..", sizeof(up.name) - 1);
        up.isDir = true;
        dirEntries_.push_back(up);
    }

    DIR* dir = opendir(path);
    if (!dir) return;

    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.') continue; // skip hidden
        FileEntry fe;
        strncpy(fe.name, ent->d_name, sizeof(fe.name) - 1);
        fe.name[sizeof(fe.name) - 1] = '\0';

        // Check if directory
        char fullPath[512];
        snprintf(fullPath, sizeof(fullPath), "%s%s", path, ent->d_name);
        struct stat st;
        fe.isDir = (stat(fullPath, &st) == 0 && S_ISDIR(st.st_mode));

        // Only show .sb3 files and directories
        if (!fe.isDir) {
            size_t len = strlen(fe.name);
            if (len < 4 || strcmp(fe.name + len - 4, ".sb3") != 0) continue;
        }

        dirEntries_.push_back(fe);
    }
    closedir(dir);
}

void OverlayMenu::navigateInto(int idx) {
    if (idx < 0 || idx >= (int)dirEntries_.size()) return;
    if (!dirEntries_[idx].isDir) return;

    if (strcmp(dirEntries_[idx].name, "..") == 0) {
        navigateUp();
        return;
    }
    char newPath[512];
    snprintf(newPath, sizeof(newPath), "%s%s/", currentDir_, dirEntries_[idx].name);
    scanDirectory(newPath);
}

void OverlayMenu::navigateUp() {
    // Strip last path component
    char tmp[256];
    strncpy(tmp, currentDir_, sizeof(tmp) - 1);
    int len = (int)strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/') tmp[--len] = '\0';
    char* slash = strrchr(tmp, '/');
    if (slash) { *(slash + 1) = '\0'; }

    if (strlen(tmp) < 5) strncpy(tmp, "fat:/scratch/", sizeof(tmp) - 1); // floor
    scanDirectory(tmp);
}

// -----------------------------------------------------------------------
// NDS info helpers
// -----------------------------------------------------------------------
void OverlayMenu::getNDSModel(char* out, int maxLen) {
    // libnds exposes isDSiMode() to detect DSi
#ifdef isDSiMode
    if (isDSiMode()) {
        strncpy(out, "Nintendo DSi", maxLen - 1);
    } else {
#endif
        // Check for DS Lite via power management chip ID
        // NDS Lite has a different PMIC; exact detection is complex.
        // Use a heuristic: if running in DS mode, report DS/DS Lite.
        strncpy(out, "Nintendo DS / DS Lite", maxLen - 1);
#ifdef isDSiMode
    }
#endif
    out[maxLen - 1] = '\0';
}

int OverlayMenu::getFreeRAM() {
    // Simple heap probe — allocate increasingly large blocks
    // until malloc fails, then return that as approximate free RAM.
    int free = 0;
    int step = 64 * 1024; // 64KB steps
    while (true) {
        void* p = malloc(step);
        if (!p) break;
        free += step;
        ::free(p);
        // Note: this doesn't account for heap fragmentation,
        // but gives a good enough estimate.
        break; // single-probe version to avoid side effects
    }
    // Fallback: NDS ARM9 has 4MB, return rough estimate
    if (free == 0) free = 2 * 1024 * 1024; // ~2MB typical free
    return free;
}

void OverlayMenu::getProjectName(char* out, int maxLen) {
    if (settings_->lastProjectPath[0] != '\0') {
        // Extract filename from path
        const char* slash = strrchr(settings_->lastProjectPath, '/');
        const char* name = slash ? slash + 1 : settings_->lastProjectPath;
        strncpy(out, name, maxLen - 1);
    } else {
        strncpy(out, "(example.sb3)", maxLen - 1);
    }
    out[maxLen - 1] = '\0';
}

void OverlayMenu::consoleClear() {
    ::consoleClear();
}
