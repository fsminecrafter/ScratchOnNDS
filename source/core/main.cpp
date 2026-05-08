// =============================================================================
// ScratchDS - Scratch 3.0 Runtime for Nintendo DS (R4 Card)
// main.cpp - Entry point, project loader, and main loop
// Requires: devkitARM, libnds, libfat, maxmod
// =============================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <time.h>
#include <nds.h>
#include <nds/arm9/console.h>
#include <fat.h>
#include "core/project.h"
#include "core/vm.h"
#include "graphics/renderer.h"
#include "audio/audio_manager.h"
#include "input/input_handler.h"
#include "core/zip_loader.h"
#include "scratch_extension/nds_extension.h"
#include "ui/overlay_menu.h"

// NDS hardware constants
#define TOP_SCREEN_W    256
#define TOP_SCREEN_H    192
#define BOTTOM_SCREEN_W 256
#define BOTTOM_SCREEN_H 192

// SD card paths — NDS is always booted from root of the card via R4.
// fatInitDefault() mounts the card as "fat:/".
#define PROJECTS_DIR    "fat:/scratch/"
#define EXTRACT_DIR     "fat:/scratch/.tmp/"
#define EXAMPLE_SB3       "fat:/scratch/example/example.sb3"
#define EXAMPLE_SB3_FLAT  "fat:/scratch/example.sb3"
#define EXAMPLE_EXTRACT   "fat:/scratch/.tmp/example/"
#define SETTINGS_PATH   "fat:/scratch/.settings"

// FPS tracking
static int   s_frameCount   = 0;
static float s_fpsTimer     = 0.0f;
static float s_currentFPS   = 60.0f;

// Global settings (shared with overlay menu)
static ScratchDSSettings g_settings;

// Sub-screen (bottom) console — initialised ONCE at the very start of main()
// before anything else, so every printf goes somewhere visible immediately.
static PrintConsole g_subConsole;

// -----------------------------------------------------------------------
// Forward declarations
// -----------------------------------------------------------------------
bool initHardware();
bool selectProject(char* pathOut, int maxLen);
bool loadProject(const char* sb3Path, ScratchProject& project, bool silent = false);
void showLoadingScreen(const char* message, int progress);
void drawFileSelector(const char** files, int count, int selected);
void mainLoop(ScratchProject& project);
void onSettingsApplied(const ScratchDSSettings& settings);
void onProjectLoad(const char* path);
void drawFPSOverlay();

