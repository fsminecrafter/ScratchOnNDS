// =============================================================================
// overlay_menu.cpp — Fixed console overlap + no extra scanKeys() calls
//
// FIXES:
//  1. consoleClear() is called at the start of render(), open(), and close()
//     so there is never leftover text from a previous state.
//  2. DPadTextInput still calls scanKeys() internally — it is a standalone
//     modal widget used only from selectProject() before the main loop
//     starts, so it is safe there.  Inside the main loop the overlay menu
//     reads InputHandler::getKeysDown() / getKeysHeld() exclusively.
//  3. The combo-hold detection in update() reads from cachedKeysHeld_ which
//     is set from InputHandler — no raw keysHeld() call.
// =============================================================================
#include "overlay_menu.h"
#include "../input/input_handler.h"
#include "../scratch_extension/nds_extension.h"
#include <nds.h>
#include <fat.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define COL_WHITE   "\x1b[37;1m"
#define COL_CYAN    "\x1b[36;1m"
#define COL_YELLOW  "\x1b[33;1m"
#define COL_GREEN   "\x1b[32;1m"
#define COL_RED     "\x1b[31;1m"
#define COL_GREY    "\x1b[37;0m"
#define COL_RESET   "\x1b[0m"

// -----------------------------------------------------------------------
// ScratchDSSettings
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
// DPadTextInput — standalone modal; owns its scanKeys() for pre-loop use
// -----------------------------------------------------------------------
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
    // Safe to call scanKeys here — this widget is only used BEFORE the
    // main loop (in selectProject), so InputHandler hasn't started yet.
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

// -----------------------------------------------------------------------
// RAM measurement (binary-search, one probe)
// -----------------------------------------------------------------------
static int s_freeRamCache = -1;

static int measureFreeRAM() {
    int lo = 0, hi = 3 * 1024 * 1024;
    while (hi - lo > 4096) {
        int mid = (lo + hi) / 2;
        void* p = malloc((size_t)mid);
        if (p) { free(p); lo = mid; } else { hi = mid; }
    }
    return lo;
}

// -----------------------------------------------------------------------
// OverlayMenu::init
// -----------------------------------------------------------------------
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
    currentDir_[0]   = selectedPath_[0] = confirmMsg_[0] = '\0';
    confirmResult_   = false;
    cachedKeysDown_  = cachedKeysHeld_ = 0;
    strncpy(currentDir_, "fat:/scratch/", sizeof(currentDir_) - 1);
}

