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

// SD card paths
#define PROJECTS_DIR    "fat:/scratch/"
#define EXTRACT_DIR     "fat:/scratch/.tmp/"
#define EXAMPLE_SB3     "fat:/scratch/example/example.sb3"
#define EXAMPLE_EXTRACT "fat:/scratch/.tmp/example/"
#define SETTINGS_PATH   "fat:/scratch/.settings"

// FPS tracking
static int   s_frameCount   = 0;
static float s_fpsTimer     = 0.0f;
static float s_currentFPS   = 60.0f;
static clock_t s_lastTick   = 0;

// Global settings (shared with overlay menu)
static ScratchDSSettings g_settings;

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
    // Init NDS hardware
    if (!initHardware()) {
        consoleDemoInit();
        printf("Hardware init failed!\n");
        while (true) swiWaitForVBlank();
    }

    showLoadingScreen("ScratchDS v" SCRATCHDS_VERSION, 0);
    showLoadingScreen("Initialising FAT...", 5);

    if (!fatInitDefault()) {
        consoleDemoInit();
        printf("FAT init failed!\nCheck R4 card & SD.\n");
        while (true) swiWaitForVBlank();
    }

    // Load persisted settings (if present)
    g_settings.load(SETTINGS_PATH);

    // Init subsystems
    showLoadingScreen("Starting subsystems...", 10);
    InputHandler::getInstance().init();
    NDSExtension::getInstance().init();
    AudioManager::getInstance().init();

    // Overlay menu
    OverlayMenu& menu = OverlayMenu::getInstance();
    menu.init(g_settings);
    menu.setApplyCallback(onSettingsApplied);
    menu.setLoadCallback(onProjectLoad);

    // Ensure directories exist
    mkdir(PROJECTS_DIR, 0777);
    mkdir(EXTRACT_DIR, 0777);
    mkdir("fat:/scratch/example/", 0777);
    mkdir("fat:/scratch/out/", 0777);

    // Try to select a project
    char projectPath[256] = {0};
    showLoadingScreen("Searching for projects...", 15);

    bool hasProject = false;

    // Check if settings has a last-used project
    if (g_settings.lastProjectPath[0] != '\0') {
        FILE* test = fopen(g_settings.lastProjectPath, "rb");
        if (test) { fclose(test); strncpy(projectPath, g_settings.lastProjectPath, 255); hasProject = true; }
    }

    // Try the file selector
    if (!hasProject) {
        showLoadingScreen("Select a project...", 20);
        hasProject = selectProject(projectPath, 256);
    }

    // Fallback to example
    if (!hasProject) {
        FILE* ex = fopen(EXAMPLE_SB3, "rb");
        if (ex) { fclose(ex); strncpy(projectPath, EXAMPLE_SB3, 255); hasProject = true; }
    }

    // Still nothing — show a waiting screen
    if (!hasProject) {
        consoleDemoInit();
        printf("\n\n  ScratchDS\n\n");
        printf("  No .sb3 project found.\n\n");
        printf("  Place .sb3 files in:\n");
        printf("  fat:/scratch/\n\n");
        printf("  Hold L+R+B to open menu\n");
        printf("  and use Load to browse.\n");

        // Wait for L+R+B to open menu (with empty project)
        ScratchProject emptyProject;
        mainLoop(emptyProject);
        return 0;
    }

    // Load the project
    ScratchProject project;
    if (!loadProject(projectPath, project)) {
        consoleDemoInit();
        printf("Failed to load project:\n%.30s\n", projectPath);
        printf("\nHold L+R+B to open menu.\n");
        mainLoop(project);
        return 0;
    }

    strncpy(g_settings.lastProjectPath, projectPath, sizeof(g_settings.lastProjectPath) - 1);
    g_settings.save(SETTINGS_PATH);

    showLoadingScreen("Starting VM...", 90);
    ScratchVM& vm = ScratchVM::getInstance();
    vm.init(project);
    vm.greenFlag();

    showLoadingScreen("Running!", 100);
    swiWaitForVBlank();

    mainLoop(project);
    return 0;
}

