// =============================================================================
// overlay_menu.h — patched: added USAGE page with detailed memory/thread/battery
// =============================================================================
#pragma once

#include <nds.h>
#include <string>
#include <vector>
#include <functional>

// Forward declarations needed for gatherUsageStats
class ScratchVM;
struct ScratchProject;

// -----------------------------------------------------------------------
// Build metadata
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
// ScratchDSSettings
// -----------------------------------------------------------------------
struct ScratchDSSettings {
    int  targetFPS;
    bool stageOnTop;
    int  stageScale;
    bool showFPSCounter;
    bool autoSaveSettings;
    char lastProjectPath[256];

    ScratchDSSettings() :
        targetFPS(60), stageOnTop(true), stageScale(1),
        showFPSCounter(false), autoSaveSettings(true)
    { lastProjectPath[0] = '\0'; }

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
// UsageStats — gathered once when the USAGE page opens (not every frame)
// to avoid hammering malloc/free in the render loop.
// -----------------------------------------------------------------------
struct UsageStats {
    // ── Memory ──────────────────────────────────────────────────────────
    int totalRamBytes;        // NDS ARM9 main RAM = 4 MB
    int freeRamBytes;
    int usedBySpritesBytes;   // sum of gfx VRAM pointers × pixel counts
    int usedBySoundsBytes;    // sum of loaded PCM buffers
    int vramEstimateBytes;    // VRAM A+B+C+D combined 512 KB

    // Per-sprite memory breakdown (up to 16 entries)
    struct SpriteEntry {
        char name[24];
        int  costumesBytes;
        int  soundsBytes;
        int  numBlocks;
        int  numCostumes;
        int  numSounds;
        bool visible;
        bool isStage;
    };
    static constexpr int MAX_SPRITES = 16;
    SpriteEntry sprites[MAX_SPRITES];
    int         numSprites;

    // ── Threads ──────────────────────────────────────────────────────────
    struct ThreadEntry {
        char spriteName[20];
        char state[16];    // "RUNNING","WAIT_SEC","WAIT_SND","DONE"
        char blockId[12];  // truncated current block id
        int  stackDepth;
        int  stepsThisFrame;
    };
    static constexpr int MAX_THREADS = 64;
    ThreadEntry threads[MAX_THREADS];
    int         numThreads;

    // ── System ───────────────────────────────────────────────────────────
    int  batteryPercent;   // 0-100, or -1 if not available
    bool isCharging;
    char timeStr[12];      // "HH:MM:SS\0"
    char dateStr[12];      // "YYYY-MM-DD\0"
    int  fpsTenths;        // current FPS × 10 (avoids float in render)
    char ndsModel[24];

    // ── OAM / palette ────────────────────────────────────────────────────
    int palSlotsUsed;
    int oamSlotsUsed;

    // ── Derived ──────────────────────────────────────────────────────────
    int usedRamBytes() const { return totalRamBytes - freeRamBytes; }
};

// -----------------------------------------------------------------------
// Menu page enum
// -----------------------------------------------------------------------
enum class MenuPage {
    MAIN,
    INFO,
    SETTINGS,
    LOAD,
    COMPILE,
    CONFIRM_RESET,
    CONFIRM_LOAD,
    USAGE,          // ← new
};

// -----------------------------------------------------------------------
// OverlayMenu
// -----------------------------------------------------------------------
class OverlayMenu {
public:
    static OverlayMenu& getInstance() {
        static OverlayMenu inst;
        return inst;
    }

    void setConsole(PrintConsole* con) { externalConsole_ = con; }

    void init(ScratchDSSettings& settings);
    bool update(float dt);
    void open();
    void close();
    bool isOpen() const { return open_; }

    using ApplyCallback = std::function<void(const ScratchDSSettings&)>;
    using LoadCallback  = std::function<void(const char* path)>;
    void setApplyCallback(ApplyCallback cb) { onApply_ = cb; }
    void setLoadCallback(LoadCallback  cb)  { onLoad_  = cb; }

    // Called from main loop so USAGE page can access live VM/project data.
    // Safe to call every frame; data is only re-gathered when USAGE page opens.
    void setLiveData(ScratchProject* project, ScratchVM* vm,
                     float currentFps, int palSlotsUsed, int oamSlotsUsed) {
        liveProject_     = project;
        liveVm_          = vm;
        liveFps_         = currentFps;
        livePalSlots_    = palSlotsUsed;
        liveOamSlots_    = oamSlotsUsed;
    }

    const ScratchDSSettings& getSettings() const { return *settings_; }

private:
    OverlayMenu() : open_(false), settings_(nullptr),
                    cachedKeysDown_(0), cachedKeysHeld_(0),
                    liveProject_(nullptr), liveVm_(nullptr),
                    liveFps_(0), livePalSlots_(0), liveOamSlots_(0),
                    usageScrollOff_(0), usageStatsDirty_(true) {}

    // Rendering
    void render();
    void renderMain();
    void renderInfo();
    void renderSettings();
    void renderLoad();
    void renderCompile();
    void renderConfirmReset();
    void renderUsage();
    void renderHeader(const char* title);
    void renderFooter();

    // Input
    void handleInput();
    void handleMainInput();
    void handleInfoInput();
    void handleSettingsInput();
    void handleLoadInput();
    void handleConfirmInput(bool& confirmed);
    void handleUsageInput();

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

    // Usage stats helpers
    void gatherUsageStats();
    static int measureFreeRamBinary();

    // Drawing helpers
    void consoleClear();

    // NDS info helpers
    void getNDSModel(char* out, int maxLen);
    int  getFreeRAM();
    void getProjectName(char* out, int maxLen);

    // ── State ────────────────────────────────────────────────────────────
    bool            open_;
    MenuPage        page_;
    int             cursor_;
    int             scrollOff_;

    ScratchDSSettings* settings_;
    ScratchDSSettings  pending_;

    u32 cachedKeysDown_;
    u32 cachedKeysHeld_;

    // Load browser
    std::vector<FileEntry> dirEntries_;
    char currentDir_[256];
    char selectedPath_[256];

    // Compile
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

    // Console
    PrintConsole* externalConsole_;

    // Callbacks
    ApplyCallback onApply_;
    LoadCallback  onLoad_;

    // Live data pointers (set by main loop via setLiveData)
    ScratchProject* liveProject_;
    ScratchVM*      liveVm_;
    float           liveFps_;
    int             livePalSlots_;
    int             liveOamSlots_;

    // Usage page
    int        usageScrollOff_;
    bool       usageStatsDirty_;  // true when we need to re-gather stats
    UsageStats usageStats_;
};

// -----------------------------------------------------------------------
// DPadTextInput
// -----------------------------------------------------------------------
class DPadTextInput {
public:
    DPadTextInput();
    void reset(const char* prompt, const char* initial = "");
    bool update();
    const char* getText() const { return buf_; }
    void render(int y);

private:
    char   buf_[256];
    int    len_;
    int    charSel_;
    char   prompt_[64];

    static const char CHARS[];
    static const int  NUM_CHARS;
};
