// =============================================================================
// input_handler.cpp — Fixed: single scanKeys(), correct edge detection
//
// NDS libnds key API:
//   scanKeys()   — latches current hardware state into internal buffers
//   keysDown()   — bits that are SET now but were CLEAR on the previous latch
//   keysHeld()   — bits that are SET now  (regardless of previous state)
//   keysUp()     — bits that are CLEAR now but were SET on the previous latch
//
// We call scanKeys() EXACTLY ONCE per frame inside update().
// Everyone else reads our cached masks.  If scanKeys() were called a second
// time in the same frame (e.g. from NDSExtension or the overlay menu) the
// "just-pressed" edge state would be consumed and lost, causing the
// "blinking" behaviour observed with hat blocks.
// =============================================================================
#include "input_handler.h"
#include <nds/arm9/sound.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

void InputHandler::init() {
    keysDownMask = keysHeldMask = keysUpMask = 0;
    touching     = false;
    micActive    = false;
    micLoudness  = 0;
    memset(micBuffer, 0, sizeof(micBuffer));
    memset(&touchPos,  0, sizeof(touchPos));
}

void InputHandler::update() {
    // ── THE ONLY PLACE scanKeys() IS CALLED ──────────────────────────
    scanKeys();

    // Cache the three masks immediately after latching so nothing else
    // needs to (or should) call the raw libnds functions.
    keysDownMask = keysDown();   // just-pressed edges
    keysHeldMask = keysHeld();   // currently held (superset of keysDown)
    keysUpMask   = keysUp();     // just-released edges

    // Touchscreen — read only when the TOUCH bit is held
    touching = (keysHeldMask & KEY_TOUCH) != 0;
    if (touching) touchRead(&touchPos);

    if (micActive) updateMic();
}

// -----------------------------------------------------------------------
// Coordinate conversion: NDS touch (0-255, 0-191) → Scratch (-240..240, -160..160)
// -----------------------------------------------------------------------
int InputHandler::getTouchX() const {
    if (!touching) return 0;
    return (int)((touchPos.px - 128) * (240.0 / 128.0));
}
int InputHandler::getTouchY() const {
    if (!touching) return 0;
    return (int)((96 - touchPos.py) * (160.0 / 96.0));
}

// -----------------------------------------------------------------------
// Button name → KEY_ mask
// -----------------------------------------------------------------------
u32 InputHandler::nameToMask(const std::string& name) const {
    if (name == "A")                              return KEY_A;
    if (name == "B")                              return KEY_B;
    if (name == "X")                              return KEY_X;
    if (name == "Y")                              return KEY_Y;
    if (name == "L")                              return KEY_L;
    if (name == "R")                              return KEY_R;
    if (name == "start")                          return KEY_START;
    if (name == "select")                         return KEY_SELECT;
    if (name == "up"    || name == "up arrow")    return KEY_UP;
    if (name == "down"  || name == "down arrow")  return KEY_DOWN;
    if (name == "left"  || name == "left arrow")  return KEY_LEFT;
    if (name == "right" || name == "right arrow") return KEY_RIGHT;
    if (name == "space")                          return KEY_START;
    if (name == "any")                            return 0xFFFFFFFFu;
    return 0;
}

// ---- Scratch key API ------------------------------------------------
bool InputHandler::isKeyDown(const std::string& name) {
    u32 mask = nameToMask(name);
    if (mask == 0xFFFFFFFFu) return keysDownMask != 0;
    return (keysDownMask & mask) != 0;
}
bool InputHandler::isKeyHeld(const std::string& name) {
    u32 mask = nameToMask(name);
    if (mask == 0xFFFFFFFFu) return keysHeldMask != 0;
    return (keysHeldMask & mask) != 0;
}
bool InputHandler::isKeyUp(const std::string& name) {
    u32 mask = nameToMask(name);
    if (mask == 0xFFFFFFFFu) return keysUpMask != 0;
    return (keysUpMask & mask) != 0;
}

// ---- NDS extension button API (same logic, different naming) ---------
// isButtonDown   → hat blocks, one-shot actions   (just-pressed EDGE)
// isButtonHeld   → continuous movement / reporters (held)
// isButtonReleased → released reporters             (just-released EDGE)
bool InputHandler::isButtonDown(const std::string& btn) {
    return (keysDownMask & nameToMask(btn)) != 0;
}
bool InputHandler::isButtonHeld(const std::string& btn) {
    return (keysHeldMask & nameToMask(btn)) != 0;
}
bool InputHandler::isButtonReleased(const std::string& btn) {
    return (keysUpMask & nameToMask(btn)) != 0;
}

std::string InputHandler::keyMaskToScratchName(u32 mask) {
    if (mask & KEY_A)      return "A";
    if (mask & KEY_B)      return "B";
    if (mask & KEY_X)      return "X";
    if (mask & KEY_Y)      return "Y";
    if (mask & KEY_L)      return "L";
    if (mask & KEY_R)      return "R";
    if (mask & KEY_UP)     return "up arrow";
    if (mask & KEY_DOWN)   return "down arrow";
    if (mask & KEY_LEFT)   return "left arrow";
    if (mask & KEY_RIGHT)  return "right arrow";
    if (mask & KEY_START)  return "start";
    if (mask & KEY_SELECT) return "select";
    return "";
}

// -----------------------------------------------------------------------
// Microphone
// -----------------------------------------------------------------------
void InputHandler::updateMic() {
    long long sum = 0;
    for (int i = 0; i < MIC_BUFFER_SIZE; i++)
        sum += (long long)micBuffer[i] * micBuffer[i];
    double rms = sqrt((double)sum / MIC_BUFFER_SIZE);
    micLoudness = (int)(rms / 327.67);
    if (micLoudness > 100) micLoudness = 100;
}

int InputHandler::getMicLoudness() {
    if (!micActive) startMicRecording();
    return micLoudness;
}

void InputHandler::startMicRecording() {
    if (!micActive) {
        soundMicRecord(micBuffer, MIC_BUFFER_SIZE * sizeof(s16),
                       MicFormat_12Bit, MIC_SAMPLE_RATE, nullptr);
        micActive = true;
    }
}
void InputHandler::stopMicRecording() {
    if (micActive) {
        soundMicOff();
        micActive    = false;
        micLoudness  = 0;
    }
}
