// =============================================================================
// input_handler.cpp
// =============================================================================
#include "input_handler.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

// -----------------------------------------------------------------------
// Init
// -----------------------------------------------------------------------
void InputHandler::init() {
    keysDownMask = keysHeldMask = keysUpMask = 0;
    touching = false;
    micActive = false;
    micLoudness = 0;
    memset(micBuffer, 0, sizeof(micBuffer));
    memset(&touchPos, 0, sizeof(touchPos));
}

// -----------------------------------------------------------------------
// Update — call at top of each frame
// -----------------------------------------------------------------------
void InputHandler::update() {
    scanKeys();
    keysDownMask = keysDown();
    keysHeldMask = keysHeld();
    keysUpMask   = keysUp();

    // Touchscreen
    touching = (keysHeld() & KEY_TOUCH) != 0;
    if (touching) {
        touchRead(&touchPos);
    }

    // Microphone
    if (micActive) updateMic();
}

// -----------------------------------------------------------------------
// Microphone
// -----------------------------------------------------------------------
void InputHandler::updateMic() {
    // Read mic via NDS hardware (uses timer-driven sampling)
    // micBuffer is filled by mic DMA/timer interrupt in real impl.
    // Here we use micReadData() from libnds:
    micReadData(micBuffer, MIC_BUFFER_SIZE, false);

    // Compute RMS loudness
    long long sum = 0;
    for (int i = 0; i < MIC_BUFFER_SIZE; i++) {
        sum += (long long)micBuffer[i] * micBuffer[i];
    }
    double rms = sqrt((double)sum / MIC_BUFFER_SIZE);
    // Scale to 0-100 (mic range ~0-32767)
    micLoudness = (int)(rms / 327.67);
    if (micLoudness > 100) micLoudness = 100;
}

void InputHandler::startMicRecording() {
    if (!micActive) {
        micSetAmp(MIC_AMP_ON, 119); // Enable amp, max gain
        micActive = true;
    }
}

void InputHandler::stopMicRecording() {
    if (micActive) {
        micSetAmp(MIC_AMP_OFF, 0);
        micActive = false;
        micLoudness = 0;
    }
}

int InputHandler::getMicLoudness() {
    if (!micActive) startMicRecording();
    return micLoudness;
}

bool InputHandler::isMicActive() { return micActive; }

// -----------------------------------------------------------------------
// Touch
// -----------------------------------------------------------------------
bool InputHandler::isTouching() { return touching; }

int InputHandler::getTouchRawX() { return touching ? touchPos.px : -1; }
int InputHandler::getTouchRawY() { return touching ? touchPos.py : -1; }

// Convert NDS touch coords (0-255, 0-191) to Scratch coords (-240..240, -160..160)
int InputHandler::getTouchX() {
    if (!touching) return 0;
    return (int)((touchPos.px - 128) * (240.0 / 128.0));
}
int InputHandler::getTouchY() {
    if (!touching) return 0;
    // NDS Y is top=0, bottom=191; Scratch Y is top=+, bottom=-
    return (int)((96 - touchPos.py) * (160.0 / 96.0));
}

// -----------------------------------------------------------------------
// Map button name string -> NDS KEY_ mask
// -----------------------------------------------------------------------
u32 InputHandler::nameToMask(const std::string& name) {
    if (name == "A" || name == "a")          return KEY_A;
    if (name == "B" || name == "b")          return KEY_B;
    if (name == "X" || name == "x")          return KEY_X;
    if (name == "Y" || name == "y")          return KEY_Y;
    if (name == "L" || name == "l")          return KEY_L;
    if (name == "R" || name == "r")          return KEY_R;
    if (name == "start")                     return KEY_START;
    if (name == "select")                    return KEY_SELECT;
    if (name == "up arrow"   || name == "up")    return KEY_UP;
    if (name == "down arrow" || name == "down")  return KEY_DOWN;
    if (name == "left arrow" || name == "left")  return KEY_LEFT;
    if (name == "right arrow"|| name == "right") return KEY_RIGHT;
    if (name == "space")                     return KEY_START; // map space -> start
    if (name == "any")                       return 0xFFFFFFFF;
    return 0;
}

// -----------------------------------------------------------------------
// Scratch key sensing
// -----------------------------------------------------------------------
bool InputHandler::isKeyDown(const std::string& name) {
    u32 mask = nameToMask(name);
    if (mask == 0xFFFFFFFF) return keysDownMask != 0;
    return (keysDownMask & mask) != 0;
}
bool InputHandler::isKeyHeld(const std::string& name) {
    u32 mask = nameToMask(name);
    if (mask == 0xFFFFFFFF) return keysHeldMask != 0;
    return (keysHeldMask & mask) != 0;
}
bool InputHandler::isKeyUp(const std::string& name) {
    u32 mask = nameToMask(name);
    if (mask == 0xFFFFFFFF) return keysUpMask != 0;
    return (keysUpMask & mask) != 0;
}

// -----------------------------------------------------------------------
// NDS extension button queries
// -----------------------------------------------------------------------
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
