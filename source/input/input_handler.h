// =============================================================================
// input_handler.h — Fixed: proper edge detection for hat blocks
// =============================================================================
#pragma once
#include <nds.h>
#include <string>

#define MIC_SAMPLE_RATE  8000
#define MIC_BUFFER_SIZE  512

class InputHandler {
public:
    static InputHandler& getInstance() {
        static InputHandler inst;
        return inst;
    }

    void init();
    void update();  // call ONCE per frame — owns the single scanKeys() call

    // ---- Scratch key sensing ----
    bool isKeyDown(const std::string& keyName);   // just pressed this frame
    bool isKeyHeld(const std::string& keyName);   // held (includes first frame)
    bool isKeyUp(const std::string& keyName);     // released this frame

    // ---- NDS extension button queries ----
    bool isButtonDown(const std::string& btn);     // just pressed (edge)
    bool isButtonHeld(const std::string& btn);     // held
    bool isButtonReleased(const std::string& btn); // just released (edge)

    // ---- Raw cached masks (read-only, set by update()) ----
    u32 getKeysDown() const { return keysDownMask; }  // just-pressed edges
    u32 getKeysHeld() const { return keysHeldMask; }  // all currently held
    u32 getKeysUp()   const { return keysUpMask;   }  // just-released edges

    // ---- Touchscreen ----
    bool isTouching() const { return touching; }
    int  getTouchX() const;
    int  getTouchY() const;
    int  getTouchRawX() const { return touching ? touchPos.px : -1; }
    int  getTouchRawY() const { return touching ? touchPos.py : -1; }

    // ---- Microphone ----
    int  getMicLoudness();
    bool isMicActive() const { return micActive; }
    void startMicRecording();
    void stopMicRecording();

    std::string keyMaskToScratchName(u32 mask);

private:
    InputHandler() {}

    // keysDownMask  = bits SET this frame that were CLEAR last frame  (just-pressed)
    // keysHeldMask  = bits SET this frame regardless of last frame    (held)
    // keysUpMask    = bits CLEAR this frame that were SET  last frame (just-released)
    u32 keysDownMask;
    u32 keysHeldMask;
    u32 keysUpMask;

    bool touching;
    touchPosition touchPos;

    bool micActive;
    s16  micBuffer[MIC_BUFFER_SIZE];
    int  micLoudness;

    u32 nameToMask(const std::string& name) const;
    void updateMic();
};
