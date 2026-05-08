// =============================================================================
// nds_extension.h — ScratchDS NDS Hardware Extension Plugin
// Provides custom Scratch blocks for NDS-specific input, output, and sensors.
//
// Block categories added:
//   [NDS Input]      — Button pressed/held/released, D-pad, combo detection
//   [NDS Touch]      — Touchscreen position, drag, tap detection
//   [NDS Microphone] — Loudness, clap detection, recording state
//   [NDS Triggers]   — Hat blocks: "when button pressed", "when clap detected"
//   [NDS System]     — Backlight, rumble, battery level placeholder
//
// Usage:
//   Call NDSExtension::registerBlocks() once at startup.
//   The VM's opcodeFromStr() and executeBlock() dispatch to this module
//   via the NDS_* opcodes defined in project.h.
//
// Scratch Extension Block Definitions (for a companion .js editor plugin):
//   See nds_extension_blocks.js for the Scratch 3.0 editor integration.
// =============================================================================
#pragma once

#include "../core/project.h"
#include "../core/vm.h"
#include "../input/input_handler.h"
#include <string>
#include <vector>

// -----------------------------------------------------------------------
// Extended NDS-specific opcodes (supplement BlockOpcode in project.h)
// These are handled by NDSExtension::execute()
// -----------------------------------------------------------------------
// Triggers (hat blocks)
//   NDS_WHENBUTTONPRESSED      — fires when a specific button is pressed
//   NDS_WHENCLAP               — fires when mic loudness crosses threshold
//   NDS_WHENTOUCHED            — fires when touchscreen is tapped
//   NDS_WHENCOMBO              — fires when a button combo is held
// Boolean/reporter blocks
//   NDS_BUTTONPRESSED          — (already in project.h) instant press
//   NDS_BUTTONHELD             — held this frame
//   NDS_BUTTONRELEASED         — released this frame
//   NDS_COMBO_HELD             — all buttons in a combo held simultaneously
//   NDS_TOUCHX / NDS_TOUCHY   — (already in project.h)
//   NDS_TOUCHPRESSED           — (already in project.h)
//   NDS_TOUCH_DELTAX/Y         — swipe/drag delta per frame
//   NDS_MICROPHONE_LOUDNESS    — (already in project.h)
//   NDS_CLAP_DETECTED          — true if loudness spike crossed threshold
//   NDS_MIC_RECORDING          — whether mic is active
//   NDS_BATTERY_LEVEL          — 0-100 placeholder (NDS has no real API)
//   NDS_BACKLIGHT_TOP          — (already in project.h) set top backlight
//   NDS_BACKLIGHT_BOTTOM       — set bottom backlight
//   NDS_RUMBLE                 — (already in project.h) pulse rumble
//   NDS_SETVIBRATION           — set rumble on/off

// -----------------------------------------------------------------------
// Clap / loudness spike detector
// -----------------------------------------------------------------------
struct ClapDetector {
    static constexpr int   HISTORY_LEN       = 8;
    static constexpr int   CLAP_THRESHOLD     = 60;   // loudness units (0-100)
    static constexpr int   CLAP_QUIET_THRESH  = 20;   // must be quiet before a clap
    static constexpr float CLAP_COOLDOWN_SECS = 0.3f; // minimum gap between claps

    int   history[HISTORY_LEN];
    int   histIdx;
    bool  wasQuiet;
    float cooldownTimer;
    bool  clapThisFrame;

    ClapDetector() : histIdx(0), wasQuiet(true), cooldownTimer(0), clapThisFrame(false) {
        for (int i = 0; i < HISTORY_LEN; i++) history[i] = 0;
    }

    // Call each frame with current loudness and dt
    void update(int loudness, float dt) {
        clapThisFrame = false;
        history[histIdx % HISTORY_LEN] = loudness;
        histIdx++;

        if (cooldownTimer > 0) { cooldownTimer -= dt; return; }

        if (wasQuiet && loudness >= CLAP_THRESHOLD) {
            clapThisFrame = true;
            cooldownTimer = CLAP_COOLDOWN_SECS;
            wasQuiet = false;
        } else if (loudness <= CLAP_QUIET_THRESH) {
            wasQuiet = true;
        }
    }

    bool detected() const { return clapThisFrame; }
};

// -----------------------------------------------------------------------
// Button combo tracker
// -----------------------------------------------------------------------
struct ComboTracker {
    struct Combo {
        std::string name;     // e.g. "L+R", "A+B", "L+R+B"
        u32         mask;     // OR of KEY_ masks
        bool        heldNow;
        bool        justPressed;
    };

    std::vector<Combo> combos;
    u32 prevHeld;

    ComboTracker() : prevHeld(0) {}

    void registerCombo(const std::string& name, u32 mask) {
        combos.push_back({name, mask, false, false});
    }

    void update(u32 heldMask) {
        for (auto& c : combos) {
            bool wasHeld  = c.heldNow;
            c.heldNow     = (heldMask & c.mask) == c.mask;
            c.justPressed = (!wasHeld && c.heldNow);
        }
        prevHeld = heldMask;
    }

    bool isHeld(const std::string& name) const {
        for (auto& c : combos) if (c.name == name) return c.heldNow;
        return false;
    }

    bool justTriggered(const std::string& name) const {
        for (auto& c : combos) if (c.name == name) return c.justPressed;
        return false;
    }
};

