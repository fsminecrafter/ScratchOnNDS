// =============================================================================
// ScratchDS - Scratch 3.0 Runtime for Nintendo DS (R4 Card)
// main.cpp - Entry point, project loader, and main loop
// Requires: devkitARM, libnds, libfat, maxmod
// =============================================================================

#include <nds.h>
#include <fat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#include "core/project.h"
#include "core/vm.h"
#include "graphics/renderer.h"
#include "audio/audio_manager.h"
#include "input/input_handler.h"
#include "core/zip_loader.h"

// NDS hardware constants
#define TOP_SCREEN_W    256
#define TOP_SCREEN_H    192
#define BOTTOM_SCREEN_W 256
#define BOTTOM_SCREEN_H 192

// R4 SD card project path
#define PROJECTS_DIR    "fat:/scratch/"
#define EXTRACT_DIR     "fat:/scratch/.tmp/"

// -----------------------------------------------------------------------
// Forward declarations
// -----------------------------------------------------------------------
bool initHardware();
bool selectProject(char* pathOut, int maxLen);
void showLoadingScreen(const char* message, int progress);
void drawFileSelector(const char** files, int count, int selected);
void mainLoop(ScratchProject& project);

// -----------------------------------------------------------------------
// Program entry
// -----------------------------------------------------------------------
int main() {
    // Init NDS hardware
    if (!initHardware()) {
        consoleDemoInit();
        iprintf("Hardware init failed!\n");
        while (true) swiWaitForVBlank();
    }

    // Show splash
    showLoadingScreen("ScratchDS v1.0", 0);
    showLoadingScreen("Initializing FAT...", 5);

    // Init FAT (SD card via R4)
    if (!fatInitDefault()) {
        consoleDemoInit();
        iprintf("FAT init failed!\nCheck R4 card & SD.\n");
        while (true) swiWaitForVBlank();
    }

    // Init audio subsystem
    AudioManager::getInstance().init();

    // Make sure extraction dir exists
    mkdir(PROJECTS_DIR, 0777);
    mkdir(EXTRACT_DIR, 0777);

    // Let user select a .sb3 file
    char projectPath[256] = {0};
    showLoadingScreen("Select a project...", 10);
    if (!selectProject(projectPath, 256)) {
        consoleDemoInit();
        iprintf("No project selected.\n");
        while (true) swiWaitForVBlank();
    }

    // Extract .sb3 (which is a ZIP)
    showLoadingScreen("Extracting project...", 20);
    ZipLoader loader;
    if (!loader.extract(projectPath, EXTRACT_DIR)) {
        consoleDemoInit();
        iprintf("Failed to extract:\n%s\n", projectPath);
        while (true) swiWaitForVBlank();
    }

    // Parse project.json
    showLoadingScreen("Loading project.json...", 40);
    ScratchProject project;
    if (!project.load(EXTRACT_DIR)) {
        consoleDemoInit();
        iprintf("Invalid project.json\n");
        while (true) swiWaitForVBlank();
    }

    // Load assets into VRAM/RAM
    showLoadingScreen("Loading sprites...", 55);
    Renderer::getInstance().loadSprites(project);

    showLoadingScreen("Loading sounds...", 70);
    AudioManager::getInstance().loadSounds(project, EXTRACT_DIR);

    showLoadingScreen("Starting VM...", 85);
    ScratchVM vm(project);
    vm.greenFlag(); // Fire green flag event

    showLoadingScreen("Running!", 100);
    swiWaitForVBlank();

    // Hand off to main loop
    mainLoop(project);

    return 0;
}