// -----------------------------------------------------------------------
// Load a .sb3 into a ScratchProject (extract + parse + load assets)
// -----------------------------------------------------------------------
bool loadProject(const char* sb3Path, ScratchProject& project, bool silent) {
    if (!silent) showLoadingScreen("Extracting...", 30);

    // Determine extract dir (use hashed subdir to avoid conflicts)
    char extractDir[256];
    snprintf(extractDir, sizeof(extractDir), "%s", EXTRACT_DIR);

    ZipLoader loader;
    if (!loader.extract(sb3Path, extractDir)) {
        if (!silent) { consoleDemoInit(); printf("Extraction failed:\n%s\n", sb3Path); }
        return false;
    }

    if (!silent) showLoadingScreen("Parsing project.json...", 50);
    if (!project.load(extractDir)) {
        if (!silent) { consoleDemoInit(); printf("Invalid project.json\n"); }
        return false;
    }

    if (!silent) showLoadingScreen("Loading costumes...", 65);
    Renderer::getInstance().loadSprites(project);

    if (!silent) showLoadingScreen("Loading sounds...", 80);
    AudioManager::getInstance().loadSounds(project, extractDir);

    return true;
}

// -----------------------------------------------------------------------
// Hardware initialisation
// -----------------------------------------------------------------------
bool initHardware() {
    powerOn(POWER_ALL);

    videoSetMode(MODE_5_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    vramSetBankB(VRAM_B_MAIN_SPRITE);

    videoSetModeSub(MODE_0_2D);
    vramSetBankC(VRAM_C_SUB_BG);
    vramSetBankD(VRAM_D_SUB_SPRITE);

    oamInit(&oamMain, SpriteMapping_1D_32, false);
    oamInit(&oamSub,  SpriteMapping_1D_32, false);

    // Timer 0: frame timing / FPS measurement
    timerStart(0, ClockDivider_1024, TIMER_FREQ_1024(60), nullptr);

    // Seed RNG from hardware timer
    srand(timerElapsed(0));

    return true;
}

// -----------------------------------------------------------------------
// Project file selector (D-pad UI)
// -----------------------------------------------------------------------
bool selectProject(char* pathOut, int maxLen) {
    const int MAX_FILES = 64;
    static char filenames[MAX_FILES][256];
    const char* filenamesPtrs[MAX_FILES];
    int count = 0;

    DIR* dir = opendir(PROJECTS_DIR);
    if (!dir) dir = opendir("fat:/");
    if (!dir) return false;

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

    if (count == 0) return false;
    if (count == 1) {
        snprintf(pathOut, maxLen, "%s%s", PROJECTS_DIR, filenames[0]);
        return true;
    }

    int selected = 0;
    consoleDemoInit();

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
        // Hold L+R+B during selector to skip (menu will handle load)
        if ((keysHeld() & (KEY_L | KEY_R | KEY_B)) == (KEY_L | KEY_R | KEY_B)) return false;
    }
}

void drawFileSelector(const char** files, int count, int selected) {
    consoleClear();
    printf("\n  ScratchDS - Select Project\n");
    printf("  ==========================\n\n");
    for (int i = 0; i < count; i++)
        printf("  %s %.26s\n", (i == selected) ? ">" : " ", files[i]);
    printf("\n  [A] Load  [Up/Down] Select\n");
    printf("  [L+R+B] Skip to Menu\n");
}

// -----------------------------------------------------------------------
// Loading screen (sub/bottom screen console)
// -----------------------------------------------------------------------
void showLoadingScreen(const char* message, int progress) {
    consoleDemoInit();
    consoleClear();
    printf("\n\n\n");
    printf("   *** ScratchDS v" SCRATCHDS_VERSION " ***\n\n");
    printf("   %s\n\n", message);
    printf("   [");
    int filled = progress / 5;
    for (int i = 0; i < 20; i++) printf("%s", i < filled ? "#" : "-");
    printf("] %d%%\n", progress);
    printf("\n   Hold L+R+B for menu\n");
}

// -----------------------------------------------------------------------
// Settings applied callback — resets scene or reconfigures video
// -----------------------------------------------------------------------
static ScratchProject* g_currentProject = nullptr; // set in mainLoop

