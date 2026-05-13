// =============================================================================
// audio_manager.h — corrected for real maxmod9 API
// =============================================================================
#pragma once
#include "../core/project.h"
#include <maxmod9.h>
#include <string>

#define SOUND_SIZE_STREAM_THRESHOLD (2 * 1024 * 1024)  // 2 MB
#define MAX_CHANNELS 8

class AudioManager {
public:
    AudioManager();
    static AudioManager& getInstance() {
        static AudioManager inst;
        return inst;
    }

    void init();
    void update();

    void loadSounds(ScratchProject& project, const char* extractDir);
    void playSound(ScratchSprite* sprite, const std::string& soundName);
    void stopAll();
    bool isPlaying();
    void setVolume(int vol);

    void playRamSound(ScratchSound& sound);
    void update();

private:

    static constexpr int MAX_ACTIVE_SOUNDS = 32;

    mm_sfxhand activeHandles[MAX_ACTIVE_SOUNDS];
    int activeHandleCount;

    bool loadWavToRam(ScratchSound& sound, const char* path);
    bool loadMp3ToRam(ScratchSound& sound, const char* path);
    void startStream(ScratchSound& sound);
    static mm_word streamCallback(mm_word length, mm_addr dest, mm_stream_formats format);

    bool initialized;
    mm_sfxhand activeHandles[MAX_CHANNELS];
    int        numActive;
    bool       streamActive;
    FILE*      streamFile;
    int        streamSampleRate;
    uint8_t*   streamBuffer;
    size_t     streamBufSize;
    int        masterVolume;
};
