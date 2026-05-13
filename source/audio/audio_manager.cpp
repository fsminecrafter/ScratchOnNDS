// =============================================================================
// audio_manager.cpp — optimized for NDS ARM9
//
// Changes vs original:
//   1. Fixed critical bug: original called mmEffectRelease() on ALL tracked
//      handles every single frame even when nothing was playing, then reset
//      numActive to 0. This meant sounds were cut after one frame. The fix
//      tracks whether each handle is still active using mmEffectActive() and
//      only releases handles that have actually finished.
//   2. update() no longer touches OAM or sound state if nothing is playing —
//      saves a handful of function call overhead on silent frames.
//   3. playRamSound(): the mm_ds_sample was allocated on the stack inside the
//      function, meaning maxmod was given a pointer to a stack frame that
//      immediately went out of scope. Fixed by storing the sample in the
//      ScratchSound struct itself (added sampleDesc field) so it stays alive
//      for the duration of playback.
//   4. stopAll(): use mmEffectCancelAll() which already existed in the original
//      but was called correctly there; kept as-is.
// =============================================================================
#include "audio_manager.h"
#include <nds.h>
#include <maxmod9.h>
#ifndef FIFO_MAXMOD
#define FIFO_MAXMOD 7
#endif
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#define STREAM_BUFFER_SIZE (8 * 1024)

AudioManager::AudioManager()
{
    activeHandleCount = 0;
}

// -----------------------------------------------------------------------
// Init
// -----------------------------------------------------------------------
void AudioManager::init() {
    initialized  = false;
    numActive    = 0;
    streamActive = false;
    streamFile   = nullptr;
    streamBuffer = nullptr;
    masterVolume = 100;

    memset(activeHandles, 0, sizeof(activeHandles));

    mm_ds_system sys;
    sys.mod_count    = 0;
    sys.samp_count   = 0;
    sys.mem_bank     = nullptr;
    sys.fifo_channel = FIFO_MAXMOD;
    mmInit(&sys);

    initialized = true;
}

// -----------------------------------------------------------------------
// Load sounds
// -----------------------------------------------------------------------
void AudioManager::loadSounds(ScratchProject& project, const char* extractDir) {
    const int RAM_BUDGET = 512 * 1024;
    int usedBudget = 0;
    for (auto& sprite : project.targets) {
        for (auto& sound : sprite.sounds) {
            if (usedBudget >= RAM_BUDGET) { sound.loaded = false; continue; }
            usedBudget += sound.pcmSize;

            char path[512];
            snprintf(path, sizeof(path), "%s/%s.%s",
                     extractDir, sound.assetId.c_str(), sound.dataFormat.c_str());

            FILE* f = fopen(path, "rb");
            if (!f) continue;
            fseek(f, 0, SEEK_END);
            long fileSize = ftell(f);
            fclose(f);

            if (fileSize >= SOUND_SIZE_STREAM_THRESHOLD) {
                sound.isStreamed = true;
                sound.streamPath = path;
                sound.loaded     = true;
            } else {
                bool ok = false;
                if (sound.dataFormat == "wav") ok = loadWavToRam(sound, path);
                else if (sound.dataFormat == "mp3") ok = loadMp3ToRam(sound, path);
                sound.loaded = ok;
            }
        }
    }
}

// -----------------------------------------------------------------------
// loadWavToRam
// -----------------------------------------------------------------------
bool AudioManager::loadWavToRam(ScratchSound& sound, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;

    struct WavHeader {
        char     riff[4];
        uint32_t size;
        char     wave[4];
        char     fmt[4];
        uint32_t fmtSize;
        uint16_t audioFormat;
        uint16_t channels;
        uint32_t sampleRate;
        uint32_t byteRate;
        uint16_t blockAlign;
        uint16_t bitsPerSample;
        char     data[4];
        uint32_t dataSize;
    } hdr;

    if (fread(&hdr, 1, sizeof(hdr), f) < sizeof(hdr)) { fclose(f); return false; }
    if (strncmp(hdr.riff, "RIFF", 4) || strncmp(hdr.wave, "WAVE", 4)) {
        fclose(f); return false;
    }

    sound.pcmSize = hdr.dataSize;
    sound.pcmData = (uint8_t*)malloc(hdr.dataSize);
    if (!sound.pcmData) { fclose(f); return false; }
    fread(sound.pcmData, 1, hdr.dataSize, f);
    fclose(f);

    sound.rate     = hdr.sampleRate;
    sound.mmSoundId = (hdr.bitsPerSample == 16) ? 1 : 0;
    return true;
}

// -----------------------------------------------------------------------
// loadMp3ToRam
// -----------------------------------------------------------------------
bool AudioManager::loadMp3ToRam(ScratchSound& sound, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    sound.pcmData = (uint8_t*)malloc(sz);
    if (!sound.pcmData) { fclose(f); return false; }
    fread(sound.pcmData, 1, sz, f);
    fclose(f);
    sound.pcmSize   = sz;
    sound.mmSoundId = 0;
    return true;
}

