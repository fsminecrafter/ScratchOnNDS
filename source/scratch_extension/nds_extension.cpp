// =============================================================================
// nds_extension.cpp — ScratchDS NDS Hardware Extension Plugin
// =============================================================================
#include "nds_extension.h"
#include <nds.h>
#include <string.h>
#include <stdlib.h>

// -----------------------------------------------------------------------
// Init — register all combos, start mic
// -----------------------------------------------------------------------
void NDSExtension::init() {
    rumbleActive = false;
    rumbleTimer  = 0.0f;

    // Register all supported button combos
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
// Update — call every frame before VM step
// -----------------------------------------------------------------------
void NDSExtension::update(float dt) {
    InputHandler& input = InputHandler::getInstance();

    // Update combo tracker with current held keys
    combos.update(keysHeld());

    // Update touch gesture tracker
    bool touching = input.isTouching();
    touch.update(touching,
                 input.getTouchX(),
                 input.getTouchY(),
                 dt);

    // Update clap detector with mic loudness
    int loudness = 0;
    if (input.isMicActive()) {
        loudness = input.getMicLoudness();
    }
    clap.update(loudness, dt);

    // Update rumble pulse timer
    updateRumble(dt);
}

// -----------------------------------------------------------------------
// Button queries — delegate to InputHandler
// -----------------------------------------------------------------------
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
int  NDSExtension::getTouchX() const       { return touch.lastX; }
int  NDSExtension::getTouchY() const       { return touch.lastY; }
bool NDSExtension::isTouching() const      { return touch.wasTouching; }
bool NDSExtension::isTapped() const        { return touch.tapped(); }
int  NDSExtension::getTouchDeltaX() const  { return touch.deltaX; }
int  NDSExtension::getTouchDeltaY() const  { return touch.deltaY; }

// -----------------------------------------------------------------------
// Microphone queries
// -----------------------------------------------------------------------
int  NDSExtension::getMicLoudness() const  {
    return InputHandler::getInstance().getMicLoudness();
}
bool NDSExtension::isClapDetected() const  { return clap.detected(); }
bool NDSExtension::isMicRecording() const  {
    return InputHandler::getInstance().isMicActive();
}

// -----------------------------------------------------------------------
// Hat block firing conditions
// -----------------------------------------------------------------------
bool NDSExtension::shouldFireButtonHat(const std::string& btn) const {
    return isButtonPressed(btn); // fires on press edge, not hold
}
bool NDSExtension::shouldFireClapHat() const  { return isClapDetected(); }
bool NDSExtension::shouldFireTouchHat() const { return isTapped(); }
bool NDSExtension::shouldFireComboHat(const std::string& combo) const {
    return isComboJustPressed(combo);
}

// -----------------------------------------------------------------------
// Backlight control (libnds API)
// -----------------------------------------------------------------------
void NDSExtension::setTopBacklight(bool on) {
    if (on) powerOn(POWER_LCD | POWER_2D_A);
    else    powerOff(POWER_LCD);
}
void NDSExtension::setBottomBacklight(bool on) {
    // The NDS sub-screen (bottom) is controlled via the same LCD power bit.
    // Individual backlight dimming requires BIOS calls not in libnds.
    // Best approximation: toggle the sub-BG master brightness.
    REG_MASTER_BRIGHT_SUB = on ? 0 : (1 << 14) | 16; // max dark
}

// -----------------------------------------------------------------------
// Rumble pak (GBA Slot-2 rumble device)
// Toggling GBA bus address 0x08000000 bit 3 controls most rumble paks.
// -----------------------------------------------------------------------
void NDSExtension::setRumble(bool on) {
#ifdef ARM9
    // Standard slot-2 rumble pak protocol
    *(vu16*)0x08000000 = on ? 0x0002 : 0x0000;
    rumbleActive = on;
#endif
}
void NDSExtension::pulseRumble(float durationSecs) {
    setRumble(true);
    rumbleTimer = durationSecs;
}
void NDSExtension::updateRumble(float dt) {
    if (rumbleTimer > 0) {
        rumbleTimer -= dt;
        if (rumbleTimer <= 0) {
            rumbleTimer = 0;
            setRumble(false);
        }
    }
}
