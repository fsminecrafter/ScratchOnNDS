// =============================================================================
// overlay_menu.h / overlay_menu.cpp
// ScratchDS System Overlay Menu
//
// Activated by holding L + R + B simultaneously for ~0.5 seconds.
// Pauses the Scratch VM and renders a full-screen menu on the TOP screen
// (leaving the bottom touchscreen free for menu navigation via touch OR d-pad).
//
// Menu structure:
//   ┌─────────────────────────────┐
//   │  ScratchDS  [version]       │
//   ├─────────────────────────────┤
//   │  > Info                     │
//   │    Settings                 │
//   │    Load                     │
//   │    Resume                   │
//   └─────────────────────────────┘
//
//  Info page:
//    Build date, version, devkitARM version, libnds version,
//    NDS model (Phat/Lite/DSi), RAM free, loaded project name.
//
//  Settings page:
//    - Target FPS         [30 | 60]
//    - Primary Screen     [Top | Bottom]  (stage vs UI screen swap)
//    - Stage Scale        [Full | Aspect | Native]
//    - Compile Project    [Start Compilation...]
//    - Reset Scene        [Confirm]
//
//  Load page:
//    File browser for fat:/scratch/*.sb3
//    Manual path input (via on-screen keyboard or D-pad char select)
//
// =============================================================================
#pragma once

#include <nds.h>
#include <string>
#include <vector>
#include <functional>

// -----------------------------------------------------------------------
// Build metadata (set by Makefile via -D flags, with fallbacks)
// -----------------------------------------------------------------------
#ifndef SCRATCHDS_VERSION
#define SCRATCHDS_VERSION "1.0.0"
#endif
#ifndef SCRATCHDS_BUILD_DATE
#define SCRATCHDS_BUILD_DATE __DATE__ " " __TIME__
#endif
#ifndef DEVKITARM_VERSION
#define DEVKITARM_VERSION "unknown"
#endif

// -----------------------------------------------------------------------
// Settings that the menu can modify
// -----------------------------------------------------------------------
struct ScratchDSSettings {
    int  targetFPS;          // 30 or 60
    bool stageOnTop;         // true = stage top / UI bottom; false = swapped
    int  stageScale;         // 0=Full(stretch), 1=Aspect, 2=Native(1:1)
    bool showFPSCounter;
    bool autoSaveSettings;   // persist to fat:/scratch/.settings
    char lastProjectPath[256];

    ScratchDSSettings() :
        targetFPS(60),
        stageOnTop(true),
        stageScale(1),
        showFPSCounter(false),
        autoSaveSettings(true)
    {
        lastProjectPath[0] = '\0';
    }

    // Persist to / load from SD card
    bool save(const char* path = "fat:/scratch/.settings");
    bool load(const char* path = "fat:/scratch/.settings");
};

// -----------------------------------------------------------------------
// File entry for the Load browser
// -----------------------------------------------------------------------
struct FileEntry {
    char name[256];
    bool isDir;
};

// -----------------------------------------------------------------------
// Overlay menu page enum
// -----------------------------------------------------------------------
enum class MenuPage {
    MAIN,
    INFO,
    SETTINGS,
    LOAD,
    COMPILE,
    CONFIRM_RESET,
    CONFIRM_LOAD,
};

// -----------------------------------------------------------------------
// OverlayMenu — manages the pause overlay
// -----------------------------------------------------------------------
class OverlayMenu {
public:
    static OverlayMenu& getInstance() {
        static OverlayMenu inst;
        return inst;
    }

    // Call once after hardware init
    void init(ScratchDSSettings& settings);

    // Call every frame — handles open/close combo, renders menu if open
    // Returns true if the Scratch VM should be paused this frame
    bool update(float dt);

    // Open / close programmatically
    void open();
    void close();
    bool isOpen() const { return open_; }

    // Called when settings are applied (callback to main)
    using ApplyCallback = std::function<void(const ScratchDSSettings&)>;
    using LoadCallback  = std::function<void(const char* path)>;
    void setApplyCallback(ApplyCallback cb) { onApply_ = cb; }
    void setLoadCallback(LoadCallback  cb)  { onLoad_  = cb; }

    // Access current settings (read-only from outside)
    const ScratchDSSettings& getSettings() const { return *settings_; }

private:
    OverlayMenu() : open_(false), settings_(nullptr) {}

    // Rendering
    void render();
    void renderMain();
    void renderInfo();
    void renderSettings();
    void renderLoad();
    void renderCompile();
    void renderConfirmReset();
    void renderConfirmLoad();
    void renderHeader(const char* title);
    void renderFooter();
    void renderCursor(int row);

    // Input handling
    void handleInput();
    void handleMainInput();
    void handleInfoInput();
    void handleSettingsInput();
    void handleLoadInput();
    void handleConfirmInput(bool& confirmed);

    // File browser
    void scanDirectory(const char* path);
    void navigateInto(int idx);
    void navigateUp();

    // Settings helpers
    void cycleFPS();
    void cycleScreen();
    void cycleScale();
    void applySettings();
    void startCompile();
    void tickCompile(float dt);

    // Drawing primitives (NDS console + BG tile tricks)
    void consolePrintf(int x, int y, const char* fmt, ...);
    void consoleClear();
    void drawBox(int x, int y, int w, int h, bool selected);
    void drawProgressBar(int x, int y, int w, int percent, const char* label);
    void drawScrollList(int x, int y, int h,
                        const std::vector<std::string>& items,
                        int selected, int scrollOffset);

    // State
    bool            open_;
    MenuPage        page_;
    int             cursor_;          // selected row on current page
    int             scrollOff_;       // scroll offset for lists

    ScratchDSSettings* settings_;
    ScratchDSSettings  pending_;      // edits in progress

    // Load browser state
    std::vector<FileEntry> dirEntries_;
    char currentDir_[256];
    char selectedPath_[256];        // full path of selected .sb3

    // Compile state
    bool   compiling_;
    float  compileTimer_;
    int    compileProgress_;
    char   compileStatus_[128];

    // Confirm dialog
    char   confirmMsg_[128];
    bool   confirmResult_;

    // Menu open combo hold timer
    float  comboHoldTimer_;
    static constexpr float COMBO_HOLD_REQUIRED = 0.4f;

    // NDS console handles (top screen used for menu when open)
    PrintConsole menuConsole_;
    bool         consoleInited_;

    // Callbacks
    ApplyCallback onApply_;
    LoadCallback  onLoad_;

    // NDS info helpers
    void   getNDSModel(char* out, int maxLen);
    int    getFreeRAM();
    void   getProjectName(char* out, int maxLen);
};

// -----------------------------------------------------------------------
// Simple on-screen D-pad character picker (for manual path entry)
// -----------------------------------------------------------------------
class DPadTextInput {
public:
    DPadTextInput();
    void reset(const char* prompt, const char* initial = "");
    bool update();          // returns true when ENTER pressed
    const char* getText() const { return buf_; }
    void render(int y);     // render at console row y

private:
    char   buf_[256];
    int    len_;
    int    charSel_;        // index into char table
    char   prompt_[64];

    static const char CHARS[];
    static const int  NUM_CHARS;
};