// -----------------------------------------------------------------------
// Touch gesture tracker (tap, drag delta)
// -----------------------------------------------------------------------
struct TouchTracker {
    int  lastX, lastY;
    int  deltaX, deltaY;
    bool wasTouching;
    bool tapThisFrame;
    float tapTimer;

    static constexpr float TAP_MAX_DURATION = 0.25f; // seconds

    TouchTracker() : lastX(0), lastY(0), deltaX(0), deltaY(0),
                     wasTouching(false), tapThisFrame(false), tapTimer(0) {}

    void update(bool touching, int x, int y, float dt) {
        tapThisFrame = false;

        if (touching) {
            deltaX = wasTouching ? (x - lastX) : 0;
            deltaY = wasTouching ? (y - lastY) : 0;
            lastX = x; lastY = y;
            tapTimer += dt;
            wasTouching = true;
        } else {
            if (wasTouching && tapTimer <= TAP_MAX_DURATION
                && abs(lastX) < 10 && abs(lastY) < 10) {
                tapThisFrame = true;
            }
            deltaX = deltaY = 0;
            tapTimer = 0;
            wasTouching = false;
        }
    }

    bool tapped() const { return tapThisFrame; }
};

// -----------------------------------------------------------------------
// NDSExtension — singleton managing all NDS plugin state
// -----------------------------------------------------------------------
class NDSExtension {
public:
    static NDSExtension& getInstance() {
        static NDSExtension inst;
        return inst;
    }

    // Called once at startup after hardware init
    void init();

    // Called every frame before VM step
    void update(float dt);

    // Query methods (called by VM executeBlock for NDS_* opcodes)
    bool  isButtonPressed(const std::string& btn) const;
    bool  isButtonHeld(const std::string& btn) const;
    bool  isButtonReleased(const std::string& btn) const;
    bool  isComboHeld(const std::string& combo) const;
    bool  isComboJustPressed(const std::string& combo) const;

    int   getTouchX() const;
    int   getTouchY() const;
    bool  isTouching() const;
    bool  isTapped() const;
    int   getTouchDeltaX() const;
    int   getTouchDeltaY() const;

    int   getMicLoudness() const;
    bool  isClapDetected() const;
    bool  isMicRecording() const;

    // Hat block polling: called by VM to check if a hat should fire this frame
    bool  shouldFireButtonHat(const std::string& btn) const;
    bool  shouldFireClapHat() const;
    bool  shouldFireTouchHat() const;
    bool  shouldFireComboHat(const std::string& combo) const;

    // Backlight control
    void  setTopBacklight(bool on);
    void  setBottomBacklight(bool on);

    // Rumble (GBA slot-2 rumble pak)
    void  setRumble(bool on);
    void  pulseRumble(float durationSecs);

    // Check if the overlay menu combo (L+R+B) just triggered
    bool  menuComboJustPressed() const;

private:
    NDSExtension() {}

    ClapDetector  clap;
    ComboTracker  combos;
    TouchTracker  touch;

    bool  rumbleActive;
    float rumbleTimer;

    void  updateRumble(float dt);
};

// -----------------------------------------------------------------------
// Scratch block definitions for the companion JS editor extension.
// These strings are also used to validate opcode names at parse time.
// -----------------------------------------------------------------------
namespace NDSBlocks {
    // Hat blocks (triggers)
    static const char* WHEN_BUTTON_PRESSED    = "nds_whenbuttonpressed";
    static const char* WHEN_CLAP              = "nds_whenclap";
    static const char* WHEN_TOUCHED           = "nds_whentouched";
    static const char* WHEN_COMBO             = "nds_whencombo";

    // Boolean reporters
    static const char* BUTTON_PRESSED         = "nds_buttonpressed";
    static const char* BUTTON_HELD            = "nds_buttonheld";
    static const char* BUTTON_RELEASED        = "nds_buttonreleased";
    static const char* COMBO_HELD             = "nds_combo_held";
    static const char* TOUCH_PRESSED          = "nds_touchpressed";
    static const char* CLAP_DETECTED          = "nds_clap_detected";
    static const char* MIC_RECORDING          = "nds_mic_recording";

    // Numeric reporters
    static const char* TOUCH_X                = "nds_touchx";
    static const char* TOUCH_Y                = "nds_touchy";
    static const char* TOUCH_DELTA_X          = "nds_touch_deltax";
    static const char* TOUCH_DELTA_Y          = "nds_touch_deltay";
    static const char* MICROPHONE_LOUDNESS    = "nds_microphone_loudness";
    static const char* BATTERY_LEVEL          = "nds_battery_level";

    // Command blocks (actions)
    static const char* SET_TOP_BACKLIGHT      = "nds_backlight_top";
    static const char* SET_BOTTOM_BACKLIGHT   = "nds_backlight_bottom";
    static const char* RUMBLE                 = "nds_rumble";
    static const char* SET_VIBRATION          = "nds_setvibration";

    // Button name constants (field values)
    static const char* BUTTONS[] = {
        "A", "B", "X", "Y", "L", "R",
        "start", "select",
        "up", "down", "left", "right",
        nullptr
    };

    // Combo name constants
    static const char* COMBOS[] = {
        "L+R", "A+B", "L+R+B", "L+A", "R+A",
        "up+A", "down+A", "left+A", "right+A",
        nullptr
    };
}
