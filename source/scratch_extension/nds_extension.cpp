// =============================================================================
// nds_extension.cpp — Fixed hat-block firing (edge detection, not level)
//
// THE BUG THAT CAUSED BLINKING:
//   shouldFireButtonHat() was calling isButtonDown() which reads
//   keysDownMask (just-pressed edge).  That is CORRECT.
//
//   BUT main.cpp was ALSO calling fireKeyPressed() for NDS buttons via
//   vm.fireKeyPressed() every frame as long as keysDownMask was set,
//   which is fine — HOWEVER the real problem was that NDSExtension::update()
//   was being called AFTER InputHandler::update() but in some code paths
//   scanKeys() was being called a second time (in the overlay menu combo
//   detection and in selectProject()), resetting the edge state.
//
//   Fix: NDSExtension never calls scanKeys().  It only reads from
//   InputHandler's cached masks.  The overlay menu combo detection
//   also reads from InputHandler (not its own scanKeys call).
// =============================================================================
#include "nds_extension.h"
#include <nds.h>
#include <string.h>
#include <stdlib.h>

namespace NDSBlocks {
    const char* WHEN_BUTTON_PRESSED  = "nds_whenbuttonpressed";
    const char* WHEN_CLAP            = "nds_whenclap";
    const char* WHEN_TOUCHED         = "nds_whentouched";
    const char* WHEN_COMBO           = "nds_whencombo";
    const char* BUTTON_PRESSED       = "nds_buttonpressed";
    const char* BUTTON_HELD          = "nds_buttonheld";
    const char* BUTTON_RELEASED      = "nds_buttonreleased";
    const char* COMBO_HELD           = "nds_combo_held";
    const char* TOUCH_PRESSED        = "nds_touchpressed";
    const char* CLAP_DETECTED        = "nds_clap_detected";
    const char* MIC_RECORDING        = "nds_mic_recording";
    const char* TOUCH_X              = "nds_touchx";
    const char* TOUCH_Y              = "nds_touchy";
    const char* TOUCH_DELTA_X        = "nds_touch_deltax";
    const char* TOUCH_DELTA_Y        = "nds_touch_deltay";
    const char* MICROPHONE_LOUDNESS  = "nds_microphone_loudness";
    const char* BATTERY_LEVEL        = "nds_battery_level";
    const char* SET_TOP_BACKLIGHT    = "nds_backlight_top";
    const char* SET_BOTTOM_BACKLIGHT = "nds_backlight_bottom";
    const char* RUMBLE               = "nds_rumble";
    const char* SET_VIBRATION        = "nds_setvibration";

    const char* const BUTTONS[] = {
        "A","B","X","Y","L","R","start","select",
        "up","down","left","right", nullptr
    };
    const char* const COMBOS[] = {
        "L+R","A+B","L+R+B","L+A","R+A",
        "up+A","down+A","left+A","right+A", nullptr
    };
}

// -----------------------------------------------------------------------
// Init
// -----------------------------------------------------------------------
void NDSExtension::init() {
    rumbleActive = false;
    rumbleTimer  = 0.0f;

    combos.registerCombo("L+R",     KEY_L | KEY_R);
    combos.registerCombo("A+B",     KEY_A | KEY_B);
    combos.registerCombo("L+R+B",   KEY_L | KEY_R | KEY_B);
    combos.registerCombo("L+A",     KEY_L | KEY_A);
    combos.registerCombo("R+A",     KEY_R | KEY_A);
    combos.registerCombo("up+A",    KEY_UP    | KEY_A);
    combos.registerCombo("down+A",  KEY_DOWN  | KEY_A);
    combos.registerCombo("left+A",  KEY_LEFT  | KEY_A);
    combos.registerCombo("right+A", KEY_RIGHT | KEY_A);
}

// -----------------------------------------------------------------------
// Update — reads ONLY from InputHandler's cached masks (no scanKeys here)
// -----------------------------------------------------------------------
void NDSExtension::update(float dt) {
    InputHandler& input = InputHandler::getInstance();

    // keysHeld() would call the raw libnds function; we use our cached
    // version instead so we don't disturb the edge-detection state.
    combos.update(input.getKeysHeld());

    bool touching = input.isTouching();
    touch.update(touching, input.getTouchX(), input.getTouchY(), dt);

    int loudness = input.isMicActive() ? input.getMicLoudness() : 0;
    clap.update(loudness, dt);

    updateRumble(dt);
}