// -----------------------------------------------------------------------
// Hardware initialisation
// -----------------------------------------------------------------------
bool initHardware() {
    // Power on both screens
    powerOn(POWER_ALL);

    // Setup video modes:
    // Main (top): Mode 5 2D with extended rotation backgrounds for stage
    videoSetMode(MODE_5_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    vramSetBankB(VRAM_B_MAIN_SPRITE);

    // Sub (bottom): Mode 0 for UI / touchscreen
    videoSetModeSub(MODE_0_2D);
    vramSetBankC(VRAM_C_SUB_BG);
    vramSetBankD(VRAM_D_SUB_SPRITE);

    // Enable sprites on both screens
    oamInit(&oamMain, SpriteMapping_1D_32, false);
    oamInit(&oamSub,  SpriteMapping_1D_32, false);

    // Enable touchscreen
    touchInit();

    // Timers: use timer 0 for frame timing
    timerStart(0, ClockDivider_1024, TIMER_FREQ_1024(60), nullptr);

    return true;
}

// -----------------------------------------------------------------------
// Scan PROJECTS_DIR for .sb3 files and let user pick with D-pad
// -----------------------------------------------------------------------
bool selectProject(char* pathOut, int maxLen) {
    const int MAX_FILES = 64;
    static char filenames[MAX_FILES][256];
    const char* filenamesPtrs[MAX_FILES];
    int count = 0;

    DIR* dir = opendir(PROJECTS_DIR);
    if (!dir) {
        // No projects dir - use first .sb3 found in root
        dir = opendir("fat:/");
        if (!dir) return false;
    }

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

    // Interactive selector
    int selected = 0;
    consoleDemoInit();

    while (true) {
        drawFileSelector(filenamesPtrs, count, selected);
        swiWaitForVBlank();

        scanKeys();
        u32 keys = keysDown();

        if (keys & KEY_UP)    selected = (selected - 1 + count) % count;
        if (keys & KEY_DOWN)  selected = (selected + 1) % count;
        if (keys & KEY_A) {
            snprintf(pathOut, maxLen, "%s%s", PROJECTS_DIR, filenames[selected]);
            return true;
        }
    }
}

void drawFileSelector(const char** files, int count, int selected) {
    consoleClear();
    iprintf("\n  ScratchDS - Select Project\n");
    iprintf("  ==========================\n\n");
    for (int i = 0; i < count; i++) {
        iprintf("  %s %s\n", (i == selected) ? ">" : " ", files[i]);
    }
    iprintf("\n  [A] Load  [Up/Down] Select\n");
}

// -----------------------------------------------------------------------
// Loading screen on top display
// -----------------------------------------------------------------------
void showLoadingScreen(const char* message, int progress) {
    // Draw simple loading bar on sub screen console
    consoleDemoInit();
    consoleClear();
    iprintf("\n\n\n");
    iprintf("   *** ScratchDS ***\n\n");
    iprintf("   %s\n\n", message);
    iprintf("   [");
    int filled = progress / 5; // 0-20 chars
    for (int i = 0; i < 20; i++) iprintf("%s", i < filled ? "#" : "-");
    iprintf("] %d%%\n", progress);
}

// -----------------------------------------------------------------------
// Main game loop - runs after project is loaded and green flag fired
// -----------------------------------------------------------------------
void mainLoop(ScratchProject& project) {
    ScratchVM& vm   = ScratchVM::getInstance();
    Renderer&  rend = Renderer::getInstance();
    AudioManager& audio = AudioManager::getInstance();
    InputHandler& input = InputHandler::getInstance();

    while (true) {
        // 1. Read all NDS inputs (buttons, touchscreen, mic)
        input.update();

        // 2. Step the VM (executes Scratch scripts for ~1 frame worth)
        vm.step(1.0 / 60.0);

        // 3. Render stage to top screen
        rend.renderFrame(project);

        // 4. Render UI (variable monitor, button hints) to bottom screen
        rend.renderUI(project, input);

        // 5. Flush OAM updates
        oamUpdate(&oamMain);
        oamUpdate(&oamSub);

        // 6. Tick audio streaming
        audio.update();

        // 7. VBlank sync for 60fps
        swiWaitForVBlank();
    }
}
