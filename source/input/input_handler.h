// =============================================================================
// input_handler.h / input_handler.cpp
// Handles all NDS inputs: buttons, D-pad, touchscreen, microphone
// Maps NDS keys to Scratch key names and NDS extension opcodes
// =============================================================================
#pragma once
#include <nds.h>
#include <string>

// Microphone sample rate
#define MIC_SAMPLE_RATE  8000
#define MIC_BUFFER_SIZE  512

class InputHandler {
public:
    static InputHandler& getInstance() {
        static InputHandler inst;
        return inst;
    }

    void init();
    void update();  // call once per frame

    // ---- Button queries (for Scratch "key pressed?" sensing) ----
    // keyName: "space", "up arrow", "down arrow", "left arrow", "right arrow",
    //          "a"..."z", "0"..."9", "any"
    // NDS keys additionally: "A", "B", "X", "Y", "L", "R", "start", "select"
    bool isKeyDown(const std::string& keyName);
    bool isKeyHeld(const std::string& keyName);
    bool isKeyUp(const std::string& keyName);   // released this frame

    // ---- NDS extension button queries (raw button name) ----
    bool isButtonDown(const std::string& btn);      // pressed this frame
    bool isButtonHeld(const std::string& btn);      // held
    bool isButtonReleased(const std::string& btn);  // released this frame

    // ---- Touchscreen ----
    bool isTouching();
    int  getTouchX();   // Scratch coords: -240 to 240
    int  getTouchY();   // Scratch coords: -160 to 160 (inverted)
    int  getTouchRawX(); // 0-255
    int  getTouchRawY(); // 0-191

    // ---- Microphone ----
    int  getMicLoudness();   // 0-100 (Scratch-compatible loudness %)
    bool isMicActive();
    void startMicRecording();
    void stopMicRecording();

    // ---- Scratch key name from NDS key mask ----
    std::string keyMaskToScratchName(u32 mask);

private:
    InputHandler() {}

    u32  keysDownMask;
    u32  keysHeldMask;
    u32  keysUpMask;

    bool touching;
    touchPosition touchPos;

    // Mic
    bool micActive;
    s16  micBuffer[MIC_BUFFER_SIZE];
    int  micLoudness;

    u32  nameToMask(const std::string& name);
    void updateMic();
};