void onSettingsApplied(const ScratchDSSettings& settings) {
    // Reconfigure frame timing (timer 1 for 30fps mode)
    if (settings.targetFPS == 30) {
        timerStart(1, ClockDivider_1024, TIMER_FREQ_1024(30), nullptr);
    }

    // Screen layout swap
    if (!settings.stageOnTop) {
        // Move OAM/BG to sub screen, console to main
        // (Full swap requires re-routing VRAM banks — simplified here)
        videoSetMode(MODE_0_2D);       // main becomes text console
        videoSetModeSub(MODE_5_2D);    // sub becomes stage
        vramSetBankC(VRAM_C_SUB_BG_0x06200000);
        vramSetBankD(VRAM_D_SUB_SPRITE);
    } else {
        // Restore default layout
        videoSetMode(MODE_5_2D);
        vramSetBankA(VRAM_A_MAIN_BG);
        vramSetBankB(VRAM_B_MAIN_SPRITE);
        videoSetModeSub(MODE_0_2D);
        vramSetBankC(VRAM_C_SUB_BG);
        vramSetBankD(VRAM_D_SUB_SPRITE);
    }

    // Reset VM and reload project if one is loaded
    if (g_currentProject && g_currentProject->targets.empty() == false) {
        ScratchVM::getInstance().stopAll();
        ScratchVM::getInstance().init(*g_currentProject);
        ScratchVM::getInstance().greenFlag();
    }
}

void onProjectLoad(const char* path) {
    AudioManager::getInstance().stopAll();

    if (g_currentProject) {
        *g_currentProject = ScratchProject(); // reset
        showLoadingScreen("Loading new project...", 0);
        if (loadProject(path, *g_currentProject)) {
            strncpy(g_settings.lastProjectPath, path, sizeof(g_settings.lastProjectPath) - 1);
            g_settings.save(SETTINGS_PATH);
            ScratchVM::getInstance().init(*g_currentProject);
            ScratchVM::getInstance().greenFlag();
        }
    }
}

// -----------------------------------------------------------------------
// FPS overlay (drawn on sub-screen console bottom line)
// -----------------------------------------------------------------------
void drawFPSOverlay() {
    if (!g_settings.showFPSCounter) return;
    printf("\x1b[0;28H" "\x1b[33;1m" "%4.1f" "\x1b[0m", s_currentFPS);
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

    // Frame timing
    clock_t prevTime = clock();
    float   dt       = 1.0f / 60.0f;

    // Target frame duration in VBlanks
    int vblanksPerFrame = (g_settings.targetFPS >= 60) ? 1 : 2;

    while (true) {
        // 1. Measure dt
        clock_t now = clock();
        dt = (float)(now - prevTime) / CLOCKS_PER_SEC;
        if (dt <= 0.0f || dt > 0.1f) dt = 1.0f / (float)g_settings.targetFPS;
        prevTime = now;

        // 2. Update FPS counter
        s_fpsTimer += dt;
        s_frameCount++;
        if (s_fpsTimer >= 1.0f) {
            s_currentFPS  = (float)s_frameCount / s_fpsTimer;
            s_frameCount  = 0;
            s_fpsTimer    = 0.0f;
        }

        // 3. Read hardware inputs
        input.update();

        // 4. Update NDS extension (combos, touch gestures, clap)
        ext.update(dt);

        // 5. Update overlay menu — if it returns true, VM is paused
        bool paused = menu.update(dt);

        if (!paused && !project.targets.empty()) {
            // 6a. Fire hat blocks from NDS extension triggers
            //     Button hat blocks
            for (int b = 0; NDSBlocks::BUTTONS[b] != nullptr; b++) {
                if (ext.shouldFireButtonHat(NDSBlocks::BUTTONS[b])) {
                    vm.fireKeyPressed(NDSBlocks::BUTTONS[b]);
                }
            }
            //     Clap hat
            if (ext.shouldFireClapHat()) {
                vm.broadcast("__nds_clap__");
            }
            //     Touch tap hat
            if (ext.shouldFireTouchHat()) {
                vm.broadcast("__nds_touch__");
            }
            //     Combo hats
            for (int c = 0; NDSBlocks::COMBOS[c] != nullptr; c++) {
                if (ext.shouldFireComboHat(NDSBlocks::COMBOS[c])) {
                    vm.broadcast(std::string("__nds_combo_") + NDSBlocks::COMBOS[c] + "__");
                }
            }

            // 6b. Step the VM
            vm.step(dt);

            // 6c. Render stage to top screen
            rend.renderFrame(project);

            // 6d. Render UI (variable monitor, button hints) to bottom screen
            rend.renderUI(project, input);

            // 6e. FPS overlay
            drawFPSOverlay();
        }

        // 7. Flush OAM
        oamUpdate(&oamMain);
        oamUpdate(&oamSub);

        // 8. Tick audio streaming
        audio.update();

        // 9. VBlank sync
        for (int v = 0; v < vblanksPerFrame; v++) swiWaitForVBlank();
    }
}