// -----------------------------------------------------------------------
// Program entry
// -----------------------------------------------------------------------
int main() {
    // ── Step 1: Power on everything ────────────────────────────────────
    // Hard reset video state left over from R4 launcher
    powerOn(POWER_ALL);
    
    // Reset both video engines to known state
    videoSetMode(0);       // blank main screen
    videoSetModeSub(0);    // blank sub screen
    
    // Wait a frame for the reset to take effect
    swiWaitForVBlank();
    swiWaitForVBlank();

    // ── Step 2: Set video modes so consoles have somewhere to render ───
    // Top screen  (main engine A) → stage / sprites later
    videoSetMode(MODE_5_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    vramSetBankB(VRAM_B_MAIN_SPRITE);

    // Bottom screen (sub engine B) → text console permanently
    videoSetModeSub(MODE_0_2D);
    vramSetBankC(VRAM_C_SUB_BG);

    // ── Step 3: Bring up the sub-screen text console IMMEDIATELY ───────
    // This is the ONLY place we call consoleInit for the bottom screen.
    // All showLoadingScreen / printf output goes here.
    consoleInit(&g_subConsole,
                3,                  // BG layer 3 on sub engine
                BgType_Text4bpp,
                BgSize_T_256x256,
                31,                 // map base
                0,                  // tile base
                false,              // not main screen
                true);              // load default font
    consoleSelect(&g_subConsole);

    // Print immediately so we know the console works
    printf("\n\n");
    printf("  *** ScratchDS v" SCRATCHDS_VERSION " ***\n\n");
    printf("  Console OK\n");
    printf("  Initialising hardware...\n");

    // ── Step 4: Finish hardware init (OAM, timers, RNG) ────────────────
    oamInit(&oamMain, SpriteMapping_1D_32, false);
    oamInit(&oamSub,  SpriteMapping_1D_32, false);

    // Timer 0 for frame timing / FPS measurement
    timerStart(0, ClockDivider_1024, TIMER_FREQ_1024(60), nullptr);
    srand(timerElapsed(0) ^ 0xDEAD);

    printf("  Hardware OK\n");

    // ── Step 5: FAT / SD card ───────────────────────────────────────────
    printf("  Mounting SD card...\n");
    if (!fatInitDefault()) {
        printf("\n  [FAIL] FAT init failed!\n");
        printf("  Check R4 card and SD.\n");
        printf("  Press any button to hang.\n");
        while (true) swiWaitForVBlank();
    }
    printf("  SD card OK\n");

    // ── Step 6: Load persisted settings ────────────────────────────────
    g_settings.load(SETTINGS_PATH);

    // ── Step 7: Init subsystems ─────────────────────────────────────────
    printf("  Input...\n");
    InputHandler::getInstance().init();

    printf("  NDS extension...\n");
    NDSExtension::getInstance().init();

    printf("  Audio...\n");
    AudioManager::getInstance().init();

    // ── Step 8: Overlay menu ────────────────────────────────────────────
    OverlayMenu& menu = OverlayMenu::getInstance();
    menu.init(g_settings);
    menu.setApplyCallback(onSettingsApplied);
    menu.setLoadCallback(onProjectLoad);

    // ── Step 9: Ensure directories exist ───────────────────────────────
    mkdir(PROJECTS_DIR, 0777);
    mkdir(EXTRACT_DIR,  0777);
    mkdir("fat:/scratch/example/", 0777);
    mkdir("fat:/scratch/out/", 0777);

    // ── Step 10: Find a project ─────────────────────────────────────────
    char projectPath[256] = {0};
    bool hasProject = false;

    // Last-used project from settings?
    if (g_settings.lastProjectPath[0] != '\0') {
        printf("  Checking last project...\n");
        FILE* test = fopen(g_settings.lastProjectPath, "rb");
        if (test) {
            fclose(test);
            strncpy(projectPath, g_settings.lastProjectPath, 255);
            hasProject = true;
            printf("  Found: %.28s\n", projectPath);
        } else {
            printf("  Last project not found.\n");
            g_settings.lastProjectPath[0] = '\0';
        }
    }

    // File selector
    if (!hasProject) {
        printf("  Scanning fat:/scratch/ ...\n");
        swiWaitForVBlank(); // let the print render
        hasProject = selectProject(projectPath, 256);
    }

    // Fallback to example
    if (!hasProject) {
        const char* exPaths[] = { EXAMPLE_SB3, EXAMPLE_SB3_FLAT, nullptr };
        for (int i = 0; exPaths[i] && !hasProject; i++) {
            FILE* ex = fopen(exPaths[i], "rb");
            if (ex) {
                fclose(ex);
                strncpy(projectPath, exPaths[i], 255);
                hasProject = true;
            }
        }
    }

    // Still nothing
    if (!hasProject) {
        printf("\n  No .sb3 file found.\n\n");
        printf("  Place .sb3 files in:\n");
        printf("  fat:/scratch/\n\n");
        printf("  Hold L+R+B to open menu.\n");
        printf("  (or power off and add files)\n");
        ScratchProject emptyProject;
        mainLoop(emptyProject);
        return 0;
    }

    // ── Step 11: Load the project ───────────────────────────────────────
    printf("\n  Loading project...\n");
    printf("  %.28s\n", projectPath);
    swiWaitForVBlank();

    ScratchProject project;
    if (!loadProject(projectPath, project)) {
        printf("\n  [FAIL] Could not load project.\n");
        printf("  %.30s\n", projectPath);
        printf("\n  Hold L+R+B for menu.\n");
        mainLoop(project);
        return 0;
    }

    strncpy(g_settings.lastProjectPath, projectPath,
            sizeof(g_settings.lastProjectPath) - 1);
    g_settings.save(SETTINGS_PATH);

    // ── Step 12: Start VM ───────────────────────────────────────────────
    printf("  Starting VM...\n");
    ScratchVM& vm = ScratchVM::getInstance();
    vm.init(project);
    vm.greenFlag();

    printf("  Running! Green flag sent.\n");
    printf("  Hold L+R+B for menu.\n");
    swiWaitForVBlank();
    swiWaitForVBlank();
    swiWaitForVBlank();

    mainLoop(project);
    return 0;
}

// -----------------------------------------------------------------------
// Load a .sb3 into a ScratchProject (extract + parse + load assets)
// -----------------------------------------------------------------------
bool loadProject(const char* sb3Path, ScratchProject& project, bool silent) {
    consoleSelect(&g_subConsole);

    if (!silent) {
        printf("  Extracting ZIP...\n");
        swiWaitForVBlank();
    }

    char extractDir[256];
    snprintf(extractDir, sizeof(extractDir), "%s", EXTRACT_DIR);

    ZipLoader loader;
    if (!loader.extract(sb3Path, extractDir)) {
        if (!silent) printf("  [FAIL] Extraction failed.\n");
        return false;
    }

    if (!silent) {
        printf("  Parsing project.json...\n");
        swiWaitForVBlank();
    }
    if (!project.load(extractDir)) {
        if (!silent) printf("  [FAIL] Bad project.json\n");
        return false;
    }

    if (!silent) {
        printf("  Loading costumes...\n");
        swiWaitForVBlank();
    }
    Renderer::getInstance().loadSprites(project);

    if (!silent) {
        printf("  Loading sounds...\n");
        swiWaitForVBlank();
    }
    AudioManager::getInstance().loadSounds(project, extractDir);

    if (!silent) printf("  Load OK.\n");
    return true;
}

// -----------------------------------------------------------------------
// showLoadingScreen — uses the already-initialised g_subConsole.
// Does NOT call consoleDemoInit() (which would fight with our console).
// -----------------------------------------------------------------------
void showLoadingScreen(const char* message, int progress) {
    consoleSelect(&g_subConsole);
    consoleClear();
    printf("\n\n\n");
    printf("  *** ScratchDS v" SCRATCHDS_VERSION " ***\n\n");
    printf("  %s\n\n", message);
    printf("  [");
    int filled = progress / 5;
    for (int i = 0; i < 20; i++) printf("%s", i < filled ? "#" : "-");
    printf("] %d%%\n", progress);
    printf("\n  Hold L+R+B for menu\n");
    swiWaitForVBlank(); // ensure the frame is displayed
}

// -----------------------------------------------------------------------
// Project file selector (D-pad UI on sub screen)
// -----------------------------------------------------------------------
bool selectProject(char* pathOut, int maxLen) {
    const int MAX_FILES = 64;
    static char filenames[MAX_FILES][256];
    const char* filenamesPtrs[MAX_FILES];
    int count = 0;

    // Scan for .sb3 files
    DIR* dir = opendir(PROJECTS_DIR);
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr && count < MAX_FILES) {
            size_t len = strlen(entry->d_name);
            if (len > 4 && strcmp(entry->d_name + len - 4, ".sb3") == 0) {
                strncpy(filenames[count], entry->d_name, 255);
                filenamesPtrs[count] = filenames[count];
                count++;
            }
        }
        closedir(dir);
    }

    if (count == 0) return false;

    if (count == 1) {
        snprintf(pathOut, maxLen, "%s%s", PROJECTS_DIR, filenames[0]);
        return true;
    }

    // Multiple files — show selector
    int selected = 0;
    while (true) {
        drawFileSelector(filenamesPtrs, count, selected);
        swiWaitForVBlank();
        scanKeys();
        u32 keys = keysDown();
        if (keys & KEY_UP)   selected = (selected - 1 + count) % count;
        if (keys & KEY_DOWN) selected = (selected + 1) % count;
        if (keys & KEY_A) {
            snprintf(pathOut, maxLen, "%s%s", PROJECTS_DIR, filenames[selected]);
            return true;
        }
        if ((keysHeld() & (KEY_L | KEY_R | KEY_B)) == (KEY_L | KEY_R | KEY_B))
            return false;
    }
}