// -----------------------------------------------------------------------
// update — reads InputHandler cache, NEVER calls scanKeys()
// -----------------------------------------------------------------------
bool OverlayMenu::update(float dt) {
    // Pull fresh cached masks from InputHandler (which already called
    // scanKeys() once this frame in its own update()).
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

// -----------------------------------------------------------------------
// open / close — always clear console so no ghost text remains
// -----------------------------------------------------------------------
void OverlayMenu::open() {
    open_      = true;
    page_      = MenuPage::MAIN;
    cursor_    = 0;
    scrollOff_ = 0;
    pending_   = *settings_;
    s_freeRamCache = -1;  // re-measure RAM on next Info page view

    if (externalConsole_) consoleSelect(externalConsole_);
    consoleClear();
}

void OverlayMenu::close() {
    open_ = false;
    if (externalConsole_) consoleSelect(externalConsole_);
    consoleClear();
}

// -----------------------------------------------------------------------
// consoleClear helper (always selects our console first)
// -----------------------------------------------------------------------
void OverlayMenu::consoleClear() {
    if (externalConsole_) consoleSelect(externalConsole_);
    ::consoleClear();
}

// -----------------------------------------------------------------------
// Input dispatch — all methods read cachedKeysDown_ / cachedKeysHeld_
// -----------------------------------------------------------------------
void OverlayMenu::handleInput() {
    switch (page_) {
        case MenuPage::MAIN:         handleMainInput();     break;
        case MenuPage::INFO:         handleInfoInput();     break;
        case MenuPage::SETTINGS:     handleSettingsInput(); break;
        case MenuPage::LOAD:         handleLoadInput();     break;
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
    const int N = 4;
    if (cachedKeysDown_ & KEY_UP)    cursor_ = (cursor_ - 1 + N) % N;
    if (cachedKeysDown_ & KEY_DOWN)  cursor_ = (cursor_ + 1) % N;
    if (cachedKeysDown_ & (KEY_A | KEY_START)) {
        switch (cursor_) {
            case 0: page_ = MenuPage::INFO;     cursor_ = 0; break;
            case 1: page_ = MenuPage::SETTINGS; cursor_ = 0; pending_ = *settings_; break;
            case 2: page_ = MenuPage::LOAD;     cursor_ = 0; scrollOff_ = 0;
                    scanDirectory(currentDir_); break;
            case 3: close(); break;
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
    if (cursor_ < scrollOff_)           scrollOff_ = cursor_;
    if (cursor_ >= scrollOff_ + VIS)    scrollOff_ = cursor_ - VIS + 1;

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

// -----------------------------------------------------------------------
// Render — ALWAYS consoleClear() first, then draw fresh content
// -----------------------------------------------------------------------
void OverlayMenu::render() {
    if (externalConsole_) consoleSelect(externalConsole_);
    ::consoleClear();   // wipe every frame before redraw — no ghost text

    switch (page_) {
        case MenuPage::MAIN:          renderMain();         break;
        case MenuPage::INFO:          renderInfo();         break;
        case MenuPage::SETTINGS:      renderSettings();     break;
        case MenuPage::LOAD:          renderLoad();         break;
        case MenuPage::COMPILE:       renderCompile();      break;
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
    const char* items[] = { "  Info", "  Settings", "  Load Project", "  Resume" };
    const char* icons[] = { "i", "*", "^", ">" };
    for (int i = 0; i < 4; i++) {
        bool sel = (cursor_ == i);
        printf("%s[%s]%s%s\n", sel ? COL_YELLOW : COL_GREY, icons[i], items[i], COL_RESET);
        printf("\n");
    }
    printf("\n");
    printf(COL_GREY " Hold L+R+B to reopen menu" COL_RESET "\n");
    renderFooter();
}

void OverlayMenu::renderInfo() {
    renderHeader("Info");
    if (s_freeRamCache < 0) s_freeRamCache = measureFreeRAM();

    static const int TOTAL_RAM = 4 * 1024 * 1024;
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
    snprintf(fpsStr,      sizeof(fpsStr),     "%d",  pending_.targetFPS);
    snprintf(scaleStr,    sizeof(scaleStr),   "%s",
             pending_.stageScale == 0 ? "Stretch" :
             pending_.stageScale == 1 ? "Aspect"  : "Native");
    snprintf(fpsCountStr, sizeof(fpsCountStr),"%s",
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

// -----------------------------------------------------------------------
// Settings helpers
// -----------------------------------------------------------------------
void OverlayMenu::cycleFPS()    { pending_.targetFPS = (pending_.targetFPS == 60) ? 30 : 60; }
void OverlayMenu::cycleScreen() { pending_.stageOnTop = !pending_.stageOnTop; }
void OverlayMenu::cycleScale()  { pending_.stageScale = (pending_.stageScale + 1) % 3; }

void OverlayMenu::applySettings() {
    *settings_ = pending_;
    if (settings_->autoSaveSettings) settings_->save();
    if (onApply_) onApply_(*settings_);
}

// -----------------------------------------------------------------------
// Compile simulation
// -----------------------------------------------------------------------
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

// -----------------------------------------------------------------------
// File browser
// -----------------------------------------------------------------------
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

// -----------------------------------------------------------------------
// NDS info helpers
// -----------------------------------------------------------------------
void OverlayMenu::getNDSModel(char* out, int maxLen) {
#ifdef isDSiMode
    strncpy(out, isDSiMode() ? "Nintendo DSi" : "Nintendo DS / DS Lite", maxLen-1);
#else
    strncpy(out, "Nintendo DS / DS Lite", maxLen-1);
#endif
    out[maxLen-1] = '\0';
}
int  OverlayMenu::getFreeRAM()                    { return measureFreeRAM(); }
void OverlayMenu::getProjectName(char* out, int maxLen) {
    if (settings_->lastProjectPath[0] != '\0') {
        const char* slash = strrchr(settings_->lastProjectPath, '/');
        strncpy(out, slash ? slash+1 : settings_->lastProjectPath, maxLen-1);
    } else {
        strncpy(out, "(example.sb3)", maxLen-1);
    }
    out[maxLen-1] = '\0';
}
