// =============================================================================
// audio_manager.cpp
// =============================================================================
#include "audio_manager.h"
#include <nds.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

// Single-header decoders (drop these into project):
// #define DR_WAV_IMPLEMENTATION
// #include "dr_wav.h"
// #define DR_MP3_IMPLEMENTATION
// #include "dr_mp3.h"
// For this implementation, we provide stubs that show the pattern.

#define STREAM_BUFFER_SIZE (8 * 1024)  // 8KB streaming chunks

// -----------------------------------------------------------------------
// Init maxmod
// -----------------------------------------------------------------------
void AudioManager::init() {
    initialized = false;
    numActive = 0;
    streamActive = false;
    streamFile = nullptr;
    streamBuffer = nullptr;
    masterVolume = 100;

    memset(activeHandles, 0, sizeof(activeHandles));

    // Init maxmod with no soundbank (we add sounds dynamically)
    mm_ds_system sys;
    sys.mod_count   = 0;
    sys.samp_count  = 0;
    sys.mem_bank    = nullptr;
    sys.fifo_channel = FIFO_MAXMOD;
    mmInit(&sys);

    initialized = true;
}

// -----------------------------------------------------------------------
// Load all sounds for all sprites
// -----------------------------------------------------------------------
void AudioManager::loadSounds(ScratchProject& project, const char* extractDir) {
    for (auto& sprite : project.targets) {
        for (auto& sound : sprite.sounds) {
            // Build file path: extractDir/<assetId>.<format>
            char path[512];
            snprintf(path, sizeof(path), "%s/%s.%s",
                     extractDir, sound.assetId.c_str(), sound.dataFormat.c_str());

            // Check file size to decide loading strategy
            FILE* f = fopen(path, "rb");
            if (!f) continue;
            fseek(f, 0, SEEK_END);
            long fileSize = ftell(f);
            fclose(f);

            if (fileSize >= SOUND_SIZE_STREAM_THRESHOLD) {
                // Large file: will stream from SD when played
                sound.isStreamed = true;
                sound.loaded = true;
                // Store path for later streaming
                sound.streamPath = path;
            } else {
                // Small file: decode to PCM in RAM
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
// Load WAV to RAM using dr_wav
// -----------------------------------------------------------------------
bool AudioManager::loadWavToRam(ScratchSound& sound, const char* path) {
    // Using dr_wav single-header library pattern:
    //   drwav wav;
    //   if (!drwav_init_file(&wav, path, nullptr)) return false;
    //   sound.pcmSize = wav.totalPCMFrameCount * wav.channels * sizeof(int16_t);
    //   sound.pcmData = (uint8_t*)malloc(sound.pcmSize);
    //   drwav_read_pcm_frames_s16(&wav, wav.totalPCMFrameCount, (int16_t*)sound.pcmData);
    //   sound.sampleRate = wav.sampleRate;
    //   drwav_uninit(&wav);

    // Minimal manual WAV header parser (44-byte RIFF header):
    FILE* f = fopen(path, "rb");
    if (!f) return false;

    struct WavHeader {
        char     riff[4];      // "RIFF"
        uint32_t size;
        char     wave[4];      // "WAVE"
        char     fmt[4];       // "fmt "
        uint32_t fmtSize;
        uint16_t audioFormat;  // 1=PCM
        uint16_t channels;
        uint32_t sampleRate;
        uint32_t byteRate;
        uint16_t blockAlign;
        uint16_t bitsPerSample;
        char     data[4];      // "data"
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
    // Register with maxmod as a sample
    mm_ds_sample samp;
    samp.data   = sound.pcmData;
    samp.length = hdr.dataSize / (hdr.bitsPerSample / 8);
    samp.loop   = false;
    samp.format = (hdr.bitsPerSample == 16) ? MM_SAMPLE_16BIT : MM_SAMPLE_8BIT;
    samp.rate   = hdr.sampleRate;

    // mmLoadEffect returns an ID we store for playback
    sound.mmSoundId = mmLoadEffect(&samp);
    return sound.mmSoundId >= 0;
}

// -----------------------------------------------------------------------
// Load MP3 to RAM via dr_mp3 (decode to PCM16)
// -----------------------------------------------------------------------
bool AudioManager::loadMp3ToRam(ScratchSound& sound, const char* path) {
    // Pattern with dr_mp3:
    //   drmp3 mp3;
    //   if (!drmp3_init_file(&mp3, path, nullptr)) return false;
    //   drmp3_uint64 frames = drmp3_get_pcm_frame_count(&mp3);
    //   sound.pcmData = (uint8_t*)malloc(frames * mp3.channels * 2);
    //   drmp3_read_pcm_frames_s16(&mp3, frames, (int16_t*)sound.pcmData);
    //   sound.pcmSize = frames * mp3.channels * 2;
    //   sound.rate = mp3.sampleRate;
    //   drmp3_uninit(&mp3);

    // For now, fall back to treating as 8-bit at declared rate
    // (full dr_mp3 integration requires including dr_mp3.h)
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    sound.pcmData = (uint8_t*)malloc(sz);
    if (!sound.pcmData) { fclose(f); return false; }
    fread(sound.pcmData, 1, sz, f);
    fclose(f);
    sound.pcmSize = sz;
    // Note: Without dr_mp3, this plays raw MP3 bytes as PCM (distorted).
    // Drop in dr_mp3.h and use the pattern above for real decoding.
    return true;
}

// -----------------------------------------------------------------------
// Play a sound by name from sprite's sound list
// -----------------------------------------------------------------------
void AudioManager::playSound(ScratchSprite* sprite, const std::string& soundName) {
    for (auto& sound : sprite->sounds) {
        if (sound.name == soundName && sound.loaded) {
            if (sound.isStreamed) {
                startStream(sound);
            } else {
                playRamSound(sound);
            }
            return;
        }
    }
}

// -----------------------------------------------------------------------
// Play a RAM-resident sound via maxmod effect
// -----------------------------------------------------------------------
void AudioManager::playRamSound(ScratchSound& sound) {
    if (sound.mmSoundId < 0) return;

    mm_sound_effect sfx;
    sfx.id     = sound.mmSoundId;
    sfx.rate   = 0x400; // normal rate
    sfx.handle = 0;
    sfx.volume = (masterVolume * 255) / 100;
    sfx.panning = 128; // center

    mm_sfxhand h = mmEffectEx(&sfx);
    if (numActive < MAX_CHANNELS) {
        activeHandles[numActive++] = h;
    }
}

// -----------------------------------------------------------------------
// Start streaming a large sound from SD
// -----------------------------------------------------------------------
void AudioManager::startStream(ScratchSound& sound) {
    if (streamActive && streamFile) {
        fclose(streamFile);
        mmStreamClose();
        streamActive = false;
    }
    if (streamBuffer) { free(streamBuffer); streamBuffer = nullptr; }

    streamFile = fopen(sound.streamPath.c_str(), "rb");
    if (!streamFile) return;

    // Skip WAV header (44 bytes) if WAV
    if (sound.dataFormat == "wav") fseek(streamFile, 44, SEEK_SET);

    streamBuffer = (uint8_t*)malloc(STREAM_BUFFER_SIZE);
    streamBufSize = STREAM_BUFFER_SIZE;
    streamSampleRate = (int)sound.rate;

    // Setup maxmod stream
    mm_stream stream;
    stream.sampling_rate = streamSampleRate;
    stream.buffer_length = STREAM_BUFFER_SIZE / 2; // in 16-bit samples
    stream.callback      = streamCallback;          // fill buffer callback
    stream.format        = MM_STREAM_16BIT_MONO;
    stream.timer         = MM_TIMER0;
    stream.manual        = false;

    mmStreamOpen(&stream);
    mmStreamBegin();
    streamActive = true;
}

// maxmod stream callback (static — fills buffer from SD)
mm_word AudioManager::streamCallback(mm_word length, mm_addr dest,
                                      mm_stream_formats format) {
    AudioManager& am = AudioManager::getInstance();
    if (!am.streamFile) return 0;

    size_t toRead = length * 2; // 16-bit mono
    size_t got = fread(dest, 1, toRead, am.streamFile);

    if (got < toRead) {
        // End of file — stop stream
        memset((uint8_t*)dest + got, 0, toRead - got);
        am.streamActive = false;
        return got / 2;
    }
    return length;
}

// -----------------------------------------------------------------------
// Update — tick maxmod and clean up finished effects
// -----------------------------------------------------------------------
void AudioManager::update() {
    mmFrame();

    // Remove finished handles
    int j = 0;
    for (int i = 0; i < numActive; i++) {
        if (mmEffectActive(activeHandles[i])) {
            activeHandles[j++] = activeHandles[i];
        }
    }
    numActive = j;
}

// -----------------------------------------------------------------------
// Stop all sounds
// -----------------------------------------------------------------------
void AudioManager::stopAll() {
    for (int i = 0; i < numActive; i++) {
        mmEffectCancel(activeHandles[i]);
    }
    numActive = 0;

    if (streamActive) {
        mmStreamClose();
        if (streamFile) { fclose(streamFile); streamFile = nullptr; }
        if (streamBuffer) { free(streamBuffer); streamBuffer = nullptr; }
        streamActive = false;
    }
}

bool AudioManager::isPlaying() {
    if (streamActive) return true;
    return numActive > 0;
}

void AudioManager::setVolume(int vol) {
    masterVolume = vol;
    if (masterVolume < 0) masterVolume = 0;
    if (masterVolume > 100) masterVolume = 100;
}