void drawFileSelector(const char** files, int count, int selected) {
    consoleSelect(&g_subConsole);
    consoleClear();
    printf("\n  ScratchDS - Select Project\n");
    printf("  ==========================\n\n");
    for (int i = 0; i < count && i < 16; i++)
        printf("  %s %.26s\n", (i == selected) ? ">" : " ", files[i]);
    printf("\n  [A]=Load  [Up/Down]=Select\n");
    printf("  [L+R+B]=Skip to Menu\n");
}

// -----------------------------------------------------------------------
// Settings applied callback
// -----------------------------------------------------------------------
static ScratchProject* g_currentProject = nullptr;

void onSettingsApplied(const ScratchDSSettings& settings) {
    if (settings.targetFPS == 30) {
        timerStart(1, ClockDivider_1024, TIMER_FREQ_1024(30), nullptr);
    }

    if (!settings.stageOnTop) {
        videoSetMode(MODE_0_2D);
        videoSetModeSub(MODE_5_2D);
        vramSetBankC(VRAM_C_SUB_BG_0x06200000);
        vramSetBankD(VRAM_D_SUB_SPRITE);
    } else {
        videoSetMode(MODE_5_2D);
        vramSetBankA(VRAM_A_MAIN_BG);
        vramSetBankB(VRAM_B_MAIN_SPRITE);
        videoSetModeSub(MODE_0_2D);
        vramSetBankC(VRAM_C_SUB_BG);
    }

    if (g_currentProject && !g_currentProject->targets.empty()) {
        ScratchVM::getInstance().stopAll();
        ScratchVM::getInstance().init(*g_currentProject);
        ScratchVM::getInstance().greenFlag();
    }
}

