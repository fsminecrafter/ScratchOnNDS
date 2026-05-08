// =============================================================================
// audio_manager.h / audio_manager.cpp
// Handles Scratch sound playback on NDS using maxmod
// - Small sounds (<2MB): decode to PCM in RAM, play with mm_effect
// - Large sounds (>=2MB): stream from SD card with mm_stream
// Supports WAV and (basic) MP3 via dr_mp3 single-header decoder
// =============================================================================
#pragma once
#include "../core/project.h"
#include <maxmod9.h>
#include <string>

#define SOUND_SIZE_STREAM_THRESHOLD (2 * 1024 * 1024)  // 2 MB
#define MAX_CHANNELS 8

class AudioManager {
public:
    static AudioManager& getInstance() {
        static AudioManager inst;
        return inst;
    }

    void init();
    void update();   // call each frame for streaming tick

    // Load all sounds for a project from SD card
    void loadSounds(ScratchProject& project, const char* extractDir);

    // Play a sound by name on a sprite's sound list
    void playSound(ScratchSprite* sprite, const std::string& soundName);

    // Stop all sounds
    void stopAll();

    // Check if any sound is currently playing (for "play until done")
    bool isPlaying();

    // Volume (0-100)
    void setVolume(int vol);

private:
    AudioManager() {}

    bool loadWavToRam(ScratchSound& sound, const char* path);
    bool loadMp3ToRam(ScratchSound& sound, const char* path);
    bool setupStream(ScratchSound& sound, const char* path);

    void playRamSound(ScratchSound& sound);
    void startStream(ScratchSound& sound);

    // maxmod sound bank (built at runtime from loaded sounds)
    mm_ds_system mmSys;
    bool initialized;

    // Active channel tracking
    mm_sfxhand activeHandles[MAX_CHANNELS];
    int        numActive;

    // Streaming state
    bool     streamActive;
    FILE*    streamFile;
    int      streamSampleRate;
    uint8_t* streamBuffer;
    size_t   streamBufSize;

    // Master volume
    int masterVolume;
};