// -----------------------------------------------------------------------
// Button queries
// -----------------------------------------------------------------------
// IMPORTANT: isButtonDown  → just-pressed EDGE   → use for hat blocks
//            isButtonHeld  → currently held       → use for motion/reporters
//            isButtonReleased → just-released     → use for release reporters
bool NDSExtension::isButtonPressed(const std::string& btn) const {
    return InputHandler::getInstance().isButtonDown(btn);
}
bool NDSExtension::isButtonHeld(const std::string& btn) const {
    return InputHandler::getInstance().isButtonHeld(btn);
}
bool NDSExtension::isButtonReleased(const std::string& btn) const {
    return InputHandler::getInstance().isButtonReleased(btn);
}

// -----------------------------------------------------------------------
// Combo queries
// -----------------------------------------------------------------------
bool NDSExtension::isComboHeld(const std::string& combo) const {
    return combos.isHeld(combo);
}
bool NDSExtension::isComboJustPressed(const std::string& combo) const {
    return combos.justTriggered(combo);
}
bool NDSExtension::menuComboJustPressed() const {
    return combos.justTriggered("L+R+B");
}

// -----------------------------------------------------------------------
// Touch queries
// -----------------------------------------------------------------------
int  NDSExtension::getTouchX() const      { return touch.lastX; }
int  NDSExtension::getTouchY() const      { return touch.lastY; }
bool NDSExtension::isTouching() const     { return touch.wasTouching; }
bool NDSExtension::isTapped() const       { return touch.tapped(); }
int  NDSExtension::getTouchDeltaX() const { return touch.deltaX; }
int  NDSExtension::getTouchDeltaY() const { return touch.deltaY; }

// -----------------------------------------------------------------------
// Microphone
// -----------------------------------------------------------------------
int  NDSExtension::getMicLoudness() const {
    return InputHandler::getInstance().getMicLoudness();
}
bool NDSExtension::isClapDetected() const { return clap.detected(); }
bool NDSExtension::isMicRecording() const {
    return InputHandler::getInstance().isMicActive();
}

// -----------------------------------------------------------------------
// Hat block predicates
// These are called from main.cpp's loop ONCE per frame.
// They use edge detection (just-pressed / just-triggered) so they only
// fire on the frame the event actually starts — not every frame while held.
// -----------------------------------------------------------------------
bool NDSExtension::shouldFireButtonHat(const std::string& btn) const {
    // Edge: just-pressed this frame.
    return InputHandler::getInstance().isButtonDown(btn);
}
bool NDSExtension::shouldFireClapHat() const  { return isClapDetected(); }
bool NDSExtension::shouldFireTouchHat() const { return isTapped(); }
bool NDSExtension::shouldFireComboHat(const std::string& combo) const {
    return isComboJustPressed(combo);
}

// -----------------------------------------------------------------------
// Backlight / rumble
// -----------------------------------------------------------------------
void NDSExtension::setTopBacklight(bool on) {
    if (on) powerOn(POWER_LCD | POWER_2D_A);
    else    powerOff(POWER_LCD);
}
void NDSExtension::setBottomBacklight(bool on) {
    REG_MASTER_BRIGHT_SUB = on ? 0 : (1 << 14) | 16;
}
void NDSExtension::setRumble(bool on) {
#ifdef ARM9
    *(vu16*)0x08000000 = on ? 0x0002 : 0x0000;
    rumbleActive = on;
#else
    (void)on;
#endif
}
void NDSExtension::pulseRumble(float durationSecs) {
    setRumble(true);
    rumbleTimer = durationSecs;
}
void NDSExtension::updateRumble(float dt) {
    if (rumbleTimer > 0.0f) {
        rumbleTimer -= dt;
        if (rumbleTimer <= 0.0f) {
            rumbleTimer = 0.0f;
            setRumble(false);
        }
    }
}