void onProjectLoad(const char* path) {
    AudioManager::getInstance().stopAll();
    if (g_currentProject) {
        *g_currentProject = ScratchProject();
        consoleSelect(&g_subConsole);
        consoleClear();
        printf("  Loading: %.28s\n", path);
        if (loadProject(path, *g_currentProject)) {
            strncpy(g_settings.lastProjectPath, path,
                    sizeof(g_settings.lastProjectPath) - 1);
            g_settings.save(SETTINGS_PATH);
            ScratchVM::getInstance().init(*g_currentProject);
            ScratchVM::getInstance().greenFlag();
        }
    }
}

// -----------------------------------------------------------------------
// FPS overlay on bottom screen
// -----------------------------------------------------------------------
void drawFPSOverlay() {
    if (!g_settings.showFPSCounter) return;
    consoleSelect(&g_subConsole);
    printf("\x1b[0;28H\x1b[33;1m%4.1f\x1b[0m", s_currentFPS);
}

// -----------------------------------------------------------------------
// Main game loop
// -----------------------------------------------------------------------
void mainLoop(ScratchProject& project) {
    g_currentProject = &project;

    ScratchVM&    vm    = ScratchVM::getInstance();
    Renderer&     rend  = Renderer::getInstance();
    AudioManager& audio = AudioManager::getInstance();
    InputHandler& input = InputHandler::getInstance();
    NDSExtension& ext   = NDSExtension::getInstance();
    OverlayMenu&  menu  = OverlayMenu::getInstance();

    // Init the top-screen renderer (does NOT touch the sub console)
    rend.init();

    clock_t prevTime = clock();
    float   dt       = 1.0f / 60.0f;
    int vblanksPerFrame = (g_settings.targetFPS >= 60) ? 1 : 2;

    while (true) {
        clock_t now = clock();
        dt = (float)(now - prevTime) / CLOCKS_PER_SEC;
        if (dt <= 0.0f || dt > 0.1f) dt = 1.0f / (float)g_settings.targetFPS;
        prevTime = now;

        s_fpsTimer += dt;
        s_frameCount++;
        if (s_fpsTimer >= 1.0f) {
            s_currentFPS  = (float)s_frameCount / s_fpsTimer;
            s_frameCount  = 0;
            s_fpsTimer    = 0.0f;
        }

        input.update();
        ext.update(dt);

        bool paused = menu.update(dt);

        if (!paused && !project.targets.empty()) {
            // Fire NDS hat blocks
            for (int b = 0; NDSBlocks::BUTTONS[b] != nullptr; b++) {
                if (ext.shouldFireButtonHat(NDSBlocks::BUTTONS[b]))
                    vm.fireKeyPressed(NDSBlocks::BUTTONS[b]);
            }
            if (ext.shouldFireClapHat())  vm.broadcast("__nds_clap__");
            if (ext.shouldFireTouchHat()) vm.broadcast("__nds_touch__");
            for (int c = 0; NDSBlocks::COMBOS[c] != nullptr; c++) {
                if (ext.shouldFireComboHat(NDSBlocks::COMBOS[c]))
                    vm.broadcast(std::string("__nds_combo_") + NDSBlocks::COMBOS[c] + "__");
            }

            vm.step(dt);
            rend.renderFrame(project);

            // UI on bottom screen — re-select our console first
            consoleSelect(&g_subConsole);
            rend.renderUI(project, input);
            drawFPSOverlay();
        }

        oamUpdate(&oamMain);
        oamUpdate(&oamSub);
        audio.update();

        for (int v = 0; v < vblanksPerFrame; v++) swiWaitForVBlank();
    }
}
