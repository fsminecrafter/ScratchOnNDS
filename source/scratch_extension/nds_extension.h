// =============================================================================
// nds_extension.h — ScratchDS NDS Hardware Extension Plugin
// =============================================================================
#pragma once

#include "../core/project.h"
#include "../core/vm.h"
#include "../input/input_handler.h"
#include <string>
#include <vector>

// -----------------------------------------------------------------------
// Clap / loudness spike detector
// -----------------------------------------------------------------------
struct ClapDetector {
    static constexpr int   HISTORY_LEN       = 8;
    static constexpr int   CLAP_THRESHOLD     = 60;
    static constexpr int   CLAP_QUIET_THRESH  = 20;
    static constexpr float CLAP_COOLDOWN_SECS = 0.3f;

    int   history[HISTORY_LEN];
    int   histIdx;
    bool  wasQuiet;
    float cooldownTimer;
    bool  clapThisFrame;

    ClapDetector() : histIdx(0), wasQuiet(true), cooldownTimer(0), clapThisFrame(false) {
        for (int i = 0; i < HISTORY_LEN; i++) history[i] = 0;
    }

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
        std::string name;
        u32         mask;
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

    static constexpr float TAP_MAX_DURATION = 0.25f;

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

    void init();
    void update(float dt);

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

    bool  shouldFireButtonHat(const std::string& btn) const;
    bool  shouldFireClapHat() const;
    bool  shouldFireTouchHat() const;
    bool  shouldFireComboHat(const std::string& combo) const;

    void  setTopBacklight(bool on);
    void  setBottomBacklight(bool on);

    void  setRumble(bool on);
    void  pulseRumble(float durationSecs);

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
// Scratch block opcode name constants for the NDS extension.
//
// Declared as extern const char* in the header and defined once in
// nds_extension.cpp.  This avoids "defined but not used" warnings that
// arise when static const char* members are defined inline in a header
// included by many translation units.
//
// Only BUTTONS[] and COMBOS[] are used at runtime (in main.cpp's loop);
// the individual string constants are provided for reference / JS plugin.
// -----------------------------------------------------------------------
namespace NDSBlocks {
    // Hat blocks (triggers)
    extern const char* WHEN_BUTTON_PRESSED;
    extern const char* WHEN_CLAP;
    extern const char* WHEN_TOUCHED;
    extern const char* WHEN_COMBO;

    // Boolean reporters
    extern const char* BUTTON_PRESSED;
    extern const char* BUTTON_HELD;
    extern const char* BUTTON_RELEASED;
    extern const char* COMBO_HELD;
    extern const char* TOUCH_PRESSED;
    extern const char* CLAP_DETECTED;
    extern const char* MIC_RECORDING;

    // Numeric reporters
    extern const char* TOUCH_X;
    extern const char* TOUCH_Y;
    extern const char* TOUCH_DELTA_X;
    extern const char* TOUCH_DELTA_Y;
    extern const char* MICROPHONE_LOUDNESS;
    extern const char* BATTERY_LEVEL;

    // Command blocks
    extern const char* SET_TOP_BACKLIGHT;
    extern const char* SET_BOTTOM_BACKLIGHT;
    extern const char* RUMBLE;
    extern const char* SET_VIBRATION;

    // Null-terminated button/combo name arrays used by main.cpp's hat loop
    extern const char* const BUTTONS[];
    extern const char* const COMBOS[];
} // namespace NDSBlocks