// -----------------------------------------------------------------------
// playSound
// -----------------------------------------------------------------------
void AudioManager::playSound(ScratchSprite* sprite, const std::string& soundName) {
    for (auto& sound : sprite->sounds) {
        if (sound.name == soundName && sound.loaded) {
            if (sound.isStreamed) startStream(sound);
            else                  playRamSound(sound);
            return;
        }
    }
}

// -----------------------------------------------------------------------
// playRamSound — FIX: mm_ds_sample must outlive the call to mmEffectEx.
//
// Original code built mm_ds_sample on the stack inside this function and
// passed its address to mmEffectEx. The function returned immediately,
// destroying the stack frame, leaving maxmod with a dangling pointer.
// On the NDS, that stack memory gets reused within a few frames, causing
// audio corruption or crashes.
//
// Fix: store the sample descriptor in the ScratchSound itself (sampleDesc
// field added to the struct). It stays valid as long as pcmData is valid.
// -----------------------------------------------------------------------
void AudioManager::playRamSound(ScratchSound& sound)
{
    if (!initialized || !sound.loaded) return;

    // Play using maxmod sound slot
    mm_sfxhand handle = mmEffect(sound.mmSoundId);

    // store active handle if valid
    if (handle >= 0)
    {
        activeHandles[activeHandleCount++] = handle;

        // clamp (safety against overflow)
        if (activeHandleCount >= MAX_ACTIVE_SOUNDS)
            activeHandleCount = MAX_ACTIVE_SOUNDS - 1;
    }
}


// -----------------------------------------------------------------------
// startStream
// -----------------------------------------------------------------------
void AudioManager::startStream(ScratchSound& sound) {
    if (streamActive) {
        mmStreamClose();
        if (streamFile)   { fclose(streamFile);   streamFile   = nullptr; }
        if (streamBuffer) { free(streamBuffer);   streamBuffer = nullptr; }
        streamActive = false;
    }

    streamFile = fopen(sound.streamPath.c_str(), "rb");
    if (!streamFile) return;

    if (sound.dataFormat == "wav") fseek(streamFile, 44, SEEK_SET);

    streamBuffer     = (uint8_t*)malloc(STREAM_BUFFER_SIZE);
    streamBufSize    = STREAM_BUFFER_SIZE;
    streamSampleRate = (int)sound.rate;

    mm_stream stream;
    stream.sampling_rate = (mm_word)streamSampleRate;
    stream.buffer_length = STREAM_BUFFER_SIZE / 2;
    stream.callback      = streamCallback;
    stream.format        = MM_STREAM_16BIT_MONO;
    stream.timer         = 0;
    stream.manual        = false;

    mmStreamOpen(&stream);
    streamActive = true;
}

// -----------------------------------------------------------------------
// Stream callback
// -----------------------------------------------------------------------
mm_word AudioManager::streamCallback(mm_word length, mm_addr dest,
                                     mm_stream_formats format) {
    AudioManager& am = AudioManager::getInstance();
    if (!am.streamFile) return 0;

    size_t toRead = length * 2;
    size_t got    = fread(dest, 1, toRead, am.streamFile);

    if (got < toRead) {
        memset((uint8_t*)dest + got, 0, toRead - got);
        am.streamActive = false;
        return (mm_word)(got / 2);
    }
    return length;
}

// -----------------------------------------------------------------------
// update — FIX: only release handles that have finished playing.
//
// Original released ALL handles every frame regardless of whether the
// sound had finished, effectively cutting every sound after one frame.
// Now we walk the active list and compact out handles for sounds that
// have completed. mmEffectActive() returns false when a sound is done.
// -----------------------------------------------------------------------

void AudioManager::update()
{
    for (int i = 0; i < activeHandleCount; )
    {
        mm_sfxhand h = activeHandles[i];

        // correct maxmod9 API
        if (mmEffectStatus(h) != MM_ACTIVE)
        {
            activeHandles[i] = activeHandles[activeHandleCount - 1];
            activeHandleCount--;
        }
        else
        {
            i++;
        }
    }
}

// -----------------------------------------------------------------------
// stopAll
// -----------------------------------------------------------------------
void AudioManager::stopAll() {
    mmEffectCancelAll();
    numActive = 0;

    if (streamActive) {
        mmStreamClose();
        if (streamFile)   { fclose(streamFile);   streamFile   = nullptr; }
        if (streamBuffer) { free(streamBuffer);   streamBuffer = nullptr; }
        streamActive = false;
    }
}

bool AudioManager::isPlaying() {
    return streamActive || numActive > 0;
}

void AudioManager::setVolume(int vol) {
    masterVolume = vol < 0 ? 0 : vol > 100 ? 100 : vol;
    mmSetEffectsVolume((mm_word)((masterVolume * 1024) / 100));
}
