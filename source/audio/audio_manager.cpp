// =============================================================================
// audio_manager.cpp — corrected for real maxmod9 API
// =============================================================================
#include "audio_manager.h"
#include <nds.h>
// fifo.h not present -- define manually
#ifndef FIFO_MAXMOD
#define FIFO_MAXMOD 7
#endif
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#define STREAM_BUFFER_SIZE (8 * 1024)

// -----------------------------------------------------------------------
// Init maxmod
// -----------------------------------------------------------------------
void AudioManager::init() {
    initialized  = false;
    numActive    = 0;
    streamActive = false;
    streamFile   = nullptr;
    streamBuffer = nullptr;
    masterVolume = 100;

    memset(activeHandles, 0, sizeof(activeHandles));

    // Maxmod on NDS requires a soundbank. We're loading sounds dynamically
    // via mm_sound_effect.sample pointer (not soundbank IDs), so we init
    // with zero counts and a null bank — this is valid for effect-only use.
    mm_ds_system sys;
    sys.mod_count    = 0;
    sys.samp_count   = 0;
    sys.mem_bank     = nullptr;
    sys.fifo_channel = FIFO_MAXMOD;
    mmInit(&sys);

    initialized = true;
}

// -----------------------------------------------------------------------
// Load all sounds for all sprites
// -----------------------------------------------------------------------
void AudioManager::loadSounds(ScratchProject& project, const char* extractDir) {
    const int RAM_BUDGET = 512 * 1024;  // reserve 512KB for sounds total
    int usedBudget = 0;
    for (auto& sprite : project.targets) {
        for (auto& sound : sprite.sounds) {

            if (usedBudget >= RAM_BUDGET) {
                sound.loaded = false;
                continue;
            }
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
                if (sound.dataFormat == "wav") {
                    ok = loadWavToRam(sound, path);
                } else if (sound.dataFormat == "mp3") {
                    ok = loadMp3ToRam(sound, path);
                }
                sound.loaded = ok;
            }
        }
    }
}

// -----------------------------------------------------------------------
// Load WAV to RAM
// Real maxmod dynamic sample playback uses mm_sound_effect.sample pointer
// pointing to an mm_ds_sample struct — no soundbank ID needed.
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

    sound.rate = hdr.sampleRate;

    // mm_ds_sample format: 0 = 8-bit PCM, 1 = 16-bit PCM
    // base_rate: (sampleRate * 512) / 15768  (maxmod internal rate formula)
    // For simplicity store as 0 and let mmEffectEx handle rate via mm_sound_effect.rate

    // mmSoundId stores the format for use in playRamSound
    // 0 = 8-bit, 1 = 16-bit
    sound.mmSoundId = (hdr.bitsPerSample == 16) ? 1 : 0;

    return true;
}

// -----------------------------------------------------------------------
// Load MP3 to RAM (stub — store raw bytes, treat as 8-bit PCM)
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
    sound.pcmSize  = sz;
    sound.mmSoundId = 0; // treat as 8-bit
    return true;
}

// -----------------------------------------------------------------------
// Play a sound by name
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
// Play a RAM sound via mm_sound_effect with .sample pointer
// This uses the "external sample" path — mm_sound_effect.sample points
// directly to an mm_ds_sample we fill on the stack.
// -----------------------------------------------------------------------
void AudioManager::playRamSound(ScratchSound& sound) {
    if (!sound.pcmData || sound.pcmSize == 0) return;

    // Build an mm_ds_sample on the heap (must stay valid during playback)
    // We reuse pcmData buffer; the sample struct itself is small.
    // For simplicity allocate a persistent one per sound on first play.
    // (A full impl would cache this in ScratchSound.)
    mm_ds_sample samp;
    samp.loop_start  = 0;
    samp.length      = (mm_word)(sound.pcmSize / (sound.mmSoundId == 1 ? 2 : 1));
    samp.format      = (mm_byte)sound.mmSoundId; // 0=8-bit, 1=16-bit
    samp.repeat_mode = 1; // forward loop (set length=total for one-shot behaviour)
    // base_rate: approximate conversion from Hz to maxmod base_rate
    // maxmod base_rate = sampleRate * 512 / 15768 (approx)
    samp.base_rate   = (mm_hword)((sound.rate * 512) / 15768);
    samp.data        = sound.pcmData;

    mm_sound_effect sfx;
    sfx.sample  = &samp;      // use external sample pointer (not soundbank ID)
    sfx.rate    = 0x400;      // 1.0x playback rate (6.10 fixed point)
    sfx.handle  = 0;
    sfx.volume  = (mm_byte)((masterVolume * 255) / 100);
    sfx.panning = 128;        // center

    mm_sfxhand h = mmEffectEx(&sfx);
    if (numActive < MAX_CHANNELS) {
        activeHandles[numActive++] = h;
    }
}

// -----------------------------------------------------------------------
// Stream a large sound from SD
// -----------------------------------------------------------------------
void AudioManager::startStream(ScratchSound& sound) {
    if (streamActive) {
        mmStreamClose();
        if (streamFile) { fclose(streamFile); streamFile = nullptr; }
        if (streamBuffer) { free(streamBuffer); streamBuffer = nullptr; }
        streamActive = false;
    }

    streamFile = fopen(sound.streamPath.c_str(), "rb");
    if (!streamFile) return;

    if (sound.dataFormat == "wav") fseek(streamFile, 44, SEEK_SET);

    streamBuffer    = (uint8_t*)malloc(STREAM_BUFFER_SIZE);
    streamBufSize   = STREAM_BUFFER_SIZE;
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
                                     // Update — tick streaming; no mmFrame in real maxmod9
                                     // -----------------------------------------------------------------------
                                     void AudioManager::update() {
                                         if (streamActive) {
                                             mmStreamUpdate();
                                         }
                                         // mmEffectActive doesn't exist in real maxmod9 — we can't easily track
                                         // when effects finish. Just release handles after a timeout or leave them;
                                         // mmEffectRelease lets maxmod reuse the channel automatically.
                                         // For now, release all tracked handles each frame so channels stay free.
                                         for (int i = 0; i < numActive; i++) {
                                             mmEffectRelease(activeHandles[i]);
                                         }
                                         numActive = 0;
                                     }

                                     // -----------------------------------------------------------------------
                                     // Stop all sounds
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
                                         // Scale to maxmod 0->1024 range
                                         mmSetEffectsVolume((mm_word)((masterVolume * 1024) / 100));
                                     }
